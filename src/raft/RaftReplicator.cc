// ----------------------------------------------------------------------
// File: RaftReplicator.cc
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

#include <thread>
#include <algorithm>
#include "raft/RaftReplicator.hh"
#include "raft/RaftTalker.hh"
#include "raft/RaftUtils.hh"
#include "raft/RaftResilverer.hh"
#include "raft/RaftConfig.hh"
#include "raft/RaftState.hh"
#include "raft/RaftJournal.hh"
#include "raft/RaftCommitTracker.hh"
#include "raft/RaftTimeouts.hh"
#include "raft/RaftLease.hh"
#include "raft/RaftTrimmer.hh"
#include "raft/RaftContactDetails.hh"
#include "utils/FileUtils.hh"
#include <dirent.h>
#include <fstream>

using namespace quarkdb;

RaftReplicator::RaftReplicator(RaftJournal &journal_, RaftState &state_, RaftLease &lease_, RaftCommitTracker &ct, RaftTrimmer &trim, ShardDirectory &sharddir, RaftConfig &conf, const RaftContactDetails &cd)
: journal(journal_), state(state_), lease(lease_), commitTracker(ct), trimmer(trim), shardDirectory(sharddir), config(conf), contactDetails(cd) {

}

RaftReplicator::~RaftReplicator() {
  deactivate();
}

RaftReplicaTracker::RaftReplicaTracker(const RaftServer &target_, const RaftStateSnapshotPtr &snapshot_, RaftJournal &journal_, RaftState &state_, RaftLease &lease_, RaftCommitTracker &ct, RaftTrimmer &trim, ShardDirectory &sharddir, RaftConfig &conf, const RaftContactDetails &cd)
: target(target_), snapshot(snapshot_), journal(journal_),
  state(state_), lease(lease_), commitTracker(ct), trimmer(trim), shardDirectory(sharddir), config(conf), contactDetails(cd),
  matchIndex(commitTracker.getHandler(target)),
  lastContact(lease.getHandler(target)),
  trimmingBlock(trimmer, 0) {
  if(target == state.getMyself()) {
    qdb_throw("attempted to run replication on myself");
  }

  RaftStateSnapshotPtr current = state.getSnapshot();
  if(snapshot->term > current->term) {
    qdb_throw("bug, a state snapshot has a larger term than the current state");
  }

  if(snapshot->term < current->term) {
    return;
  }

  if(current->status != RaftStatus::LEADER && current->status != RaftStatus::SHUTDOWN) {
    qdb_throw("bug, attempted to initiate replication for a term in which I'm not a leader");
  }

  running = true;
  thread = std::thread(&RaftReplicaTracker::main, this);

  heartbeatThread.reset(&RaftReplicaTracker::sendHeartbeats, this);
  heartbeatThread.setName(SSTR("heartbeat-thread-for-" << target.toString()));
}

RaftReplicaTracker::~RaftReplicaTracker() {
  shutdown = 1;
  while(running) {
    journal.notifyWaitingThreads();
  }
  if(thread.joinable()) {
    thread.join();
  }
}

bool RaftReplicaTracker::buildPayload(LogIndex nextIndex, int64_t payloadLimit,
  std::vector<std::string> &entries, RaftTerm &lastEntryTerm) {

  int64_t payloadSize = std::min(payloadLimit, journal.getLogSize() - nextIndex);
  entries.resize(payloadSize);

  RaftJournal::Iterator iterator = journal.getIterator(nextIndex, true);
  RaftTerm entryTerm = -1;

  for(int64_t i = nextIndex; i < nextIndex+payloadSize; i++) {
    if(!iterator.valid()) {
      qdb_critical("could not fetch entry with index " << i << " .. aborting building payload");
      return false;
    }

    iterator.current(entries[i-nextIndex]);

    entryTerm = RaftEntry::fetchTerm(entries[i-nextIndex]);
    if(snapshot->term < entryTerm) {
      qdb_warn("Found journal entry with higher term than my snapshot, " << snapshot->term << " vs " << entryTerm);
      return false;
    }

    iterator.next();
  }

  lastEntryTerm = entryTerm;
  return true;
}

