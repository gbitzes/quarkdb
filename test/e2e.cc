// ----------------------------------------------------------------------
// File: e2e.cc
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
#include "raft/RaftConfig.hh"
#include "Poller.hh"
#include "Configuration.hh"
#include "QuarkDBNode.hh"
#include "test-utils.hh"
#include "RedisParser.hh"
#include <gtest/gtest.h>
#include "test-reply-macros.hh"
#include "qclient/QScanner.hh"
#include "qclient/QSet.hh"
#include "qclient/ConnectionInitiator.hh"
#include "qclient/QHash.hh"

using namespace quarkdb;
#define ASSERT_OK(msg) ASSERT_TRUE(msg.ok())
class Raft_e2e : public TestCluster3NodesFixture {};
class Raft_e2e5 : public TestCluster5NodesFixture {};

TEST_F(Raft_e2e, coup) {
  spinup(0); spinup(1); spinup(2);

  RETRY_ASSERT_TRUE(checkStateConsensus(0, 1, 2));

  int leaderID = getLeaderID();
  ASSERT_GE(leaderID, 0);
  ASSERT_LE(leaderID, 2);

  int instigator = (leaderID+1)%3;
  for(int i = 1; i < 10; i++) {
    RaftTerm term = state(instigator)->getCurrentTerm();
    ASSERT_REPLY(tunnel(instigator)->exec("RAFT_ATTEMPT_COUP"), "vive la revolution");
    RETRY_ASSERT_TRUE(state(instigator)->getCurrentTerm() > term);
    RETRY_ASSERT_TRUE(checkStateConsensus(0, 1, 2));

    if(instigator == getLeaderID()) {
      qdb_info("Successful coup in " << i << " attempts");
      return; // pass test
    }
  }
  ASSERT_TRUE(false) << "Test has failed";
}

TEST_F(Raft_e2e, simultaneous_clients) {
  spinup(0); spinup(1); spinup(2);
  RETRY_ASSERT_TRUE(checkStateConsensus(0, 1, 2));

  int leaderID = getServerID(state(0)->getSnapshot()->leader);
  ASSERT_GE(leaderID, 0);
  ASSERT_LE(leaderID, 2);

  LogIndex lastEntry = journal(leaderID)->getLogSize() - 1;
  std::vector<std::future<redisReplyPtr>> futures;

  // send off many requests, pipeline them
  futures.emplace_back(tunnel(leaderID)->exec("get", "asdf"));
  futures.emplace_back(tunnel(leaderID)->exec("ping"));
  futures.emplace_back(tunnel(leaderID)->exec("set", "asdf", "1234"));
  futures.emplace_back(tunnel(leaderID)->exec("get", "asdf"));
  futures.emplace_back(tunnel(leaderID)->exec("raft_fetch", SSTR(lastEntry+1) ));

  ASSERT_REPLY(futures[0], "");
  ASSERT_REPLY(futures[1], "PONG");
  ASSERT_REPLY(futures[2], "OK");
  ASSERT_REPLY(futures[3], "1234");

  RaftEntry entry;
  ASSERT_TRUE(RaftParser::fetchResponse(futures[4].get().get(), entry));
  ASSERT_EQ(entry.term, state(0)->getCurrentTerm());
  ASSERT_EQ(entry.request, make_req("set", "asdf", "1234"));

  futures.clear();
  futures.emplace_back(tunnel(leaderID)->exec("set", "asdf", "3456"));
  futures.emplace_back(tunnel(leaderID)->exec("get", "asdf"));

  ASSERT_REPLY(futures[0], "OK");
  ASSERT_REPLY(futures[1], "3456");

  // make sure the log entry has been propagated to all nodes
  for(size_t i = 0; i < 3; i++) {
    std::string value;
    RETRY_ASSERT_TRUE(stateMachine(i)->get("asdf", value).ok() && value == "3456");
  }

  ASSERT_REPLY(tunnel(leaderID)->exec("set", "qwerty", "789"), "OK");
  futures.clear();

  // interwine pipelined requests from three connections
  qclient::QClient tunnel2(myself(leaderID).hostname, myself(leaderID).port);
  qclient::QClient tunnel3(myself(leaderID).hostname, myself(leaderID).port);

  futures.emplace_back(tunnel2.exec("get", "qwerty"));
  futures.emplace_back(tunnel(leaderID)->exec("set", "client2", "val"));
  futures.emplace_back(tunnel(leaderID)->exec("get", "client2"));
  futures.emplace_back(tunnel(leaderID)->exec("sadd", "myset", "a"));
  futures.emplace_back(tunnel2.exec("sadd", "myset", "b"));
  futures.emplace_back(tunnel2.exec("sadd", "myset")); // malformed request
  futures.emplace_back(tunnel3.exec("set", "client3", "myval"));
  futures.emplace_back(tunnel3.exec("get", "client3"));

  // not guaranteed that response will be "myval" here, since it's on a different connection
  futures.emplace_back(tunnel2.exec("get", "client3"));

  ASSERT_REPLY(futures[0], "789");
  ASSERT_REPLY(futures[1], "OK");
  ASSERT_REPLY(futures[2], "val");
  ASSERT_REPLY(futures[3], 1);
  ASSERT_REPLY(futures[4], 1);
  ASSERT_REPLY(futures[5], "ERR wrong number of arguments for 'sadd' command");
  ASSERT_REPLY(futures[6], "OK");
  ASSERT_REPLY(futures[7], "myval");

  redisReplyPtr reply = futures[8].get();
  std::string str = std::string(reply->str, reply->len);
  qdb_info("Race-y request: GET client3 ==> " << str);
  ASSERT_TRUE(str == "myval" || str == "");

  ASSERT_REPLY(tunnel2.exec("scard", "myset"), 2);

  // but here we've received an ack - response _must_ be myval
  ASSERT_REPLY(tunnel2.exec("get", "client3"), "myval");

  RaftInfo info = dispatcher(leaderID)->info();
  ASSERT_EQ(info.blockedWrites, 0u);
  ASSERT_EQ(info.leader, myself(leaderID));

  std::string err;
  std::string checkpointPath = SSTR(commonState.testdir << "/checkpoint");

  // Before taking a checkpoint, ensure node #0 is caught up
  RETRY_ASSERT_TRUE(stateMachine(0)->getLastApplied() == stateMachine(leaderID)->getLastApplied());

  ASSERT_TRUE(dispatcher()->checkpoint(checkpointPath, err));
  ASSERT_FALSE(dispatcher()->checkpoint(checkpointPath, err)); // exists already

  // pretty expensive to open two extra databases, but necessary
  StateMachine checkpointSM(SSTR(checkpointPath << "/state-machine"));

  std::string tmp;
  ASSERT_OK(checkpointSM.get("client3", tmp));
  ASSERT_EQ(tmp, "myval");

  ASSERT_OK(checkpointSM.get("client2", tmp));
  ASSERT_EQ(tmp, "val");

  // TODO: verify checkpointSM last applied, once atomic commits are implemented

  // ensure the checkpoint journal is identical to the original
  RaftJournal checkpointJournal(SSTR(checkpointPath << "/raft-journal"));
  ASSERT_EQ(checkpointJournal.getLogSize(), journal()->getLogSize());
  for(LogIndex i = 0; i < journal()->getLogSize(); i++) {
    RaftEntry entry1, entry2;

    ASSERT_OK(checkpointJournal.fetch(i, entry1));
    ASSERT_OK(journal()->fetch(i, entry2));

    ASSERT_EQ(entry1, entry2);
  }
}

