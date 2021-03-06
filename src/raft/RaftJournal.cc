// ----------------------------------------------------------------------
// File: RaftJournal.cc
// Author: Georgios Bitzes - CERN
// ----------------------------------------------------------------------

/************************************************************************
 * quarkdb - a redis-like highly available key-value store              *
 * Copyright (C) 2016 CERN/Switzerland                                  *
 *                                                                      *
 * This program is free software: you can redistribute it and/or modify *
 * it under the terms of the GNU General Public License as published by *
 * the Free Software Foundation, either version 3 of the License, or    *
 * (at your option) any later version.                                  *
 *                                                                      *
 * This program is distributed in the hope that it will be useful,      *
 * but WITHOUT ANY WARRANTY; without even the implied warranty of       *
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the        *
 * GNU General Public License for more details.                         *
 *                                                                      *
 * You should have received a copy of the GNU General Public License    *
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.*
 ************************************************************************/

#include "raft/RaftJournal.hh"
#include "raft/RaftMembers.hh"
#include "storage/KeyConstants.hh"
#include "Common.hh"
#include "Utils.hh"
#include "utils/IntToBinaryString.hh"
#include "utils/StaticBuffer.hh"
#include "utils/StringUtils.hh"
#include "../deps/StringMatchLen.h"
#include "raft/RaftState.hh"

#include <algorithm>
#include <rocksdb/utilities/checkpoint.h>
#include <rocksdb/filter_policy.h>
#include <rocksdb/table.h>

using namespace quarkdb;
#define THROW_ON_ERROR(stmt) { rocksdb::Status st2 = stmt; if(!st2.ok()) qdb_throw(st2.ToString()); }

//------------------------------------------------------------------------------
// Helper functions
//------------------------------------------------------------------------------

constexpr size_t kEntryKeySize = 1 + sizeof(LogIndex);
using KeyBuffer = StaticBuffer<kEntryKeySize>;

static std::string encodeEntryKey(LogIndex index) {
  return SSTR("E" << intToBinaryString(index));
}

static bool parseEntryKey(std::string_view key, LogIndex &index) {
  if(key.size() != 1 + sizeof(index)) {
    return false;
  }

  if(key[0] != 'E') {
    return false;
  }

  index = binaryStringToInt(key.data() + 1);
  return true;
}

QDB_ALWAYS_INLINE
inline void encodeEntryKey(LogIndex index, KeyBuffer &key) {
  key.data()[0] = 'E';
  intToBinaryString(index, key.data()+1);
}

//------------------------------------------------------------------------------
// Initialize fsync policy, if not already. Ensures compatibility with
// pre-0.4.1 versions of QuarkDB.
//------------------------------------------------------------------------------
void RaftJournal::ensureFsyncPolicyInitialized() {
  std::string tmp;
  rocksdb::Status st = db->Get(rocksdb::ReadOptions(), KeyConstants::kJournal_FsyncPolicy, &tmp);

  if(!st.ok() && !st.IsNotFound()) {
    qdb_throw(st.ToString());
  }

  if(st.ok()) {
    return;
  }

  this->set_or_die(KeyConstants::kJournal_FsyncPolicy, fsyncPolicyToString(FsyncPolicy::kSyncImportantUpdates));
}

//------------------------------------------------------------------------------
// Should we sync this write?
//------------------------------------------------------------------------------
bool RaftJournal::shouldSync(bool important) {
  if(fsyncPolicy == FsyncPolicy::kAlways) {
    return true;
  }

  if(fsyncPolicy == FsyncPolicy::kAsync) {
    return false;
  }

  qdb_assert(fsyncPolicy == FsyncPolicy::kSyncImportantUpdates);
  return important;
}

//------------------------------------------------------------------------------
// RaftJournal
//------------------------------------------------------------------------------
void RaftJournal::ObliterateAndReinitializeJournal(const std::string &path, RaftClusterID clusterID, std::vector<RaftServer> nodes, LogIndex startIndex, FsyncPolicy fsyncPolicy) {
  RaftJournal journal(path, clusterID, nodes, startIndex, fsyncPolicy);
}