enum class AppendEntriesReception {
  kOk = 0,
  kNotArrivedYet = 1,
  kError = 2
};

static AppendEntriesReception retrieve_response(
  std::future<redisReplyPtr> &fut,
  RaftAppendEntriesResponse &resp,
  const std::chrono::milliseconds &timeout
) {

  std::future_status status = fut.wait_for(timeout);
  if(status != std::future_status::ready) {
    return AppendEntriesReception::kNotArrivedYet;
  }

  redisReplyPtr rep = fut.get();
  if(rep == nullptr) return AppendEntriesReception::kError;

  if(!RaftParser::appendEntriesResponse(rep, resp)) {
    if(strncmp(rep->str, "ERR unavailable", strlen("ERR unavailable")) != 0) {
      // unexpected response
      qdb_critical("cannot parse response from append entries");
    }
    return AppendEntriesReception::kError;
  }

  return AppendEntriesReception::kOk;
}

static bool retrieve_heartbeat_reply(std::future<redisReplyPtr> &fut, RaftHeartbeatResponse &resp) {
  std::future_status status = fut.wait_for(std::chrono::milliseconds(500));
  if(status != std::future_status::ready) {
    return false;
  }

  redisReplyPtr rep = fut.get();
  if(rep == nullptr) return false;

  if(!RaftParser::heartbeatResponse(rep, resp)) {
    if(strncmp(rep->str, "ERR unavailable", strlen("ERR unavailable")) != 0) {
      // unexpected response
      qdb_critical("cannot parse response from heartbeat");
    }

    return false;
  }

  return true;
}

void RaftReplicaTracker::triggerResilvering() {
  // Check: Already resilvering target?
  if(resilverer && resilverer->getStatus().state == ResilveringState::INPROGRESS) {
    return;
  }

  if(resilverer && resilverer->getStatus().state == ResilveringState::FAILED) {
    qdb_critical("Resilvering attempt for " << target.toString() << " failed: " << resilverer->getStatus().err);
    resilverer.reset();

    // Try again during the next round
    return;
  }

  // Start the resilverer
  resilverer.reset(new RaftResilverer(shardDirectory, target, contactDetails, trimmer));
}

class ConditionVariableNotifier {
public:
  ConditionVariableNotifier(std::mutex &mtx_, std::condition_variable &cv_)
  : mtx(mtx_), cv(cv_) {}

  ~ConditionVariableNotifier() {
    std::unique_lock<std::mutex> lock(mtx);
    cv.notify_one();
  }

private:
  std::mutex &mtx;
  std::condition_variable &cv;
};

void RaftReplicaTracker::monitorAckReception(ThreadAssistant &assistant) {
  ConditionVariableNotifier destructionNotifier(inFlightMtx, inFlightPoppedCV);
  std::unique_lock<std::mutex> lock(inFlightMtx);

  while(!assistant.terminationRequested()) {
    if(inFlight.size() == 0) {
      // Empty queue, sleep
      inFlightCV.wait_for(lock, contactDetails.getRaftTimeouts().getHeartbeatInterval());
      continue;
    }

    // Fetch top item
    PendingResponse item = std::move(inFlight.front());
    inFlight.pop();
    inFlightPoppedCV.notify_one();
    lock.unlock();

    RaftAppendEntriesResponse response;

    size_t attempts = 0;
    while(attempts < 10) {

      if(assistant.terminationRequested()) {
        streamingUpdates = false;
        return;
      }

      AppendEntriesReception reception = retrieve_response(
        item.fut,
        response,
        std::chrono::milliseconds(500)
      );

      if(reception == AppendEntriesReception::kOk) {
        // Exit inner loop to verify acknowledgement
        break;
      }

      if(reception == AppendEntriesReception::kError) {
        // Stop streaming, we need to stabilize the target
        streamingUpdates = false;
        return;
      }
    }

    // If we're here, an acknowledgement to AppendEntries has been received.
    // Verify it makes sense.

    state.observed(response.term, {});

    if(!response.outcome) {
      streamingUpdates = false;
      return;
    }

    if(response.term != snapshot->term) {
      streamingUpdates = false;
      return;
    }

    if(response.logSize != item.pushedFrom + item.payloadSize) {
      qdb_warn("Mismatch in expected logSize when streaming updates: response.logsize: " << response.logSize <<
        ", pushedFrom: " << item.pushedFrom << ", payloadSize: " << item.payloadSize);

       streamingUpdates = false;
       return;
    }

    // All clear, acknowledgement is OK, carry on.
    updateStatus(true, response.logSize);
    lastContact.heartbeat(item.sent);

    // Only update the commit tracker once we're replicating entries from our
    // snapshot term. (Figure 8 and section 5.4.2 from the raft paper)
    if(item.lastEntryTerm == snapshot->term) {
      matchIndex.update(response.logSize-1);
    }

    // Progress trimming block.
    trimmingBlock.enforce(response.logSize-2);

    lock.lock();
  }

  streamingUpdates = false;
}