TEST_F(Raft_e2e, hscan) {
  spinup(0); spinup(1); spinup(2);
  RETRY_ASSERT_TRUE(checkStateConsensus(0, 1, 2));
  int leaderID = getServerID(state(0)->getSnapshot()->leader);

  for(size_t i = 1; i < 10; i++) {
    ASSERT_REPLY(tunnel(leaderID)->exec("hset", "hash", SSTR("f" << i), SSTR("v" << i)), 1);
  }

  redisReplyPtr reply = tunnel(leaderID)->exec("hscan", "hash", "0", "cOUnT", "3").get();
  ASSERT_REPLY(reply, std::make_pair("next:f4", make_vec("f1", "v1", "f2", "v2", "f3", "v3")));

  reply = tunnel(leaderID)->exec("hscan", "hash", "0", "asdf", "123").get();
  ASSERT_ERR(reply, "ERR syntax error");

  reply = tunnel(leaderID)->exec("hscan", "hash", "next:f4", "COUNT", "3").get();
  ASSERT_REPLY(reply, std::make_pair("next:f7", make_vec("f4", "v4", "f5", "v5", "f6", "v6")));

  reply = tunnel(leaderID)->exec("hscan", "hash", "next:f7", "COUNT", "30").get();
  ASSERT_REPLY(reply, std::make_pair("0", make_vec("f7", "v7", "f8", "v8", "f9", "v9")));

  reply = tunnel(leaderID)->exec("hscan", "hash", "adfaf").get();
  ASSERT_ERR(reply, "ERR invalid cursor");

  reply = tunnel(leaderID)->exec("hscan", "hash", "next:zz").get();
  ASSERT_REPLY(reply, std::make_pair("0", make_vec()));
}

TEST_F(Raft_e2e, scan) {
  spinup(0); spinup(1); spinup(2);
  RETRY_ASSERT_TRUE(checkStateConsensus(0, 1, 2));
  int leaderID = getLeaderID();

  for(size_t i = 1; i < 10; i++) {
    ASSERT_REPLY(tunnel(leaderID)->exec("set", SSTR("f" << i), SSTR("v" << i)), "OK");
  }

  redisReplyPtr reply = tunnel(leaderID)->exec("scan", "0", "MATCH", "f[1-2]").get();
  ASSERT_REPLY(reply, std::make_pair("0", make_vec("f1", "f2")));

  reply = tunnel(leaderID)->exec("scan", "0", "MATCH", "f*", "COUNT", "3").get();
  ASSERT_REPLY(reply, std::make_pair("next:f4", make_vec("f1", "f2", "f3")));

  // without MATCH
  reply = tunnel(leaderID)->exec("scan", "0", "COUNT", "3").get();
  ASSERT_REPLY(reply, std::make_pair("next:f4", make_vec("f1", "f2", "f3")));

  // with "*" MATCH pattern
  reply = tunnel(leaderID)->exec("scan", "0", "COUNT", "3", "MATCH", "*").get();
  ASSERT_REPLY(reply, std::make_pair("next:f4", make_vec("f1", "f2", "f3")));

  QScanner scanner(*tunnel(leaderID), "f*", 3);

  std::vector<std::string> ret;
  ASSERT_TRUE(scanner.next(ret));
  ASSERT_EQ(ret, make_vec("f1", "f2", "f3"));

  ASSERT_TRUE(scanner.next(ret));
  ASSERT_EQ(ret, make_vec("f4", "f5", "f6"));

  ASSERT_TRUE(scanner.next(ret));
  ASSERT_EQ(ret, make_vec("f7", "f8", "f9"));

  ASSERT_FALSE(scanner.next(ret));
}

