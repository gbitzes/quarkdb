// ----------------------------------------------------------------------
// File: RaftDirector.hh
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

#ifndef QUARKDB_RAFT_DIRECTOR_HH
#define QUARKDB_RAFT_DIRECTOR_HH

#include "raft/RaftState.hh"
#include "raft/RaftTimeouts.hh"
#include "raft/RaftDispatcher.hh"
#include "raft/RaftLease.hh"
#include "raft/RaftCommitTracker.hh"
#include "raft/RaftWriteTracker.hh"
#include <thread>

namespace quarkdb {

class RaftTrimmer; class ShardDirectory; class RaftConfig; class RaftReplicator;
class RaftContactDetails; class Publisher;
class RaftHeartbeatTracker; class RaftJournal;
class StateMachine;

class RaftDirector {
public:
  RaftDirector(RaftJournal &journal, StateMachine &stateMachine, RaftState &state, RaftLease &lease, RaftCommitTracker &commitTracker, RaftHeartbeatTracker &rht, RaftWriteTracker &wt, ShardDirectory &sharddir, RaftConfig &config, RaftReplicator &replicator, const RaftContactDetails &contactDetails, Publisher &publisher);
  ~RaftDirector();
  DISALLOW_COPY_AND_ASSIGN(RaftDirector);
private:
  void main();
  void followerLoop(RaftStateSnapshotPtr &snapshot);
  void leaderLoop(RaftStateSnapshotPtr &snapshot);
  void runForLeader(bool preVote);
  void applyCommits();
  bool checkBasicSanity();

  RaftJournal &journal;
  StateMachine &stateMachine;
  RaftState &state;
  RaftHeartbeatTracker &heartbeatTracker;
  RaftLease &lease;
  RaftCommitTracker &commitTracker;
  RaftWriteTracker &writeTracker;
  ShardDirectory &shardDirectory;
  RaftConfig &config;
  RaftReplicator &replicator;
  const RaftContactDetails &contactDetails;
  Publisher &publisher;

  std::chrono::steady_clock::time_point lastHeartbeatBeforeVeto;
  std::thread mainThread;
};

}

#endif