void RaftJournal::obliterate(RaftClusterID newClusterID, const std::vector<RaftServer> &newNodes, LogIndex startIndex, FsyncPolicy fsyncPolicy) {
  IteratorPtr iter(db->NewIterator(rocksdb::ReadOptions()));
  for(iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    db->Delete(rocksdb::WriteOptions(), iter->key().ToString());
  }

  this->set_int_or_die(KeyConstants::kJournal_CurrentTerm, 0);
  this->set_int_or_die(KeyConstants::kJournal_LogSize, startIndex+1);
  this->set_int_or_die(KeyConstants::kJournal_LogStart, startIndex);
  this->set_or_die(KeyConstants::kJournal_ClusterID, newClusterID);
  this->set_or_die(KeyConstants::kJournal_VotedFor, "");
  this->set_int_or_die(KeyConstants::kJournal_CommitIndex, startIndex);

  RaftMembers newMembers(newNodes, {});
  this->set_or_die(KeyConstants::kJournal_Members, newMembers.toString());
  this->set_int_or_die(KeyConstants::kJournal_MembershipEpoch, startIndex);
  this->set_or_die(KeyConstants::kJournal_FsyncPolicy, fsyncPolicyToString(fsyncPolicy) );

  RaftEntry entry(0, "JOURNAL_UPDATE_MEMBERS", newMembers.toString(), newClusterID);
  this->set_or_die(encodeEntryKey(startIndex), entry.serialize());

  initialize();
}

void RaftJournal::initializeFsyncPolicy() {
  std::string policyStr = this->get_or_die(KeyConstants::kJournal_FsyncPolicy);
  FsyncPolicy tmp = FsyncPolicy::kSyncImportantUpdates;

  if(!parseFsyncPolicy(policyStr, tmp)) {
    qdb_critical("Invalid fsync policy in journal: " << policyStr);
  }

  fsyncPolicy = tmp;
}

void RaftJournal::initialize() {
  currentTerm = this->get_int_or_die(KeyConstants::kJournal_CurrentTerm);
  logSize = this->get_int_or_die(KeyConstants::kJournal_LogSize);
  logStart = this->get_int_or_die(KeyConstants::kJournal_LogStart);
  clusterID = this->get_or_die(KeyConstants::kJournal_ClusterID);
  commitIndex = this->get_int_or_die(KeyConstants::kJournal_CommitIndex);
  std::string vote = this->get_or_die(KeyConstants::kJournal_VotedFor);
  this->fetch_or_die(logSize-1, termOfLastEntry);

  membershipEpoch = this->get_int_or_die(KeyConstants::kJournal_MembershipEpoch);
  members = RaftMembers(this->get_or_die(KeyConstants::kJournal_Members));
  initializeFsyncPolicy();

  if(!vote.empty() && !parseServer(vote, votedFor)) {
    qdb_throw("journal corruption, cannot parse " << KeyConstants::kJournal_VotedFor << ": " << vote);
  }

  fsyncThread.reset(new FsyncThread(db, std::chrono::seconds(1)));
}

void RaftJournal::openDB(const std::string &path) {
  qdb_info("Opening raft journal " << quotes(path));
  dbPath = path;

  rocksdb::Options options;
  rocksdb::BlockBasedTableOptions table_options;
  table_options.filter_policy.reset(rocksdb::NewBloomFilterPolicy(10, false));
  table_options.block_size = 16 * 1024;

  options.compression = rocksdb::kNoCompression;
  options.bottommost_compression = rocksdb::kNoCompression;

  options.table_factory.reset(rocksdb::NewBlockBasedTableFactory(table_options));
  options.create_if_missing = true;
  options.max_manifest_file_size = 1024 * 1024;

  // Warn on write stalls
  writeStallWarner.reset(new WriteStallWarner("raft-journal"));
  options.listeners.emplace_back(writeStallWarner);

  rocksdb::Status status = rocksdb::DB::Open(options, path, &db);
  if(!status.ok()) qdb_throw("Error while opening journal in " << path << ":" << status.ToString());
}

RaftJournal::RaftJournal(const std::string &filename, RaftClusterID clusterID, const std::vector<RaftServer> &nodes, LogIndex startIndex, FsyncPolicy fsyncPolicy) {
  openDB(filename);
  obliterate(clusterID, nodes, startIndex, fsyncPolicy);
}

