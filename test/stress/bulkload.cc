// ----------------------------------------------------------------------
// File: bulkload.cc
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

#include "raft/RaftDispatcher.hh"
#include "raft/RaftReplicator.hh"
#include "raft/RaftTalker.hh"
#include "raft/RaftTimeouts.hh"
#include "raft/RaftCommitTracker.hh"
#include "raft/RaftReplicator.hh"
#include "Poller.hh"
#include "Configuration.hh"
#include "QuarkDBNode.hh"
#include "../test-utils.hh"
#include "RedisParser.hh"
#include <gtest/gtest.h>
#include <qclient/QClient.hh>
#include "utils/AssistedThread.hh"
#include "../test-reply-macros.hh"

#define ASSERT_OK(msg) ASSERT_TRUE(msg.ok())
using namespace quarkdb;

TEST(BulkLoad, BasicSanity) {
  system("rm -rf /tmp/quarkdb-bulkload-test");

  {
  StateMachine stateMachine("/tmp/quarkdb-bulkload-test", false, true);

  for(size_t i = 0; i < 100; i++) {
    bool created;
    ASSERT_OK(stateMachine.hset("some-key", SSTR("field-" << i), "value", created));
    ASSERT_TRUE(created);

    ASSERT_OK(stateMachine.hset(SSTR("some-key-" << i), "field", "value", created));
    ASSERT_TRUE(created);

    ASSERT_OK(stateMachine.hset(SSTR("some-key-" << i), "field", "value", created));
    ASSERT_TRUE(created);

    ASSERT_OK(stateMachine.set(SSTR("a-" << i), SSTR("v-" << i)));
    ASSERT_OK(stateMachine.set(SSTR("z#|#-" << i), SSTR("vz-" << i)));

    std::vector<std::string> items;
    items.push_back(SSTR(i));
    items.push_back(SSTR(i+1));
    items.push_back(SSTR(i+200));

    int64_t ignored;
    ASSERT_OK(stateMachine.sadd(SSTR("some-set-" << i), items.begin(), items.end(), ignored));
    ASSERT_OK(stateMachine.sadd("some-set", items.begin(), items.end(), ignored));
  }


  stateMachine.finalizeBulkload();
  }

  size_t len;
  StateMachine stateMachine("/tmp/quarkdb-bulkload-test");
  ASSERT_OK(stateMachine.hlen("some-key", len));
  ASSERT_EQ(len, 100u);

  ASSERT_OK(stateMachine.scard("some-set", len));
  ASSERT_EQ(len, 201u);

  for(size_t i = 0; i < 100; i++) {
    ASSERT_OK(stateMachine.hlen(SSTR("some-key-" << i), len));
    ASSERT_EQ(len, 1u);

    std::string v;
    ASSERT_OK(stateMachine.get(SSTR("a-" << i), v));
    ASSERT_EQ(v, SSTR("v-" << i));

    ASSERT_OK(stateMachine.get(SSTR("z#|#-" << i), v));
    ASSERT_EQ(v, SSTR("vz-" << i));

    ASSERT_OK(stateMachine.scard(SSTR("some-set-" << i), len));
    ASSERT_EQ(len, 3u);
  }
}