bool RaftReplicaTracker::sendPayload(RaftTalker &talker, LogIndex nextIndex, int64_t payloadLimit,
  std::future<redisReplyPtr> &reply, std::chrono::steady_clock::time_point &contact, int64_t &payloadSize,
  RaftTerm &lastEntryTerm) {
  RaftTerm prevTerm;

  if(!journal.fetch(nextIndex-1, prevTerm).ok()) {
    qdb_critical("unable to fetch log entry " << nextIndex-1 << " when tracking " << target.toString() << ". My log start: " << journal.getLogStart());
    state.observed(snapshot->term+1, {});
    return false;
  }

  if(snapshot->term < prevTerm) {
    qdb_warn("Last journal entry has higher term than my snapshot, halting replication.");
    state.observed(snapshot->term+1, {});
    return false;
  }

  // It's critical that we retrieve the commit index before the actual entries.
  // The following could happen:
  // - We build the payload.
  // - We recognize a different leader in the meantime.
  // - The other leader overwrites some of our enties as inconsistent, and progresses
  //   our commit index.
  // - We now send an AppendEntries to the poor target, marking potentially inconsistent
  //   entries as committed.
  // - The target crashes after detecting journal inconsistency once the new
  //   leader tries to replicate entries onto it.

  LogIndex commitIndexForTarget = journal.getCommitIndex();

  std::vector<RaftSerializedEntry> entries;
  if(!buildPayload(nextIndex, payloadLimit, entries, lastEntryTerm)) {
    state.observed(snapshot->term+1, {});
    return false;
  }

  payloadSize = entries.size();

  contact = std::chrono::steady_clock::now();
  reply = talker.appendEntries(
    snapshot->term,
    state.getMyself(),
    nextIndex-1,
    prevTerm,
    commitIndexForTarget,
    entries
  );

  return true;
}