RaftJournal::~RaftJournal() {
  qdb_info("Closing raft journal " << quotes(dbPath));
  fsyncThread.reset();

  if(db) {
    delete db;
    db = nullptr;
  }
}

RaftJournal::RaftJournal(const std::string &filename) {
  openDB(filename);
  ensureFsyncPolicyInitialized();
  initialize();
}

bool RaftJournal::setCurrentTerm(RaftTerm term, RaftServer vote) {
  std::scoped_lock lock(currentTermMutex);

  //----------------------------------------------------------------------------
  // Terms should never go back in time
  //----------------------------------------------------------------------------

  if(term < currentTerm) {
    return false;
  }

  //----------------------------------------------------------------------------
  // The vote for the current term should never change
  //----------------------------------------------------------------------------

  if(term == currentTerm && !votedFor.empty()) {
    return false;
  }

  //----------------------------------------------------------------------------
  // Atomically update currentTerm and votedFor
  //----------------------------------------------------------------------------
  rocksdb::WriteBatch batch;
  THROW_ON_ERROR(batch.Put(KeyConstants::kJournal_CurrentTerm, intToBinaryString(term)));
  THROW_ON_ERROR(batch.Put(KeyConstants::kJournal_VotedFor, vote.toString()));
  commitBatch(batch, -1, true);

  currentTerm = term;
  votedFor = vote;
  return true;
}

bool RaftJournal::simulateDataLoss(size_t numberOfEntries) {
  LogIndex newLogSize = logSize - numberOfEntries;

  if(newLogSize <= commitIndex) {
    rawSetCommitIndex(newLogSize-1);
  }

  return removeEntries(newLogSize);
}

bool RaftJournal::setCommitIndex(LogIndex newIndex) {
  std::scoped_lock lock(commitIndexMutex);
  if(newIndex < commitIndex) {
    qdb_warn("attempted to set commit index in the past, from " << commitIndex << " ==> " << newIndex);
    return false;
  }

  if(logSize <= newIndex) {
    qdb_throw("attempted to mark as committed a non-existing entry. Journal size: " << logSize << ", new index: " << newIndex);
  }

  if(commitIndex < newIndex) {
    rawSetCommitIndex(newIndex);
  }
  return true;
}

void RaftJournal::rawSetCommitIndex(LogIndex newIndex) {
  this->set_int_or_die(KeyConstants::kJournal_CommitIndex, newIndex);
  commitIndex = newIndex;
  commitNotifier.notify_all();
}

bool RaftJournal::waitForCommits(const LogIndex currentCommit) {
  std::unique_lock<std::mutex> lock(commitIndexMutex);
  if(currentCommit < commitIndex) return true;

  commitNotifier.wait(lock);
  return true;
}

void RaftJournal::commitBatch(rocksdb::WriteBatch &batch, LogIndex index, bool important) {
  if(index >= 0 && index <= commitIndex) {
    qdb_throw("Attempted to remove committed entries by setting logSize to " << index << " while commitIndex = " << commitIndex);
  }

  if(index >= 0 && index != logSize) {
    THROW_ON_ERROR(batch.Put(KeyConstants::kJournal_LogSize, intToBinaryString(index)));
  }

  rocksdb::WriteOptions opts;
  opts.sync = shouldSync(important);

  rocksdb::Status st = db->Write(opts, &batch);
  if(!st.ok()) qdb_throw("unable to commit journal transaction: " << st.ToString());
  if(index >= 0) logSize = index;
}

RaftMembers RaftJournal::getMembers() {
  std::scoped_lock lock(membersMutex);
  return members;
}

RaftMembership RaftJournal::getMembership() {
  std::scoped_lock lock(membersMutex);
  return {members.nodes, members.observers, membershipEpoch};
}

bool RaftJournal::membershipUpdate(RaftTerm term, const RaftMembers &newMembers, std::string &err) {
  std::scoped_lock lock(contentMutex);

  if(commitIndex < membershipEpoch) {
    err = SSTR("the current membership epoch has not been committed yet: " << membershipEpoch);
    return false;
  }

  RaftEntry entry(term, "JOURNAL_UPDATE_MEMBERS", newMembers.toString(), clusterID);
  return appendNoLock(logSize, entry, true);
}

