// ----------------------------------------------------------------------
// File: QuarkDBNode.hh
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

#ifndef QUARKDB_NODE_H
#define QUARKDB_NODE_H

#include <chrono>

#include "Dispatcher.hh"
#include "Configuration.hh"
#include "raft/RaftTimeouts.hh"
#include "auth/AuthenticationDispatcher.hh"
#include "health/HealthIndicator.hh"

namespace quarkdb {

struct QuarkDBInfo {
  Mode mode;
  std::string baseDir;
  std::string configurationPath;
  std::string version;
  std::string rocksdbVersion;
  std::string xrootdHeaders;
  HealthStatus nodeHealthStatus;

  size_t monitors;
  int64_t bootTime;
  int64_t uptime;

  std::vector<std::string> toVector() const;
};

class Shard; class ShardDirectory;

class QuarkDBNode : public Dispatcher {
public:
  QuarkDBNode(const Configuration &config, const RaftTimeouts &t, ShardDirectory *injectedDirectory = nullptr);
  ~QuarkDBNode();

  virtual LinkStatus dispatch(Connection *conn, RedisRequest &req) override final;
  virtual LinkStatus dispatch(Connection *conn, Transaction &transaction) override final;
  virtual void notifyDisconnect(Connection *conn) override final {}

  const Configuration& getConfiguration() {
    return configuration;
  }

  Shard* getShard() {
    return shard.get();
  }

private:
  bool isAuthenticated(Connection *conn) const;

  std::unique_ptr<ShardDirectory> shardDirectoryOwnership;
  std::unique_ptr<Shard> shard;

  Configuration configuration;
  ShardDirectory *shardDirectory;

  QuarkDBInfo info();

  std::atomic<bool> shutdown {false};
  const RaftTimeouts timeouts;

  std::chrono::steady_clock::time_point bootStart;
  std::chrono::steady_clock::time_point bootEnd;

  std::string password;
  AuthenticationDispatcher authDispatcher;
};

}
#endif
