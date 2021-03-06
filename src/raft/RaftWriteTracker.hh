// ----------------------------------------------------------------------
// File: RaftWriteTracker.hh
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

#ifndef QUARKDB_RAFT_WRITE_TRACKER_HH
#define QUARKDB_RAFT_WRITE_TRACKER_HH

#include "raft/RaftCommon.hh"
#include "Dispatcher.hh"

namespace quarkdb {

//------------------------------------------------------------------------------
// Forward declarations
//------------------------------------------------------------------------------
class RaftJournal; class StateMachine;
class RedisEncodedResponse; class Publisher;

//------------------------------------------------------------------------------
// We track the state of pending writes, and apply them to the state machine
// when necessary.
//------------------------------------------------------------------------------
class RaftWriteTracker {
public:
  RaftWriteTracker(RaftJournal &jr, StateMachine &sm, Publisher &pub);
  ~RaftWriteTracker();

  bool append(LogIndex index, RaftTerm term, Transaction &&tx, const std::shared_ptr<PendingQueue> &queue, RedisDispatcher &dispatcher);
  void flushQueues(const RedisEncodedResponse &response);
  size_t size() { return blockedWrites.size(); }
private:
  std::mutex mtx;
  std::thread commitApplier;

  RaftJournal &journal;
  StateMachine &stateMachine;

  RedisDispatcher redisDispatcher;
  RaftBlockedWrites blockedWrites;

  std::atomic<bool> commitApplierActive {true};
  std::atomic<bool> shutdown {false};

  void applyCommits();
  void updatedCommitIndex(LogIndex commitIndex);
  void applySingleCommit(LogIndex index);
};

}

#endif