bool RaftJournal::addObserver(RaftTerm term, const RaftServer &observer, std::string &err) {
  RaftMembers newMembers = getMembers();
  if(!newMembers.addObserver(observer, err)) return false;
  return membershipUpdate(term, newMembers, err);
}

bool RaftJournal::removeMember(RaftTerm term, const RaftServer &member, std::string &err) {
  RaftMembers newMembers = getMembers();
  if(!newMembers.removeMember(member, err)) return false;
  return membershipUpdate(term, newMembers, err);
}

bool RaftJournal::promoteObserver(RaftTerm term, const RaftServer &observer, std::string &err) {
  RaftMembers newMembers = getMembers();
  if(!newMembers.promoteObserver(observer, err)) return false;
  return membershipUpdate(term, newMembers, err);
}

bool RaftJournal::demoteToObserver(RaftTerm term, const RaftServer &member, std::string &err) {
  RaftMembers newMembers = getMembers();
  if(!newMembers.demoteToObserver(member, err)) return false;
  return membershipUpdate(term, newMembers, err);
}

bool RaftJournal::appendNoLock(LogIndex index, const RaftEntry &entry, bool important) {
  if(index != logSize) {
    qdb_warn("attempted to insert journal entry at an invalid position. index = " << index << ", logSize = " << logSize);
    return false;
  }

  if(entry.term > currentTerm) {
    qdb_warn("attempted to insert journal entry with a higher term than the current one: " << entry.term << " vs " << currentTerm);
    return false;
  }

  if(entry.term < termOfLastEntry) {
    qdb_warn("attempted to insert journal entry with lower term " << entry.term << ", while last one is " << termOfLastEntry);
    return false;
  }

  rocksdb::WriteBatch batch;

  if(entry.request[0] == "JOURNAL_UPDATE_MEMBERS") {
    if(entry.request.size() != 3) qdb_throw("Journal corruption, invalid journal_update_members: " << entry.request);

    //--------------------------------------------------------------------------
    // Special case for membership updates
    // We don't wait until the entry is committed, and it takes effect
    // immediatelly.
    // The commit applier will ignore such entries, and apply a no-op to the
    // state machine.
    //--------------------------------------------------------------------------

    if(entry.request[2] == clusterID) {
      THROW_ON_ERROR(batch.Put(KeyConstants::kJournal_Members, entry.request[1]));
      THROW_ON_ERROR(batch.Put(KeyConstants::kJournal_MembershipEpoch, intToBinaryString(index)));

      THROW_ON_ERROR(batch.Put(KeyConstants::kJournal_PreviousMembers, members.toString()));
      THROW_ON_ERROR(batch.Put(KeyConstants::kJournal_PreviousMembershipEpoch, intToBinaryString(membershipEpoch)));

      qdb_event("Transitioning into a new membership epoch: " << membershipEpoch << " => " << index
      << ". Old members: " << members.toString() << ", new members: " << entry.request[1]);

      std::scoped_lock lock(membersMutex);
      members = RaftMembers(entry.request[1]);
      membershipEpoch = index;
    }
    else {
      qdb_critical("Received request for membership update " << entry.request << ", but the clusterIDs do not match - mine is " << clusterID
      << ". THE MEMBERSHIP UPDATE ENTRY WILL BE IGNORED. Something is either corrupted or you force-reconfigured " <<
      " the nodes recently - if it's the latter, this message is nothing to worry about.");
    }

    important = true;
  }

  KeyBuffer keyBuffer;
  encodeEntryKey(index, keyBuffer);
  THROW_ON_ERROR(batch.Put(keyBuffer.toView(), entry.serialize()));

  commitBatch(batch, index+1, important);

  termOfLastEntry = entry.term;
  logUpdated.notify_all();
  return true;
}

bool RaftJournal::append(LogIndex index, const RaftEntry &entry, bool important) {
  std::scoped_lock lock(contentMutex);
  return appendNoLock(index, entry, important);
}

bool RaftJournal::appendLeadershipMarker(LogIndex index, RaftTerm term, const RaftServer &leader) {
  return append(index, RaftEntry(term, "JOURNAL_LEADERSHIP_MARKER", SSTR(term), leader.toString()), true);
}

