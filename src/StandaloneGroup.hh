// ----------------------------------------------------------------------
// File: StandaloneGroup.hh
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

#ifndef QUARKDB_STANDALONE_GROUP_H
#define QUARKDB_STANDALONE_GROUP_H

#include <memory>
#include "Dispatcher.hh"
#include "pubsub/Publisher.hh"
#include "health/HealthIndicator.hh"

namespace quarkdb {

class ShardDirectory;
class RedisDispatcher;
class StateMachine;
class Dispatcher;

class StandaloneDispatcher : public Dispatcher {
public:
  StandaloneDispatcher(StateMachine &sm, Publisher &pub);
  virtual LinkStatus dispatch(Connection *conn, RedisRequest &req) override final;
  virtual LinkStatus dispatch(Connection *conn, Transaction &req) override final;
  virtual void notifyDisconnect(Connection *conn) override final;

private:
  StateMachine* stateMachine;
  RedisDispatcher dispatcher;
  Publisher* publisher;
};

class StandaloneGroup {
public:
  StandaloneGroup(ShardDirectory& shardDirectory, bool bulkload);
  ~StandaloneGroup();

  StateMachine* getStateMachine();
  Dispatcher* getDispatcher();

  //----------------------------------------------------------------------------
  // Return health information
  //----------------------------------------------------------------------------
  NodeHealth getHealth();

private:
  ShardDirectory &shardDirectory;
  bool bulkload;

  std::unique_ptr<StandaloneDispatcher> dispatcher;
  std::unique_ptr<Publisher> publisher;
  StateMachine* stateMachine;
};

}

#endif