TEST_F(Raft_e2e, test_qclient_convenience_classes) {
  spinup(0); spinup(1); spinup(2);
  RETRY_ASSERT_TRUE(checkStateConsensus(0, 1, 2));
  int leaderID = getLeaderID();

  std::vector<std::future<redisReplyPtr>> replies;
  for(size_t i = 0; i < 9; i++) {
    replies.push_back(tunnel(leaderID)->exec("HSET", "myhash", SSTR("f" << i), SSTR("v" << i)));
  }

  for(size_t i = 0; i < 9; i++) {
    ASSERT_REPLY(replies[i], 1);
  }

  qclient::QHash qhash(*tunnel(leaderID), "myhash");
  qclient::QHash::Iterator it = qhash.getIterator(2);

  for(size_t i = 0; i < 9; i++) {
    ASSERT_TRUE(it.valid());
    ASSERT_EQ(it.getKey(), SSTR("f" << i));
    ASSERT_EQ(it.getValue(), SSTR("v" << i));
    it.next();
  }

  ASSERT_FALSE(it.valid());
  ASSERT_EQ(it.requestsSoFar(), 5u);
}


TEST_F(Raft_e2e, test_many_redis_commands) {
  spinup(0); spinup(1); spinup(2);
  RETRY_ASSERT_TRUE(checkStateConsensus(0, 1, 2));
  int leaderID = getServerID(state(0)->getSnapshot()->leader);

  std::vector<std::future<redisReplyPtr>> futures;
  futures.emplace_back(tunnel(leaderID)->exec("SADD", "myset", "a", "b", "c"));
  futures.emplace_back(tunnel(leaderID)->exec("SCARD", "myset"));
  futures.emplace_back(tunnel(leaderID)->exec("Smembers", "myset"));
  futures.emplace_back(tunnel(leaderID)->exec("srem", "myset", "a", "b"));
  futures.emplace_back(tunnel(leaderID)->exec("srem", "myset", "b"));
  futures.emplace_back(tunnel(leaderID)->exec("scard", "myset"));
  futures.emplace_back(tunnel(leaderID)->exec("smembers", "myset"));
  futures.emplace_back(tunnel(leaderID)->exec("get", "empty_key"));

  ASSERT_REPLY(futures[0], 3);
  ASSERT_REPLY(futures[1], 3);
  ASSERT_REPLY(futures[2], make_vec("a", "b", "c"));
  ASSERT_REPLY(futures[3], 2);
  ASSERT_REPLY(futures[4], 0);
  ASSERT_REPLY(futures[5], 1);
  ASSERT_REPLY(futures[6], make_vec("c"));
  ASSERT_NIL(futures[7]);

  futures.clear();

  futures.emplace_back(tunnel(leaderID)->exec("hset", "myhash", "a", "b"));
  futures.emplace_back(tunnel(leaderID)->exec("hset", "myhash", "b", "c"));
  futures.emplace_back(tunnel(leaderID)->exec("hset", "myhash", "c", "d"));
  futures.emplace_back(tunnel(leaderID)->exec("hset", "myhash", "a", "d"));
  futures.emplace_back(tunnel(leaderID)->exec("hdel", "myhash", "a", "b", "b"));
  futures.emplace_back(tunnel(leaderID)->exec("hdel", "myhash", "a"));
  futures.emplace_back(tunnel(leaderID)->exec("sadd", "myhash", "wrongtype"));
  futures.emplace_back(tunnel(leaderID)->exec("exists", "myhash"));
  futures.emplace_back(tunnel(leaderID)->exec("hdel", "myhash", "c"));
  futures.emplace_back(tunnel(leaderID)->exec("exists", "myhash"));
  futures.emplace_back(tunnel(leaderID)->exec("sadd", "myhash", "wrongtype"));
  futures.emplace_back(tunnel(leaderID)->exec("exists", "myhash"));
  futures.emplace_back(tunnel(leaderID)->exec("hset", "myhash", "a", "b"));
  futures.emplace_back(tunnel(leaderID)->exec("srem", "myhash", "wrongtype"));
  futures.emplace_back(tunnel(leaderID)->exec("exists", "myhash"));
  futures.emplace_back(tunnel(leaderID)->exec("hset", "myhash", "a", "b"));
  futures.emplace_back(tunnel(leaderID)->exec("exists", "myhash", "myhash", "asdf"));
  futures.emplace_back(tunnel(leaderID)->exec("hexists", "myhash", "a"));
  futures.emplace_back(tunnel(leaderID)->exec("hexists", "myhash", "b"));
  futures.emplace_back(tunnel(leaderID)->exec("sismember", "myhash", "b"));
  futures.emplace_back(tunnel(leaderID)->exec("scard", "myhash"));
  futures.emplace_back(tunnel(leaderID)->exec("scard", "does-not-exist"));
  futures.emplace_back(tunnel(leaderID)->exec("raft_invalid_command"));
  futures.emplace_back(tunnel(leaderID)->exec("quarkdb_invalid_command"));
  futures.emplace_back(tunnel(leaderID)->exec("raft_fetch_last", "7"));

  size_t count = 0;
  ASSERT_REPLY(futures[count++], 1);
  ASSERT_REPLY(futures[count++], 1);
  ASSERT_REPLY(futures[count++], 1);
  ASSERT_REPLY(futures[count++], 0);
  ASSERT_REPLY(futures[count++], 2);
  ASSERT_REPLY(futures[count++], 0);
  ASSERT_REPLY(futures[count++], "ERR Invalid argument: WRONGTYPE Operation against a key holding the wrong kind of value");
  ASSERT_REPLY(futures[count++], 1);
  ASSERT_REPLY(futures[count++], 1);
  ASSERT_REPLY(futures[count++], 0);
  ASSERT_REPLY(futures[count++], 1);
  ASSERT_REPLY(futures[count++], 1);
  ASSERT_REPLY(futures[count++], "ERR Invalid argument: WRONGTYPE Operation against a key holding the wrong kind of value");
  ASSERT_REPLY(futures[count++], 1);
  ASSERT_REPLY(futures[count++], 0);
  ASSERT_REPLY(futures[count++], 1);
  ASSERT_REPLY(futures[count++], 2);
  ASSERT_REPLY(futures[count++], 1);
  ASSERT_REPLY(futures[count++], 0);
  ASSERT_REPLY(futures[count++], "ERR Invalid argument: WRONGTYPE Operation against a key holding the wrong kind of value");
  ASSERT_REPLY(futures[count++], "ERR Invalid argument: WRONGTYPE Operation against a key holding the wrong kind of value");
  ASSERT_REPLY(futures[count++], 0);
  ASSERT_REPLY(futures[count++], "ERR internal dispatching error");
  ASSERT_REPLY(futures[count++], "ERR internal dispatching error");

  redisReplyPtr entries = futures[count++].get();
  std::vector<RaftEntry> lastEntries;

  ASSERT_TRUE(RaftParser::fetchLastResponse(entries, lastEntries));
  for(size_t i = 1; i <= 7; i++) {
    RaftEntry comparison;
    LogIndex index = journal(leaderID)->getLogSize() - i;
    ASSERT_OK(journal(leaderID)->fetch(index, comparison));
    ASSERT_EQ(lastEntries[7 - i], comparison);
  }

  futures.clear();
  futures.emplace_back(tunnel(leaderID)->exec("set", "mystring", "asdf"));
  futures.emplace_back(tunnel(leaderID)->exec("keys", "*"));
  futures.emplace_back(tunnel(leaderID)->exec("exists", "mystring", "myset", "myhash", "adfa", "myhash"));
  futures.emplace_back(tunnel(leaderID)->exec("del", "myhash", "myset", "mystring"));
  futures.emplace_back(tunnel(leaderID)->exec("exists", "mystring", "myset", "myhash", "adfa", "myhash"));
  futures.emplace_back(tunnel(leaderID)->exec("del", "myhash", "myset"));

  ASSERT_REPLY(futures[0], "OK");
  ASSERT_REPLY(futures[1], make_vec("myhash", "myset", "mystring"));
  ASSERT_REPLY(futures[2], 4);
  ASSERT_REPLY(futures[3], 3);
  ASSERT_REPLY(futures[4], 0);
  ASSERT_REPLY(futures[5], 0);

  futures.clear();
  futures.emplace_back(tunnel(leaderID)->exec("set", "a", "aa"));
  futures.emplace_back(tunnel(leaderID)->exec("set", "aa", "a"));
  futures.emplace_back(tunnel(leaderID)->exec("get", "a"));
  futures.emplace_back(tunnel(leaderID)->exec("del", "a"));
  futures.emplace_back(tunnel(leaderID)->exec("get", "aa"));
  futures.emplace_back(tunnel(leaderID)->exec("keys", "*"));

  ASSERT_REPLY(futures[0], "OK");
  ASSERT_REPLY(futures[1], "OK");
  ASSERT_REPLY(futures[2], "aa");
  ASSERT_REPLY(futures[3], 1);
  ASSERT_REPLY(futures[4], "a");
  ASSERT_REPLY(futures[5], make_vec("aa"));

  futures.clear();
  futures.emplace_back(tunnel(leaderID)->exec("config_getall"));
  futures.emplace_back(tunnel(leaderID)->exec("config_set", "some.config.value", "1234"));
  futures.emplace_back(tunnel(leaderID)->exec("flushall"));
  futures.emplace_back(tunnel(leaderID)->exec("del", "aa"));
  futures.emplace_back(tunnel(leaderID)->exec("config_get", "some.config.value", "1234"));
  futures.emplace_back(tunnel(leaderID)->exec("config_get", "some.config.value"));
  futures.emplace_back(tunnel(leaderID)->exec("config_getall"));

  ASSERT_REPLY(futures[0], "");
  ASSERT_REPLY(futures[1], "OK");
  ASSERT_REPLY(futures[2], "OK");
  ASSERT_REPLY(futures[3], 0);
  ASSERT_REPLY(futures[4], "ERR wrong number of arguments for 'config_get' command");
  ASSERT_REPLY(futures[5], "1234");
  ASSERT_REPLY(futures[6], make_vec("some.config.value", "1234"));

  futures.clear();
  futures.emplace_back(tunnel(leaderID)->exec("hset", "hash", "key1", "v1"));
  futures.emplace_back(tunnel(leaderID)->exec("hset", "hash2", "key1", "v1"));
  futures.emplace_back(tunnel(leaderID)->exec("exists", "hash", "hash2"));
  futures.emplace_back(tunnel(leaderID)->exec("del", "hash"));
  futures.emplace_back(tunnel(leaderID)->exec("raft_info"));
  futures.emplace_back(tunnel(leaderID)->exec("bad_command"));
  futures.emplace_back(tunnel(leaderID)->exec("exists", "hash"));
  futures.emplace_back(tunnel(leaderID)->exec("exists", "hash2"));

  ASSERT_REPLY(futures[0], 1);
  ASSERT_REPLY(futures[1], 1);
  ASSERT_REPLY(futures[2], 2);
  ASSERT_REPLY(futures[3], 1);
  // ignore futures[4]
  ASSERT_REPLY(futures[5], "ERR unknown command 'bad_command'");
  ASSERT_REPLY(futures[6], 0);
  ASSERT_REPLY(futures[7], 1);

  futures.clear();
  futures.emplace_back(tunnel(leaderID)->exec("hmset", "hmset_test", "f1", "v1", "f2", "v2"));
  futures.emplace_back(tunnel(leaderID)->exec("exists", "hmset_test"));
  futures.emplace_back(tunnel(leaderID)->exec("hmset", "test"));
  futures.emplace_back(tunnel(leaderID)->exec("hmset", "hmset_test", "f2", "v3", "f4"));
  futures.emplace_back(tunnel(leaderID)->exec("hget", "hmset_test", "f1"));
  futures.emplace_back(tunnel(leaderID)->exec("hlen", "hmset_test"));
  futures.emplace_back(tunnel(leaderID)->exec("hmset", "hmset_test", "f2", "value2", "f3", "value3"));
  futures.emplace_back(tunnel(leaderID)->exec("hlen", "hmset_test"));
  futures.emplace_back(tunnel(leaderID)->exec("hget", "hmset_test", "f2"));
  futures.emplace_back(tunnel(leaderID)->exec("hmset", "hmset_test", "f3", "v3"));
  futures.emplace_back(tunnel(leaderID)->exec("hget", "hmset_test", "f3"));
  futures.emplace_back(tunnel(leaderID)->exec("hlen", "hmset_test"));

  ASSERT_REPLY(futures[0], "OK");
  ASSERT_REPLY(futures[1], 1);
  ASSERT_REPLY(futures[2], "ERR wrong number of arguments for 'hmset' command");
  ASSERT_REPLY(futures[3], "ERR wrong number of arguments for 'hmset' command");
  ASSERT_REPLY(futures[4], "v1");
  ASSERT_REPLY(futures[5], 2);
  ASSERT_REPLY(futures[6], "OK");
  ASSERT_REPLY(futures[7], 3);
  ASSERT_REPLY(futures[8], "value2");
  ASSERT_REPLY(futures[9], "OK");
  ASSERT_REPLY(futures[10], "v3");
  ASSERT_REPLY(futures[11], 3);

  futures.clear();
  futures.emplace_back(tunnel(leaderID)->exec("lpush", "list_test", "i1", "i2", "i3", "i4"));
  futures.emplace_back(tunnel(leaderID)->exec("exists", "list_test"));
  futures.emplace_back(tunnel(leaderID)->exec("llen", "list_test"));
  futures.emplace_back(tunnel(leaderID)->exec("lpop", "list_test"));
  futures.emplace_back(tunnel(leaderID)->exec("llen", "list_test"));
  futures.emplace_back(tunnel(leaderID)->exec("rpop", "list_test"));
  futures.emplace_back(tunnel(leaderID)->exec("llen", "list_test"));
  futures.emplace_back(tunnel(leaderID)->exec("del", "list_test"));
  futures.emplace_back(tunnel(leaderID)->exec("llen", "list_test"));
  futures.emplace_back(tunnel(leaderID)->exec("lpop", "list_test"));
  futures.emplace_back(tunnel(leaderID)->exec("rpush", "list_test", "i5", "i6", "i7", "i8"));
  futures.emplace_back(tunnel(leaderID)->exec("set", "list_test", "asdf"));
  futures.emplace_back(tunnel(leaderID)->exec("lpop", "list_test"));
  futures.emplace_back(tunnel(leaderID)->exec("rpop", "list_test"));
  futures.emplace_back(tunnel(leaderID)->exec("rpop", "list_test"));
  futures.emplace_back(tunnel(leaderID)->exec("lpop", "list_test"));
  futures.emplace_back(tunnel(leaderID)->exec("set", "list_test", "asdf"));
  futures.emplace_back(tunnel(leaderID)->exec("lpop", "list_test"));

  int i = 0;
  ASSERT_REPLY(futures[i++], 4);
  ASSERT_REPLY(futures[i++], 1);
  ASSERT_REPLY(futures[i++], 4);
  ASSERT_REPLY(futures[i++], "i4");
  ASSERT_REPLY(futures[i++], 3);
  ASSERT_REPLY(futures[i++], "i1");
  ASSERT_REPLY(futures[i++], 2);
  ASSERT_REPLY(futures[i++], 1);
  ASSERT_REPLY(futures[i++], 0);
  ASSERT_NIL(futures[i++]);
  ASSERT_REPLY(futures[i++], 4);
  ASSERT_REPLY(futures[i++], "ERR Invalid argument: WRONGTYPE Operation against a key holding the wrong kind of value");
  ASSERT_REPLY(futures[i++], "i5");
  ASSERT_REPLY(futures[i++], "i8");
  ASSERT_REPLY(futures[i++], "i7");
  ASSERT_REPLY(futures[i++], "i6");
  ASSERT_REPLY(futures[i++], "OK");
  ASSERT_REPLY(futures[i++], "ERR Invalid argument: WRONGTYPE Operation against a key holding the wrong kind of value");

  // Now test qclient callbacks, ensure things stay reasonable when we mix them
  // with futures.
  TrivialQCallback c1;
  tunnel(leaderID)->execCB(&c1, "set", "qcl-counter", "1");

  TrivialQCallback c2;
  tunnel(leaderID)->execCB(&c2, "get", "qcl-counter");

  std::future<redisReplyPtr> fut1 = tunnel(leaderID)->exec("get", "qcl-counter");
  std::future<redisReplyPtr> fut2 = tunnel(leaderID)->exec("set", "qcl-counter", "2");
  std::future<redisReplyPtr> fut3 = tunnel(leaderID)->exec("get", "qcl-counter");

  TrivialQCallback c3;
  tunnel(leaderID)->execCB(&c3, "get", "qcl-counter");

  TrivialQCallback c4;
  tunnel(leaderID)->execCB(&c4, "set", "qcl-counter", "3");

  TrivialQCallback c5;
  tunnel(leaderID)->execCB(&c5, "get", "qcl-counter");

  std::future<redisReplyPtr> fut4 = tunnel(leaderID)->exec("get", "qcl-counter");

  ASSERT_REPLY(c1.getFuture(), "OK");
  ASSERT_REPLY(c2.getFuture(), "1");
  ASSERT_REPLY(fut1, "1");
  ASSERT_REPLY(fut2, "OK");
  ASSERT_REPLY(fut3, "2");
  ASSERT_REPLY(c3.getFuture(), "2");
  ASSERT_REPLY(c4.getFuture(), "OK");
  ASSERT_REPLY(c5.getFuture(), "3");
  ASSERT_REPLY(fut4, "3");
}