void RaftJournal::setFsyncPolicy(FsyncPolicy pol) {
  std::unique_lock lock(fsyncPolicyMutex);

  if(fsyncPolicy != pol) {
    this->set_or_die(KeyConstants::kJournal_FsyncPolicy, fsyncPolicyToString(pol));
    fsyncPolicy = pol;
  }
}

FsyncPolicy RaftJournal::getFsyncPolicy() {
  return fsyncPolicy;
}

void RaftJournal::trimUntil(LogIndex newLogStart) {
  // no locking - trimmed entries should be so old
  // that they are not being accessed anymore

  if(newLogStart <= logStart) return; // no entries to trim
  if(logSize < newLogStart) qdb_throw("attempted to trim a journal past its end. logSize: " << logSize << ", new log start: " << newLogStart);
  if(commitIndex < newLogStart) qdb_throw("attempted to trim non-committed entries. commitIndex: " << commitIndex << ", new log start: " << newLogStart);

  qdb_info("Trimming raft journal from #" << logStart << " until #" << newLogStart);
  rocksdb::WriteBatch batch;

  for(LogIndex i = logStart; i < newLogStart; i++) {
    THROW_ON_ERROR(batch.Delete(encodeEntryKey(i)));
  }

  THROW_ON_ERROR(batch.Put(KeyConstants::kJournal_LogStart, intToBinaryString(newLogStart)));
  commitBatch(batch);
  logStart = newLogStart;
}

RaftServer RaftJournal::getVotedFor() {
  std::scoped_lock lock(votedForMutex);
  return votedFor;
}

std::vector<RaftServer> RaftJournal::getNodes() {
  return getMembership().nodes;
}

void RaftJournal::notifyWaitingThreads() {
  logUpdated.notify_all();
  commitNotifier.notify_all();
}

void RaftJournal::waitForUpdates(LogIndex currentSize, const std::chrono::milliseconds &timeout) {
  std::unique_lock<std::mutex> lock(contentMutex);

  // race, there's an update already
  if(currentSize < logSize) return;

  logUpdated.wait_for(lock, timeout);
}

bool RaftJournal::removeEntries(LogIndex from) {
  std::unique_lock<std::mutex> lock(contentMutex);
  if(logSize <= from) return false;

  if(from <= commitIndex) qdb_throw("attempted to remove committed entries. commitIndex: " << commitIndex << ", from: " << from);
  qdb_warn("Removing inconsistent log entries: [" << from << "," << logSize-1 << "]");

  rocksdb::WriteBatch batch;
  for(LogIndex i = from; i < logSize; i++) {
    THROW_ON_ERROR(batch.Delete(encodeEntryKey(i)));
  }

  //----------------------------------------------------------------------------
  // Membership epochs take effect immediatelly, without waiting for the entries
  // to be committed. (as per the Raft PhD thesis)
  // This means that an uncommitted membership epoch can be theoretically rolled
  // back.
  // This should be extremely uncommon, so we log a critical message.
  //----------------------------------------------------------------------------

  if(from <= membershipEpoch) {
    std::scoped_lock lock2(membersMutex);

    LogIndex previousMembershipEpoch = this->get_int_or_die(KeyConstants::kJournal_PreviousMembershipEpoch);
    std::string previousMembers = this->get_or_die(KeyConstants::kJournal_PreviousMembers);

    THROW_ON_ERROR(batch.Put(KeyConstants::kJournal_MembershipEpoch, intToBinaryString(previousMembershipEpoch)));
    THROW_ON_ERROR(batch.Put(KeyConstants::kJournal_Members, previousMembers));

    qdb_critical("Rolling back an uncommitted membership epoch. Transitioning from " <<
    membershipEpoch << " => " << previousMembershipEpoch << ". Old members: " << members.toString() <<
    ", new members: " << previousMembers);

    members = RaftMembers(previousMembers);
    membershipEpoch = previousMembershipEpoch;
  }

  commitBatch(batch, from);
  fetch_or_die(from-1, termOfLastEntry);
  return true;
}

