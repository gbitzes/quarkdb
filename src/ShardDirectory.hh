// ----------------------------------------------------------------------
// File: ShardDirectory.hh
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

#ifndef __QUARKDB_SHARD_DIRECTORY_H__
#define __QUARKDB_SHARD_DIRECTORY_H__

#include <memory>
#include "Common.hh"
#include "Configuration.hh"
#include "Status.hh"
#include "utils/Resilvering.hh"

namespace quarkdb {

class StateMachine; class RaftJournal; class Configuration;

// Snapshot of a shard. The underlying snapshot directory is deleted upon object's
// deletion, it is thus not suitable for long-lived backups.

class ShardSnapshot {
public:
  ShardSnapshot(const std::string &path);
  ~ShardSnapshot();

  std::string getPath();
private:
  const std::string path;
};

using ShardID = std::string;
using ResilveringEventID = std::string;
using SnapshotID = std::string;

// Manages a shard directory on the physical file system.
// Keeps ownership of StateMachine and RaftJournal - initialized lazily.
class ShardDirectory {
public:
  ShardDirectory(const std::string &path, Configuration config = {});
  ~ShardDirectory();

  StateMachine *getStateMachineForBulkload();
  StateMachine *getStateMachine();
  RaftJournal *getRaftJournal();
  bool hasRaftJournal(std::string &err) const;

  // Reset the contents of both the state machine and the raft journal.
  // Physical paths remain the same.
  void obliterate(RaftClusterID clusterID, const std::vector<RaftServer> &nodes,
    LogIndex startIndex, FsyncPolicy fsyncPolicy, std::unique_ptr<StateMachine> existingContents);

  // Create a standalone shard.
  static ShardDirectory* create(const std::string &path, RaftClusterID clusterID, ShardID shardID, std::unique_ptr<StateMachine> sm, Status &st);

  // Create a consensus shard.
  static ShardDirectory* create(const std::string &path, RaftClusterID clusterID, ShardID shardID, const std::vector<RaftServer> &nodes, LogIndex startIndex, FsyncPolicy fsyncPolicy, std::unique_ptr<StateMachine> sm, Status &st);

  std::unique_ptr<ShardSnapshot> takeSnapshot(const SnapshotID &id, std::string &err);

  bool resilveringStart(const ResilveringEventID &id, std::string &err);
  bool resilveringCopy(const ResilveringEventID &id, std::string_view filename, std::string_view contents, std::string &err);
  bool resilveringFinish(const ResilveringEventID &id, std::string &err);
  const ResilveringHistory& getResilveringHistory() const;

  // empty string in case of success - otherwise contains the error
  // TODO: replace with proper status object
  std::string checkpoint(std::string_view path);

  //----------------------------------------------------------------------------
  // Initialize our StateMachine with the given source, if any.
  // If no source is given, create a brand new one.
  //----------------------------------------------------------------------------
  void initializeStateMachine(std::unique_ptr<StateMachine> sm, LogIndex initialLastApplied);

  //----------------------------------------------------------------------------
  // Wipe out StateMachine contents.
  //----------------------------------------------------------------------------
  void wipeoutStateMachineContents();

private:
  void parseResilveringHistory();
  void storeResilveringHistory();

  void detach();
  std::string path;
  Configuration configuration;
  ShardID shardID;

  StateMachine *smptr = nullptr;
  RaftJournal *journalptr = nullptr;

  ResilveringHistory resilveringHistory;

  std::string stateMachinePath() const;
  std::string raftJournalPath() const;
  std::string currentPath() const;
  std::string resilveringHistoryPath();

  std::string getSupplanted(const ResilveringEventID &id);
  std::string getResilveringArena(const ResilveringEventID &id);
  std::string getTempSnapshot(const SnapshotID &id);

  static Status initializeDirectory(const std::string &path, RaftClusterID clusterID, ShardID shardID);
};



}


#endif