TEST_F(Raft_e2e, replication_with_trimmed_journal) {
  spinup(0); spinup(1);
  RETRY_ASSERT_TRUE(checkStateConsensus(0, 1));

  int leaderID = getServerID(state(0)->getSnapshot()->leader);
  int firstSlaveID = (leaderID+1)%2;
  ASSERT_GE(leaderID, 0);
  ASSERT_LE(leaderID, 1);

  // First, disable automatic resilvering..
  EncodedConfigChange configChange = raftconfig(leaderID)->setResilveringEnabled(false);
  ASSERT_TRUE(configChange.error.empty());
  ASSERT_REPLY(tunnel(leaderID)->execute(configChange.request), "OK");

  // send off many requests, pipeline them
  std::vector<std::future<redisReplyPtr>> futures;
  for(size_t i = 0; i < testreqs.size(); i++) {
    futures.emplace_back(tunnel(leaderID)->execute(testreqs[i]));
  }

  for(size_t i = 0; i < 2; i++) {
    ASSERT_REPLY(futures[i], "OK");
  }

  for(size_t i = 2; i < futures.size(); i++) {
    ASSERT_REPLY(futures[i], 1);
  }

  // now let's trim leader's journal..
  journal(leaderID)->trimUntil(4);

  // and verify it's NOT possible to bring node #2 up to date
  spinup(2);
  RETRY_ASSERT_TRUE(checkStateConsensus(0, 1, 2));

  ASSERT_EQ(journal(2)->getLogSize(), 1);
  ASSERT_EQ(journal(2)->getLogStart(), 0);

  // a divine intervention fills up the missing entries in node #2 journal
  for(size_t i = 1; i < 5; i++) {
    RaftEntry entry;
    ASSERT_TRUE(journal(firstSlaveID)->fetch(i, entry).ok());

    journal(2)->append(i, entry);
  }

  // now verify node #2 can be brought up to date successfully
  RETRY_ASSERT_TRUE(
    journal(0)->getLogSize() == journal(1)->getLogSize() &&
    journal(1)->getLogSize() == journal(2)->getLogSize()
  );

  ASSERT_EQ(journal(2)->getLogSize(), journal(leaderID)->getLogSize());
  ASSERT_EQ(journal(2)->getLogSize(), journal(firstSlaveID)->getLogSize());

  // Verify resilvering didn't happen.
  ASSERT_EQ(journal(2)->getLogStart(), 0);
}