// return the first entry which is not identical to the ones in the vector
LogIndex RaftJournal::compareEntries(LogIndex start, const std::vector<RaftEntry> entries) {
  std::scoped_lock lock(contentMutex);

  LogIndex endIndex = std::min(LogIndex(logSize), LogIndex(start+entries.size()));
  LogIndex startIndex = std::max(start, LogIndex(logStart));

  if(start != startIndex) {
    qdb_critical("Tried to compare entries which have already been trimmed.. will assume they contain no inconsistencies. logStart: " << logStart << ", asked to compare starting from: " << start);
  }

  for(LogIndex i = startIndex; i < endIndex; i++) {
    RaftEntry entry;
    fetch_or_die(i, entry);
    if(entries[i-start] != entry) {
      qdb_warn("Detected inconsistency for entry #" << i << ". Contents of my journal: " << entry << ". Contents of what the leader sent: " << entries[i-start]);
      return i;
    }
  }

  return endIndex;
}

bool RaftJournal::matchEntries(LogIndex index, RaftTerm term) {
  std::scoped_lock lock(contentMutex);

  if(logSize <= index) {
    return false;
  }

  RaftTerm tr;
  rocksdb::Status status = this->fetch(index, tr);

  if(!status.ok() && !status.IsNotFound()) {
    qdb_throw("rocksdb error: " << status.ToString());
  }

  return status.ok() && tr == term;
}

//------------------------------------------------------------------------------
// Log entry fetch operations
//------------------------------------------------------------------------------

rocksdb::Status RaftJournal::fetch(LogIndex index, RaftEntry &entry) {
  // we intentionally do not check logSize and logStart, so as to be able to
  // catch potential inconsistencies between the counters and what is
  // really contained in the journal

  std::string data;
  rocksdb::Status st = db->Get(rocksdb::ReadOptions(), encodeEntryKey(index), &data);
  if(!st.ok()) return st;

  RaftEntry::deserialize(entry, data);
  return st;
}

rocksdb::Status RaftJournal::fetch(LogIndex index, RaftTerm &term) {
  RaftEntry entry;
  rocksdb::Status st = fetch(index, entry);
  term = entry.term;
  return st;
}

rocksdb::Status RaftJournal::fetch(LogIndex index, RaftSerializedEntry &data) {
  return db->Get(rocksdb::ReadOptions(), encodeEntryKey(index), &data);
}

void RaftJournal::fetch_last(int last, std::vector<RaftEntry> &entries) {
  LogIndex endIndex = logSize;
  LogIndex startIndex = endIndex - last;
  if(startIndex < 0) startIndex = 0;

  for(LogIndex index = startIndex; index < endIndex; index++) {
    RaftEntry entry;
    fetch(index, entry);
    entries.emplace_back(entry);
  }
}

void RaftJournal::fetch_or_die(LogIndex index, RaftEntry &entry) {
  rocksdb::Status st = fetch(index, entry);
  if(!st.ok()) {
    throw FatalException(SSTR("unable to fetch entry with index " << index));
  }
}

void RaftJournal::fetch_or_die(LogIndex index, RaftTerm &term) {
  rocksdb::Status st = fetch(index, term);
  if(!st.ok()) {
    qdb_throw("unable to fetch entry with index " << index);
  }
}

void RaftJournal::set_or_die(const std::string &key, const std::string &value) {
  rocksdb::Status st = db->Put(rocksdb::WriteOptions(), key, value);
  if(!st.ok()) {
    qdb_throw("unable to set journal key " << key << ". Error: " << st.ToString());
  }
}

void RaftJournal::set_int_or_die(const std::string &key, int64_t value) {
  this->set_or_die(key, intToBinaryString(value));
}

int64_t RaftJournal::get_int_or_die(const std::string &key) {
  return binaryStringToInt(this->get_or_die(key).c_str());
}

std::string RaftJournal::get_or_die(const std::string &key) {
  std::string tmp;
  rocksdb::Status st = db->Get(rocksdb::ReadOptions(), key, &tmp);
  if(!st.ok()) qdb_throw("error when getting journal key " << key << ": " << st.ToString());
  return tmp;
}

//------------------------------------------------------------------------------
// Checkpoint for online backup
//------------------------------------------------------------------------------
rocksdb::Status RaftJournal::checkpoint(const std::string &path) {
  rocksdb::Checkpoint *checkpoint = nullptr;
  rocksdb::Status st = rocksdb::Checkpoint::Create(db, &checkpoint);
  if(!st.ok()) return st;

  st = checkpoint->CreateCheckpoint(path);
  delete checkpoint;

  return st;
}

