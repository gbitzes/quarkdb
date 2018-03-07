// ----------------------------------------------------------------------
// File: Shard.hh
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

#ifndef __QUARKDB_SHARD_H__
#define __QUARKDB_SHARD_H__

#include "raft/RaftTimeouts.hh"
#include "Dispatcher.hh"
#include "Configuration.hh"
#include "redis/CommandMonitor.hh"
#include "utils/InFlightTracker.hh"

namespace quarkdb {

class RaftGroup; class ShardDirectory;
class Shard : public Dispatcher {
public:
  Shard(ShardDirectory *shardDir, const RaftServer &me, Mode mode, const RaftTimeouts &t);
  ~Shard();

  RaftGroup* getRaftGroup();
  void spinup();
  void spindown();
  virtual LinkStatus dispatch(Connection *conn, RedisRequest &req) override final;
  virtual LinkStatus dispatch(Connection *conn, WriteBatch &batch) override final;
  size_t monitors() { return commandMonitor.size(); }

private:
  void detach();
  void attach();
  void start();
  void stopAcceptingRequests();

  CommandMonitor commandMonitor;
  ShardDirectory *shardDirectory;

  RaftGroup *raftGroup = nullptr;
  StateMachine *stateMachine = nullptr;
  Dispatcher *dispatcher = nullptr;

  RaftServer myself;
  Mode mode;
  RaftTimeouts timeouts;

  InFlightTracker inFlightTracker;
  std::mutex raftGroupMtx;
};

}

#endif