TEST_F(Raft_e2e, membership_updates) {
  spinup(0); spinup(1); spinup(2);
  RETRY_ASSERT_TRUE(checkStateConsensus(0, 1, 2));
  int leaderID = getServerID(state(0)->getSnapshot()->leader);
  ASSERT_REPLY(tunnel(leaderID)->exec("set", "pi", "3.141516"), "OK");

  // throw a node out of the cluster
  int victim = (leaderID+1) % 3;
  RETRY_ASSERT_TRUE(checkFullConsensus(0, 1, 2));
  int index = journal(leaderID)->getLogSize() - 1;
  ASSERT_REPLY(tunnel(leaderID)->exec("RAFT_REMOVE_MEMBER", myself(victim).toString()), "OK");
  RETRY_ASSERT_TRUE(dispatcher(leaderID)->info().commitIndex == index + 1);

  // verify the cluster has not been disrupted
  ASSERT_EQ(state(leaderID)->getSnapshot()->leader, myself(leaderID));

  // add it back as an observer, verify consensus
  ASSERT_REPLY(tunnel(leaderID)->exec("RAFT_ADD_OBSERVER", myself(victim).toString()), "OK");

  RETRY_ASSERT_TRUE(dispatcher(0)->info().commitIndex == index + 2);
  RETRY_ASSERT_TRUE(dispatcher(1)->info().commitIndex == index + 2);
  RETRY_ASSERT_TRUE(dispatcher(2)->info().commitIndex == index + 2);

  ASSERT_EQ(state(victim)->getSnapshot()->status, RaftStatus::FOLLOWER);

  ASSERT_EQ(state(0)->getSnapshot()->leader, state(1)->getSnapshot()->leader);
  ASSERT_EQ(state(1)->getSnapshot()->leader, state(2)->getSnapshot()->leader);

  ASSERT_EQ(journal(0)->getLogSize(), journal(1)->getLogSize());
  ASSERT_EQ(journal(1)->getLogSize(), journal(2)->getLogSize());

  // cannot be a leader, it's an observer
  ASSERT_NE(state(0)->getSnapshot()->leader, myself(victim));

  // add back as a full voting member
  leaderID = getServerID(state(0)->getSnapshot()->leader);
  ASSERT_REPLY(tunnel(leaderID)->exec("RAFT_PROMOTE_OBSERVER", myself(victim).toString()), "OK");
  RETRY_ASSERT_TRUE(dispatcher(leaderID)->info().commitIndex == index + 3);
  RETRY_ASSERT_TRUE(checkStateConsensus(0, 1, 2));
}