//------------------------------------------------------------------------------
// Scan through the contents of the journal, starting from the given index
//------------------------------------------------------------------------------
rocksdb::Status RaftJournal::scanContents(LogIndex startingPoint, size_t count, std::string_view match, std::vector<RaftEntryWithIndex> &out, LogIndex &nextCursor) {
  out.clear();
  RaftJournal::Iterator iter = getIterator(startingPoint, false);

  for(size_t i = 0; i < count; i++) {
    if(!iter.valid()) {
      break;
    }

    RaftSerializedEntry item;
    iter.current(item);

    if(match.empty() || stringmatchlen(match.data(), match.length(), item.data(), item.length(), 0) == 1) {
      RaftEntry entry;
      RaftEntry::deserialize(entry, item);
      out.emplace_back(entry, iter.getCurrentIndex());
    }

    iter.next();
  }

  if(!iter.valid()) {
    nextCursor = 0;
  }
  else {
    nextCursor = iter.getCurrentIndex();
  }

  return rocksdb::Status::OK();
}

//------------------------------------------------------------------------------
// Trigger manual compaction of the journal
//------------------------------------------------------------------------------
rocksdb::Status RaftJournal::manualCompaction() {
  qdb_event("Triggering manual journal compaction.. auto-compaction will be disabled while the manual one is running.");
  // Disabling auto-compactions is a hack to prevent write-stalling. Pending compaction
  // bytes will jump to the total size of the DB as soon as a manual compaction is
  // issued, which will most likely stall or completely stop writes for a long time.
  // (depends on the size of the DB)
  // This is a recommendation by rocksdb devs as a workaround: Disabling auto
  // compactions will disable write-stalling as well.
  THROW_ON_ERROR(db->SetOptions( { {"disable_auto_compactions", "true"} } ));

  rocksdb::CompactRangeOptions opts;
  opts.bottommost_level_compaction = rocksdb::BottommostLevelCompaction::kForce;

  rocksdb::Status st = db->CompactRange(opts, nullptr, nullptr);

  THROW_ON_ERROR(db->SetOptions( { {"disable_auto_compactions", "false"} } ));

  qdb_event("Manual journal compaction has completed with status " << st.ToString());
  return st;
}

//------------------------------------------------------------------------------
// Iterator
//------------------------------------------------------------------------------
RaftJournal::Iterator RaftJournal::getIterator(LogIndex startingPoint, bool mustMatchStartingPoint) {
  rocksdb::ReadOptions readOpts;
  readOpts.total_order_seek = true;

  std::unique_ptr<rocksdb::Iterator> it;
  it.reset(db->NewIterator(readOpts));

  return RaftJournal::Iterator(std::move(it), startingPoint, mustMatchStartingPoint);
}

RaftJournal::Iterator::Iterator(std::unique_ptr<rocksdb::Iterator> it, LogIndex startingPoint, bool mustMatchStartingPoint) {
  iter = std::move(it);
  currentIndex = startingPoint;
  iter->Seek(encodeEntryKey(currentIndex));

  if(!this->valid()) {
    iter.reset();
    return;
  }

  // Maybe the startingPoint does not exist.. return an empty iterator in
  // such case.
  if(mustMatchStartingPoint && iter->key() != encodeEntryKey(currentIndex)) {
    iter.reset();
    return;
  }
  else {
    // Figure out which index we ended up on
    if(!parseEntryKey(iter->key().ToStringView(), currentIndex)) {
      iter.reset();
      return;
    }
  }

  validate();
}

void RaftJournal::Iterator::validate() {
  qdb_assert(this->valid());

  if(iter->key()[0] != 'E') {
    iter.reset();
    return;
  }

  qdb_assert(iter->key() == encodeEntryKey(currentIndex));
}

bool RaftJournal::Iterator::valid() {
  return iter && iter->Valid();
}

void RaftJournal::Iterator::next() {
  qdb_assert(this->valid());

  iter->Next();
  if(iter->Valid()) {
    currentIndex++;
    validate();
  }
}

void RaftJournal::Iterator::current(RaftSerializedEntry &entry) {
  qdb_assert(this->valid());
  entry = iter->value().ToString();
}

LogIndex RaftJournal::Iterator::getCurrentIndex() const {
  return currentIndex;
}