LogIndex RaftReplicaTracker::streamUpdates(RaftTalker &talker, LogIndex firstNextIndex) {
  // If we're here, it means our target is very stable, so we should be able to
  // continuously stream updates without waiting for the replies.
  //
  // As soon as an error is discovered we return, and let the parent function
  // deal with it to stabilize the target once more.

  streamingUpdates = true;
  AssistedThread ackmonitor(&RaftReplicaTracker::monitorAckReception, this);
  ackmonitor.setName(SSTR("streaming-replication-ack-monitor-for-" << SSTR(target.toString())));

  const int64_t payloadLimit = 512;
  LogIndex nextIndex = firstNextIndex;

  while(shutdown == 0 && streamingUpdates && state.isSnapshotCurrent(snapshot.get())) {
    std::chrono::steady_clock::time_point contact;
    std::future<redisReplyPtr> fut;
    int64_t payloadSize;
    RaftTerm lastEntryTerm;

    if(!sendPayload(talker, nextIndex, payloadLimit, fut, contact, payloadSize, lastEntryTerm)) {
      qdb_warn("Unexpected error when sending payload to target " << target.toString() << ", halting replication");
      break;
    }

    std::unique_lock<std::mutex> lock(inFlightMtx);
    inFlight.emplace(
      std::move(fut),
      contact,
      nextIndex,
      payloadSize,
      lastEntryTerm
    );

    inFlightCV.notify_one();

    while(inFlight.size() >= 512 && shutdown == 0 && streamingUpdates && state.isSnapshotCurrent(snapshot.get())) {
      inFlightPoppedCV.wait_for(lock, contactDetails.getRaftTimeouts().getHeartbeatInterval());
    }

    lock.unlock();

    // Assume a positive response from the target, and keep pushing
    // if there are more entries.
    nextIndex += payloadSize;

    if(nextIndex >= journal.getLogSize()) {
      journal.waitForUpdates(nextIndex, contactDetails.getRaftTimeouts().getHeartbeatInterval());
    }
    else {
      // fire next round
    }
  }

  // Again, no guarantees this is the actual, current logSize of the target,
  // but the parent will figure it out.
  return nextIndex;
}

void RaftReplicaTracker::updateStatus(bool online, LogIndex logSize) {
  statusOnline = online;
  statusLogSize = logSize;

  if(resilverer) {
    statusResilveringProgress.set(SSTR(resilverer->getProgress() << "/" << resilverer->getTotalToSend()));
  }
  else {
    statusResilveringProgress.set("");
  }
}

ReplicaStatus RaftReplicaTracker::getStatus() {
  return { target, statusOnline, statusLogSize, statusNodeVersion.get(), statusResilveringProgress.get() };
}

void RaftReplicaTracker::sendHeartbeats(ThreadAssistant &assistant) {
  RaftTalker talker(target, contactDetails, "internal-heartbeat-sender");

  while(!assistant.terminationRequested() && shutdown == 0 && state.isSnapshotCurrent(snapshot.get())) {
    statusNodeVersion.set(talker.getNodeVersion());

    std::chrono::steady_clock::time_point contact = std::chrono::steady_clock::now();
    std::future<redisReplyPtr> fut = talker.heartbeat(snapshot->term, state.getMyself());
    RaftHeartbeatResponse resp;

    if(!retrieve_heartbeat_reply(fut, resp)) {
      goto nextRound;
    }

    state.observed(resp.term, {});
    if(snapshot->term < resp.term || !resp.nodeRecognizedAsLeader) continue;
    lastContact.heartbeat(contact);

nextRound:
    state.wait(contactDetails.getRaftTimeouts().getHeartbeatInterval());
  }
}

class OnlineTracker {
public:
  OnlineTracker() : online(false) { }

  void seenOnline(){
    online = true;
    lastSeen = std::chrono::steady_clock::now();
  }

  void seenOffline() {
    online = false;
  }

  bool isOnline() const {
    return online;
  }

  bool hasBeenOfflineForLong() const {
    if(online) return false;
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - lastSeen) > std::chrono::minutes(1);
  }

private:
  bool online;
  std::chrono::steady_clock::time_point lastSeen;
};

