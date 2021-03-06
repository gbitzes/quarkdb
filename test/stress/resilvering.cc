// ----------------------------------------------------------------------
// File: resilvering.cc
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

#include "../test-reply-macros.hh"
#include "../test-utils.hh"
#include "raft/RaftConfig.hh"
#include "ShardDirectory.hh"
#include "StateMachine.hh"
#include "Connection.hh"
#include "raft/RaftResilverer.hh"
#include "raft/RaftJournal.hh"

using namespace quarkdb;
class Trimming : public TestCluster3NodesFixture {};
class Resilvering : public TestCluster3NodesFixture {};

#define ASSERT_NOTFOUND(msg) ASSERT_TRUE(msg.IsNotFound())

TEST_F(Trimming, configurable_trimming_limit) {
  Connection::setPhantomBatchLimit(1);

  spinup(0); spinup(1); spinup(2);
  RETRY_ASSERT_TRUE(checkStateConsensus(0, 1, 2));

  int leaderID = getLeaderID();

  std::vector<std::future<redisReplyPtr>> futures;

  // push lots of updates
  const int64_t NENTRIES = 500;
  for(size_t i = 0; i < NENTRIES; i++) {
    futures.emplace_back(tunnel(leaderID)->exec("set", SSTR("key-" << i), SSTR("value-" << i)));
  }

  // Set journal trim config to ridiculously low values.
  // This is to ensure the trimmer never tries to remove non-committed or non-applied entries.
  // With a sane trim limit in the millions, this would never happen anyway, but let's be paranoid.
  TrimmingConfig trimConfig { 2, 1 };
  EncodedConfigChange configChange = raftconfig(leaderID)->setTrimmingConfig(trimConfig, true);
  ASSERT_TRUE(configChange.error.empty());
  ASSERT_REPLY(tunnel(leaderID)->execute(configChange.request), "OK");

  // some more updates...
  for(size_t i = NENTRIES; i < NENTRIES*2; i++) {
    futures.emplace_back(tunnel(leaderID)->exec("set", SSTR("key-" << i), SSTR("value-" << i)));
  }

  // Get responses
  for(size_t i = 0; i < futures.size(); i++) {
    ASSERT_REPLY(futures[i], "OK");
  }

  RETRY_ASSERT_EQ(journal(0)->getLogStart(), 1000);
  RETRY_ASSERT_EQ(journal(1)->getLogStart(), 1000);
  RETRY_ASSERT_EQ(journal(2)->getLogStart(), 1000);

  RETRY_ASSERT_EQ(stateMachine(0)->getLastApplied(), 1002);
  RETRY_ASSERT_EQ(stateMachine(1)->getLastApplied(), 1002);
  RETRY_ASSERT_EQ(stateMachine(2)->getLastApplied(), 1002);
}

TEST_F(Resilvering, manual) {
  // Don't spin up #2 yet.. We'll try to resilver that node manually later.
  spinup(0); spinup(1);
  RETRY_ASSERT_TRUE(checkStateConsensus(0, 1));

  int leaderID = getLeaderID();

  // push lots of updates
  const int64_t NENTRIES = 5000;
  for(size_t i = 0; i < NENTRIES; i++) {
    ASSERT_REPLY(tunnel(leaderID)->exec("set", SSTR("key-" << i), SSTR("value-" << i)), "OK");
  }

  LogIndex commitIndex = journal(leaderID)->getCommitIndex();

  RETRY_ASSERT_EQ(journal(0)->getCommitIndex(), commitIndex);
  RETRY_ASSERT_EQ(journal(1)->getCommitIndex(), commitIndex);
  ASSERT_EQ(journal(2)->getCommitIndex(), 0);

  // Stop the stable cluster and start node #2
  spindown(0);
  spindown(1);
  spinup(2);

  // Ensure node #2 is empty.
  std::string tmp;
  for(size_t i = 0; i < NENTRIES; i++) {
    ASSERT_NOTFOUND(stateMachine(2)->get(SSTR("key-" << i), tmp));
  }

  // Let's drive the resilvering logic of #2 manually.
  RaftResilverer resilverer(*shardDirectory(0), myself(2), *contactDetails(), *trimmer(0));
  RETRY_ASSERT_EQ(resilverer.getStatus().state, ResilveringState::SUCCEEDED);
  ASSERT_GE(resilverer.getProgress(), 4u);
  qdb_info("Files copied: " << resilverer.getProgress());

  ASSERT_EQ(resilverer.getProgress(), resilverer.getTotalToSend());

  // Ensure the data is there after resilvering.
  for(size_t i = 0; i < NENTRIES; i++) {
    std::string value;
    ASSERT_TRUE(stateMachine(2)->get(SSTR("key-" << i), value).ok());
    ASSERT_EQ(value, SSTR("value-" << i));
  }

  ASSERT_TRUE(journal(2)->getCommitIndex() == commitIndex);
}

TEST_F(Resilvering, automatic) {
  // Don't spin up #2 yet.. Will be resilvered later on.
  spinup(0); spinup(1); prepare(2);
  RETRY_ASSERT_TRUE(checkStateConsensus(0, 1));

  int leaderID = getLeaderID();

  // Lower the journal trim limit, so as to trigger a resilvering.
  TrimmingConfig trimConfig { 1000, 1000 };
  EncodedConfigChange configChange = raftconfig(leaderID)->setTrimmingConfig(trimConfig, true);
  ASSERT_TRUE(configChange.error.empty());
  ASSERT_REPLY(tunnel(leaderID)->execute(configChange.request), "OK");

  // push lots of updates
  const int64_t NENTRIES = 5000;
  for(size_t i = 0; i < NENTRIES; i++) {
    ASSERT_REPLY(tunnel(leaderID)->exec("set", SSTR("key-" << i), SSTR("value-" << i)), "OK");
  }

  RETRY_ASSERT_EQ(journal(0)->getCommitIndex(), NENTRIES+2);
  RETRY_ASSERT_EQ(journal(1)->getCommitIndex(), NENTRIES+2);
  ASSERT_EQ(journal(2)->getCommitIndex(), 0);

  RETRY_ASSERT_EQ(journal(0)->getLogStart(), NENTRIES - 1000);
  RETRY_ASSERT_EQ(journal(1)->getLogStart(), NENTRIES - 1000);
  ASSERT_EQ(journal(2)->getLogStart(), 0);

  const ResilveringHistory &resilveringHistory = shardDirectory(2)->getResilveringHistory();
  ASSERT_EQ(resilveringHistory.size(), 1u);
  ASSERT_EQ(resilveringHistory.at(0).getID(), "GENESIS");

  // Start up node #2, verify it gets resilvered
  spinup(2);

  // Attention here.. when resilvering is in progress, we can't access the journal
  // or state machine. Wait until resilvering is done.

  RETRY_ASSERT_EQ(resilveringHistory.size(), 2u);

  RETRY_ASSERT_EQ(journal(2)->getCommitIndex(), NENTRIES+2);
  RETRY_ASSERT_EQ(journal(2)->getLogStart(), NENTRIES - 1000);

  // Ensure the data is there after resilvering.
  for(size_t i = 0; i < NENTRIES; i++) {
    std::string value;
    ASSERT_TRUE(stateMachine(2)->get(SSTR("key-" << i), value).ok());
    ASSERT_EQ(value, SSTR("value-" << i));
  }
}