TEST_F(Raft_e2e, reject_dangerous_membership_update) {
  spinup(0); spinup(1);
  RETRY_ASSERT_TRUE(checkFullConsensus(0, 1));
  int leaderID = getLeaderID();

  // make sure dangerous node removal is prevented
  int victim = (leaderID+1) % 2;
  redisReplyPtr reply = tunnel(leaderID)->exec("RAFT_REMOVE_MEMBER", myself(victim).toString()).get();
  ASSERT_ERR(reply, "ERR membership update blocked, new cluster would not have an up-to-date quorum");

  // Try to remove a non-existent node
  ASSERT_REPLY(tunnel(leaderID)->exec("RAFT_REMOVE_MEMBER", RaftServer("random_host", 123).toString()),
    "ERR random_host:123 is neither an observer nor a full node.");

  // Make sure we can remove the third node
  ASSERT_REPLY(tunnel(leaderID)->exec("RAFT_REMOVE_MEMBER", myself(2).toString()), "OK");
  RaftMembership membership = journal(leaderID)->getMembership();
  RETRY_ASSERT_TRUE(journal(leaderID)->getCommitIndex() == membership.epoch);

  // Add it back as observer
  ASSERT_REPLY(tunnel(leaderID)->exec("RAFT_ADD_OBSERVER", myself(2).toString()), "OK");
  membership = journal(leaderID)->getMembership();
  RETRY_ASSERT_TRUE(journal(leaderID)->getCommitIndex() == membership.epoch);

  // Remove it again
  ASSERT_REPLY(tunnel(leaderID)->exec("RAFT_REMOVE_MEMBER", myself(2).toString()), "OK");
  membership = journal(leaderID)->getMembership();
  RETRY_ASSERT_TRUE(journal(leaderID)->getCommitIndex() == membership.epoch);
}