void RaftReplicaTracker::main() {
  RaftTalker talker(target, contactDetails, "internal-replicator");
  LogIndex nextIndex = journal.getLogSize();

  RaftMatchIndexTracker &matchIndex = commitTracker.getHandler(target);
  RaftLastContact &lastContact = lease.getHandler(target);

  OnlineTracker onlineTracker;
  int64_t payloadLimit = 1;

  bool warnStreamingHiccup = false;
  bool needResilvering = false;
  while(shutdown == 0 && state.isSnapshotCurrent(snapshot.get())) {

    if(warnStreamingHiccup) {
      qdb_warn("Hiccup during streaming replication of " << target.toString() << ", switching back to conservative replication.");
      warnStreamingHiccup = false;
    }

    // Target looks pretty stable, start continuous stream
    if(onlineTracker.isOnline() && payloadLimit >= 8) {
      qdb_info("Target " << target.toString() << " appears stable, initiating streaming replication.");
      resilverer.reset();
      nextIndex = streamUpdates(talker, nextIndex);
      inFlight = std::queue<PendingResponse>(); // clear queue
      warnStreamingHiccup = true;
      onlineTracker.seenOnline();
      // Something happened when streaming updates, switch back to conservative
      // mode and wait for each response
      payloadLimit = 1;
      continue;
    }

    if(nextIndex <= 0) qdb_throw("nextIndex has invalid value: " << nextIndex);
    if(nextIndex <= journal.getLogStart()) nextIndex = journal.getLogSize();

    std::chrono::steady_clock::time_point contact;
    std::future<redisReplyPtr> fut;
    int64_t payloadSize;
    RaftTerm lastEntryTerm;

    if(!sendPayload(talker, nextIndex, payloadLimit, fut, contact, payloadSize, lastEntryTerm)) {
      qdb_warn("Unexpected error when sending payload to target " << target.toString() << ", halting replication");
      break;
    }

    RaftAppendEntriesResponse resp;
    // Check: Is the target even online?
    if(retrieve_response(fut, resp, std::chrono::milliseconds(500)) != AppendEntriesReception::kOk) {
      if(onlineTracker.isOnline()) {
        payloadLimit = 1;
        qdb_event("Replication target " << target.toString() << " went offline.");
        onlineTracker.seenOffline();
      }

      goto nextRound;
    }

    if(!onlineTracker.isOnline()) {
      // Print an event if the target just came back online
      onlineTracker.seenOnline();
      qdb_event("Replication target " << target.toString() << " came back online. Log size: " << resp.logSize << ", lagging " << (journal.getLogSize() - resp.logSize) << " entries behind me. (approximate)");
    }

    state.observed(resp.term, {});
    if(snapshot->term < resp.term) continue;
    lastContact.heartbeat(contact);

    // Check: Does the target need resilvering?
    if(resp.logSize <= journal.getLogStart()) {
      nextIndex = journal.getLogSize();

      if(!needResilvering) {
        qdb_event("Unable to perform replication on " << target.toString() << ", it's too far behind (its logsize: " << resp.logSize << ") and my journal starts at " << journal.getLogStart() << ".");
        needResilvering = true;
        payloadLimit = 1;
      }

      if(config.getResilveringEnabled()) {
        triggerResilvering();
      }

      goto nextRound;
    }

    needResilvering = false;
    resilverer.reset();

    // Check: Is my current view of the target's journal correct? (nextIndex)
    if(!resp.outcome) {
      // never try to touch entry #0
      if(nextIndex >= 2 && nextIndex <= resp.logSize) {
        // There are journal inconsistencies. Move back a step to remove a single
        // inconsistent entry in the next round.
        nextIndex--;
      } else if(resp.logSize > 0) {
        // Our nextIndex is outdated, update
        nextIndex = resp.logSize;
      }

      goto nextRound;
    }

    // All checks have passed
    if(nextIndex+payloadSize != resp.logSize) {
      qdb_warn("mismatch in expected logSize. nextIndex = " << nextIndex << ", payloadSize = " << payloadSize << ", logSize: " << resp.logSize << ", resp.term: " << resp.term << ", my term: " << snapshot->term << ", journal size: " << journal.getLogSize());
    }

    // Only update the commit tracker once we're replicating entries from our
    // snapshot term. (Figure 8 and section 5.4.2 from the raft paper)
    if(lastEntryTerm == snapshot->term) {
      matchIndex.update(resp.logSize-1);
    }

    nextIndex = resp.logSize;
    if(payloadLimit < 1024) {
      payloadLimit *= 2;
    }

nextRound:
    if(onlineTracker.hasBeenOfflineForLong()) {
      // Don't let a "permanently offline" node block journal trimming indefinitely
      trimmingBlock.lift();
    }
    else {
      trimmingBlock.enforce(nextIndex-2);
    }

    updateStatus(onlineTracker.isOnline(), resp.logSize);
    if(!onlineTracker.isOnline() || needResilvering) {
      state.wait(contactDetails.getRaftTimeouts().getHeartbeatInterval());
    }
    else if(onlineTracker.isOnline() && nextIndex >= journal.getLogSize()) {
      journal.waitForUpdates(nextIndex, contactDetails.getRaftTimeouts().getHeartbeatInterval());
    }
    else {
      // don't wait, fire next round of updates
    }
  }
  qdb_event("Shutting down replicator tracker for " << target.toString());
  running = false;
}

void RaftReplicator::activate(RaftStateSnapshotPtr &snapshot_) {
  std::scoped_lock lock(mtx);
  qdb_event("Activating replicator for term " << snapshot_->term);

  qdb_assert(targets.empty());
  snapshot = snapshot_;

  commitTracker.reset();
  reconfigure();
}

void RaftReplicator::deactivate() {
  std::scoped_lock lock(mtx);
  qdb_event("De-activating replicator");

  for(auto it = targets.begin(); it != targets.end(); it++) {
    delete it->second;
  }
  targets.clear();

  snapshot = {};
  commitTracker.reset();
}

ReplicationStatus RaftReplicator::getStatus() {
  std::scoped_lock lock(mtx);

  ReplicationStatus ret;
  for(auto it = targets.begin(); it != targets.end(); it++) {
    ret.addReplica(it->second->getStatus());
  }

  ret.shakyQuorum = lease.getShakyQuorumDeadline() < std::chrono::steady_clock::now();
  return ret;
}

static std::vector<RaftServer> all_servers_except_myself(const std::vector<RaftServer> &nodes, const RaftServer &myself) {
  std::vector<RaftServer> remaining;
  size_t skipped = 0;

  for(size_t i = 0; i < nodes.size(); i++) {
    if(myself == nodes[i]) {
      if(skipped != 0) qdb_throw("found myself in the nodes list twice");
      skipped++;
      continue;
    }
    remaining.push_back(nodes[i]);
  }

  if(skipped != 1) qdb_throw("unexpected value for 'skipped', got " << skipped << " instead of 1");
  if(remaining.size() != nodes.size()-1) qdb_throw("unexpected size for remaining: " << remaining.size() << " instead of " << nodes.size()-1);
  return remaining;
}

void RaftReplicator::reconfigure() {
  RaftMembership membership = journal.getMembership();
  qdb_info("Reconfiguring replicator for membership epoch " << membership.epoch);

  // Build list of targets
  std::vector<RaftServer> full_nodes = all_servers_except_myself(membership.nodes, state.getMyself());
  std::vector<RaftServer> targets = full_nodes;

  // add observers
  for(const RaftServer& srv : membership.observers) {
    if(srv == state.getMyself()) qdb_throw("found myself in the list of observers, even though I'm leader: " << serializeNodes(membership.observers));
    targets.push_back(srv);
  }

  // reconfigure lease and commit tracker - only take into account full nodes!
  commitTracker.updateTargets(full_nodes);
  lease.updateTargets(full_nodes);

  // now set them
  setTargets(targets);
}

void RaftReplicator::setTargets(const std::vector<RaftServer> &newTargets) {
  std::scoped_lock lock(mtx);

  // add targets?
  for(size_t i = 0; i < newTargets.size(); i++) {
    if(targets.find(newTargets[i]) == targets.end()) {
      targets[newTargets[i]] = new RaftReplicaTracker(newTargets[i], snapshot, journal, state, lease, commitTracker, trimmer, shardDirectory, config, contactDetails);
    }
  }

  // remove targets?
  std::vector<RaftServer> todel;

  for(auto it = targets.begin(); it != targets.end(); it++) {
    if(!contains(newTargets, it->first)) {
      todel.push_back(it->first);
    }
  }

  for(size_t i = 0; i < todel.size(); i++) {
    delete targets[todel[i]];
    targets.erase(todel[i]);
  }
}