TEST_F(Raft_e2e5, membership_updates_with_disruptions) {
  // let's get this party started
  spinup(0); spinup(1); spinup(2); spinup(3);
  RETRY_ASSERT_TRUE(checkStateConsensus(0, 1, 2, 3));

  // throw node #4 out of the cluster
  int leaderID = getServerID(state(0)->getSnapshot()->leader);
  ASSERT_REPLY(tunnel(leaderID)->exec("RAFT_REMOVE_MEMBER", myself(4).toString()), "OK");

  // wait until membership update has been committed
  RaftMembership membership = journal(leaderID)->getMembership();
  ASSERT_GT(membership.epoch, 0u);
  ASSERT_EQ(membership.nodes.size(), 4u);
  RETRY_ASSERT_TRUE(journal(leaderID)->getCommitIndex() == membership.epoch);

  // .. and now spinup node #4 :> Ensure it doesn't disrupt the current leader
  spinup(4);
  std::this_thread::sleep_for(raftclock()->getTimeouts().getHigh()*2);
  ASSERT_EQ(leaderID, getServerID(state(0)->getSnapshot()->leader));

  // verify the cluster has not been disrupted
  ASSERT_EQ(state(leaderID)->getSnapshot()->leader, myself(leaderID));

  // remove one more node
  int victim = (leaderID+1) % 5;
  if(victim == 4) victim = 2;

  ASSERT_REPLY(tunnel(leaderID)->exec("RAFT_REMOVE_MEMBER", myself(victim).toString()), "OK");
  std::this_thread::sleep_for(raftclock()->getTimeouts().getHigh()*2);

  // verify the cluster has not been disrupted
  ASSERT_EQ(state(leaderID)->getSnapshot()->leader, myself(leaderID));

  // issue a bunch of writes and reads
  ASSERT_REPLY(tunnel(leaderID)->exec("set", "123", "abc"), "OK");
  ASSERT_REPLY(tunnel(leaderID)->exec("get", "123"), "abc");
}

TEST_F(Raft_e2e, leader_steps_down_after_follower_loss) {
  // cluster with 2 nodes
  spinup(0); spinup(1);
  RETRY_ASSERT_TRUE(checkStateConsensus(0, 1));

  int leaderID = getLeaderID();
  ASSERT_GE(leaderID, 0);
  ASSERT_LE(leaderID, 1);

  RaftTerm term = state(leaderID)->getCurrentTerm();

  int followerID = (leaderID + 1)%2;
  spindown(followerID);

  RETRY_ASSERT_TRUE(term < state(leaderID)->getCurrentTerm());
  ASSERT_TRUE(state(leaderID)->getSnapshot()->leader.empty());
}

TEST_F(Raft_e2e, stale_reads) {
  spinup(0); spinup(1); spinup(2);
  RETRY_ASSERT_TRUE(checkStateConsensus(0, 1, 2));

  int leaderID = getLeaderID();
  int follower = (getLeaderID() + 1) % 3;

  ASSERT_REPLY(tunnel(leaderID)->exec("set", "abc", "1234"), "OK");
  ASSERT_REPLY(tunnel(follower)->exec("get", "abc"), SSTR("MOVED 0 " << myself(leaderID).toString()));

  ASSERT_REPLY(tunnel(follower)->exec("activate-stale-reads"), "OK");

  redisReplyPtr reply = tunnel(follower)->exec("get", "abc").get();
  qdb_info("Race-y read: " << std::string(reply->str, reply->len));

  RETRY_ASSERT_TRUE(checkFullConsensus(0, 1, 2));
  ASSERT_REPLY(tunnel(follower)->exec("get", "abc"), "1234");
}

TEST_F(Raft_e2e, monitor) {
  spinup(0); spinup(1); spinup(2);
  RETRY_ASSERT_TRUE(checkStateConsensus(0, 1, 2));

  int leaderID = getLeaderID();

  // We can't use QClient for this, it can't handle the output of MONITOR
  qclient::ConnectionInitiator initiator("localhost", myself(leaderID).port);
  ASSERT_TRUE(initiator.ok());

  Link link(initiator.getFd());
  BufferedReader reader(&link);

  ASSERT_EQ(link.Send("*1\r\n$7\r\nMONITOR\r\n"), 17);
  ASSERT_EQ(link.Send("random string"), 13);

  std::string response;
  RETRY_ASSERT_TRUE(reader.consume(5, response));
  ASSERT_EQ(response, "+OK\r\n");

  tunnel(leaderID)->exec("set", "abc", "aaaa" "\xab" "bbb");
  response.clear();
  RETRY_ASSERT_TRUE(reader.consume(28, response));
  ASSERT_EQ(response, "+\"set\" \"abc\" \"aaaa\\xABbbb\"\r\n");

  tunnel(leaderID)->exec("get", "abc");
  response.clear();
  RETRY_ASSERT_TRUE(reader.consume(14, response));
  ASSERT_EQ(response, "+\"get\" \"abc\"\r\n");
}

TEST_F(Raft_e2e, hincrbymulti) {
  spinup(0); spinup(1); spinup(2);
  RETRY_ASSERT_TRUE(checkStateConsensus(0, 1, 2));

  int leaderID = getLeaderID();

  ASSERT_REPLY(tunnel(leaderID)->exec("hincrbymulti", "h1", "h2", "3", "h2", "h3", "4"), 7);
  ASSERT_REPLY(tunnel(leaderID)->exec("hget", "h1", "h2"), "3");
  ASSERT_REPLY(tunnel(leaderID)->exec("hget", "h2", "h3"), "4");

  ASSERT_REPLY(tunnel(leaderID)->exec("hincrbymulti", "h1", "h2", "-5", "h2", "h3", "20", "h4", "h8"), "ERR wrong number of arguments for 'hincrbymulti' command");
  ASSERT_REPLY(tunnel(leaderID)->exec("hincrbymulti", "h1", "h2", "-5", "h2", "h3", "20", "h4", "h8", "13"), 35);

  ASSERT_REPLY(tunnel(leaderID)->exec("hget", "h1", "h2"), "-2");
  ASSERT_REPLY(tunnel(leaderID)->exec("hget", "h2", "h3"), "24");
  ASSERT_REPLY(tunnel(leaderID)->exec("hget", "h4", "h8"), "13");
}

TEST_F(Raft_e2e, sscan) {
  spinup(0); spinup(1); spinup(2);
  RETRY_ASSERT_TRUE(checkStateConsensus(0, 1, 2));

  int leaderID = getLeaderID();

  redisReplyPtr reply = tunnel(leaderID)->exec("sscan", "myset", "0", "asdf", "123").get();
  ASSERT_ERR(reply, "ERR syntax error");

  ASSERT_REPLY(tunnel(leaderID)->exec("sadd", "myset", "a", "b", "c", "d", "e", "f", "g"), 7);

  reply = tunnel(leaderID)->exec("sscan", "myset", "0", "COUNT", "3").get();
  ASSERT_REPLY(reply, std::make_pair("next:d", make_vec("a", "b", "c")));

  reply = tunnel(leaderID)->exec("sscan", "myset", "next:d", "COUNT", "2").get();
  ASSERT_REPLY(reply, std::make_pair("next:f", make_vec("d", "e")));

  reply = tunnel(leaderID)->exec("sscan", "myset", "next:f", "COUNT", "2").get();
  ASSERT_REPLY(reply, std::make_pair("0", make_vec("f", "g")));

  reply = tunnel(leaderID)->exec("sscan", "myset", "next:zz").get();
  ASSERT_REPLY(reply, std::make_pair("0", make_vec()));

  reply = tunnel(leaderID)->exec("sscan", "not-existing", "next:zz").get();
  ASSERT_REPLY(reply, std::make_pair("0", make_vec()));

  QSet qset(*tunnel(leaderID), "myset");
  auto pair = qset.sscan("0", 2);

  ASSERT_EQ(pair.first, "next:c");
  ASSERT_EQ(pair.second, make_vec("a", "b"));

  pair = qset.sscan(pair.first, 2);
  ASSERT_EQ(pair.first, "next:e");
  ASSERT_EQ(pair.second, make_vec("c", "d"));

  QSet qset2(*tunnel(leaderID), "not-existing");
  pair = qset2.sscan("0", 2);

  ASSERT_EQ(pair.first, "0");
  ASSERT_EQ(pair.second, make_vec());
}
