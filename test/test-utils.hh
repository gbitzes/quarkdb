// ----------------------------------------------------------------------
// File: test-utils.hh
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

#ifndef QUARKDB_TEST_UTILS_HH
#define QUARKDB_TEST_UTILS_HH

#include <sys/socket.h>
#include <sys/un.h>
#include <vector>
#include "Common.hh"
#include "netio/AsioPoller.hh"
#include "raft/RaftState.hh"
#include <qclient/QClient.hh>
#include <gtest/gtest.h>
#include "config/test-config.hh"
#include "utils/FileUtils.hh"
#include "StateMachine.hh"
#include "raft/RaftJournal.hh"

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

namespace quarkdb {

//------------------------------------------------------------------------------
// Forward declarations
//------------------------------------------------------------------------------
class Shard; class RaftGroup; class ShardDirectory; class RaftJournal;
class RaftDispatcher; class RaftLease; class RaftDirector;
class RaftCommitTracker; class RaftConfig; class RaftTrimmer;
class QuarkDBNode; class RaftContactDetails;
class Publisher;

#define RETRY_ASSERT_TRUE_3(cond, retry, waitInterval) { \
  bool ok = false; \
  size_t nretries = 0; \
  while(nretries++ < retry) { \
    std::this_thread::sleep_for(std::chrono::milliseconds(waitInterval)); \
    if((cond)) { \
      qdb_info("Condition '" << #cond << "' is true after " << nretries << " attempts"); \
      ok = true; \
      break; \
    } \
  } \
  if(!ok) { ASSERT_TRUE(cond) << " - failure after " << nretries << " retries "; } \
}

#define RETRY_ASSERT_EQ_3(cond1, cond2, retry, waitInterval) { \
  bool ok = false; \
  size_t nretries = 0; \
  while(nretries++ < retry) { \
    std::this_thread::sleep_for(std::chrono::milliseconds(waitInterval)); \
    if( (cond1) == (cond2) ) { \
      qdb_info("Condition '" << #cond1 << " == " << #cond2 << " is true after " << nretries << " attempts"); \
      ok = true; \
      break; \
    } \
  } \
  if(!ok) { ASSERT_EQ(cond1, cond2) << " - failure after " << nretries << " retries "; } \
}

#define RETRY_ASSERT_NE_3(cond1, cond2, retry, waitInterval) { \
  bool ok = false; \
  size_t nretries = 0; \
  while(nretries++ < retry) { \
    std::this_thread::sleep_for(std::chrono::milliseconds(waitInterval)); \
    if( (cond1) != (cond2) ) { \
      qdb_info("Condition '" << #cond1 << " != " << #cond2 << " is true after " << nretries << " attempts"); \
      ok = true; \
      break; \
    } \
  } \
  if(!ok) { ASSERT_NE(cond1, cond2) << " - failure after " << nretries << " retries "; } \
}

#define RETRY_ASSERT_TRUE_SPIN(cond) RETRY_ASSERT_TRUE_3(cond, 100000, 0)
#define RETRY_ASSERT_EQ_SPIN(cond1, cond2) RETRY_ASSERT_EQ_3(cond1, cond2, 100000, 0)

#define NUMBER_OF_RETRIES ( (size_t) timeouts().getLow().count() * 10)

// retry every 10ms
#define RETRY_ASSERT_TRUE(cond) RETRY_ASSERT_TRUE_3(cond, NUMBER_OF_RETRIES, 10)
#define RETRY_ASSERT_EQ(cond1, cond2) RETRY_ASSERT_EQ_3(cond1, cond2, NUMBER_OF_RETRIES, 10)
#define RETRY_ASSERT_NE(cond1, cond2) RETRY_ASSERT_NE_3(cond1, cond2, NUMBER_OF_RETRIES, 10)

#define RETRY_ASSERT_TRUE_20MIN(cond) RETRY_ASSERT_TRUE_3(cond, 100*60*20, 10)

extern std::vector<RedisRequest> testreqs;

// necessary because C macros are dumb and don't undestand
// universal initialization with brackets {}
template<typename... Args>
RedisRequest make_req(Args... args) {
  return RedisRequest { args... };
}

template<typename... Args>
std::vector<std::string> make_vec(Args... args) {
  return std::vector<std::string> { args... };
}

// Yes, passing a callback to QClient to convert it into a future is really
// stupid, since QClient supports futures natively. This is used to test that
// qclient callbacks work as they should..
class TrivialQCallback : public qclient::QCallback {
public:
  TrivialQCallback() {}
  virtual ~TrivialQCallback() {}
  virtual void handleResponse(qclient::redisReplyPtr &&reply) override {
    promise.set_value(std::move(reply));
  }

  std::future<qclient::redisReplyPtr> getFuture() {
    return promise.get_future();
  }

private:
  std::promise<qclient::redisReplyPtr> promise;
};


class GlobalEnv : public testing::Environment {
public:
  virtual void SetUp() override;
  virtual void TearDown() override;

  // Initialize a *clean* Shard Directory. The connection to the dbs is cached,
  // because even if rocksdb is local, it takes a long time to open.
  // (often 50+ ms)

  ShardDirectory* getShardDirectory(const std::string &path, RaftClusterID clusterID, const std::vector<RaftServer> &nodes);
  void clearConnectionCache();
  const std::string testdir = "/tmp/quarkdb-tests";
  static RaftServer server(int id);
private:
  std::map<std::string, ShardDirectory*> shardDirCache;
};
extern GlobalEnv &commonState;

// Includes everything needed to simulate a single raft-enabled server.
// Everything is initialized lazily, so if you only want to test the journal for example,
// this is possible, too. Just don't call eg group()->director(), and you won't have to worry
// about raft messing up your variables and terms due to timeouts.
class TestNode {
public:
  TestNode(RaftServer myself, RaftClusterID clusterID, RaftTimeouts timeouts, const std::vector<RaftServer> &nodes);
  ~TestNode();

  QuarkDBNode* quarkdbNode();
  ShardDirectory* shardDirectory();
  Shard* shard();
  RaftGroup* group();
  AsioPoller *poller();
  qclient::QClient *tunnel();
  qclient::Options makeNoRedirectOptions();
  std::unique_ptr<qclient::Handshake> makeQClientHandshake();

  RaftServer myself();
  std::vector<RaftServer> nodes();
  qclient::Members members();

  void spinup();
  void spindown();
  void killTunnel();
private:
  RaftServer myselfSrv;
  RaftClusterID clusterID;
  std::vector<RaftServer> initialNodes;

  QuarkDBNode *qdbnodeptr = nullptr;
  ShardDirectory *sharddirptr = nullptr;
  AsioPoller *pollerptr = nullptr;
  qclient::QClient *tunnelptr = nullptr;
};

// Contains everything needed to simulate a cluster with an arbitrary number of nodes.
// Everything is initialized lazily, including the nodes of the cluster themselves.
class TestCluster {
public:
  TestCluster(RaftTimeouts timeouts, RaftClusterID clusterID,
    const std::vector<RaftServer> &nodes, int initialActiveNodes = -1);

  TestCluster(RaftClusterID clusterID, const std::vector<RaftServer> &nodes,
    int initialActiveNodes = -1);
  ~TestCluster();

  ShardDirectory* shardDirectory(int id = 0);
  StateMachine* stateMachine(int id = 0);
  RaftJournal* journal(int id = 0);
  RaftDispatcher *dispatcher(int id = 0);
  RaftState *state(int id = 0);
  AsioPoller *poller(int id = 0);
  RaftServer myself(int id = 0);
  RaftDirector *director(int id = 0);
  qclient::QClient *tunnel(int id = 0);
  RaftHeartbeatTracker *heartbeatTracker(int id = 0);
  RaftLease *lease(int id = 0);
  RaftCommitTracker *commitTracker(int id = 0);
  RaftConfig *raftconfig(int id = 0);
  RaftTrimmer* trimmer(int id = 0);
  const RaftContactDetails* contactDetails(int id = 0);
  Publisher* publisher(int id = 0);
  RaftTimeouts timeouts();

  // issue manual vote, with a pre-vote test before that.
  // ensure pre-vote and vote match, and that pre-vote does not advance
  // raft state.
  RaftVoteResponse issueManualVote(const RaftVoteRequest &votereq, int id = 0);

  qclient::Options makeNoRedirectOptions(int id = 0);
  std::unique_ptr<qclient::Handshake> makeQClientHandshake(int id = 0);

  void killTunnel(int id = 0);

  // manage node state
  void spinup(int id);
  void spindown(int id);

  // In some tests, the latency of opening rocksdb can kill us, since by the
  // time the db is open raft starts timing out.
  // This function will prepare a node, so that spinning it up later is instant.
  void prepare(int id);

  // initialize nodes using information passed on the nodes variable, except if srv is set
  TestNode* node(int id = 0, const RaftServer &srv = {});
  std::vector<RaftServer> nodes(int id = 0);
  qclient::Members members(int id = 0);
  RaftClusterID clusterID();
  size_t getClusterSize() const;

  template<typename... Args>
  bool checkValueConsensus(const std::string &key, const std::string &value, const Args... args) {
    std::vector<int> arguments = { args... };

    for(size_t i = 0; i < arguments.size(); i++) {
      std::string tmp;
      rocksdb::Status st = stateMachine(arguments[i])->get(key, tmp);

      if(!st.ok()) return false;
      if(tmp != value) return false;
    }

    return true;
  }

  // Check whether the given nodes have reached full consensus - this means:
  // - State consensus
  // - All journals are the same size, with all entries committed
  // - All state machines have already applied all entries in the journal
  template<typename... Args>
  bool checkFullConsensus(const Args... args) {
    if(!checkStateConsensusQuiet(args...)) return false;
    std::vector<int> arguments = { args... };

    LogIndex targetEntry = journal(arguments[0])->getLogSize() - 1;

    // Ensure all journals and state machines are at 'targetEntry'
    for(size_t i = 0; i < arguments.size(); i++) {
      if(journal(arguments[i])->getLogSize()-1 != targetEntry) return false;
      if(journal(arguments[i])->getCommitIndex() != targetEntry) return false;
      if(stateMachine(arguments[i])->getLastApplied() != targetEntry) return false;
    }

    qdb_info("Achieved full consensus with journal size " << targetEntry);
    return true;
  }

  template<typename... Args>
  bool crossCheckJournals(const Args... args) {
    // Check journal contents, validate they're equal across all nodes.
    // If one journal was trimmed further than some other, we only check the
    // entries which exist across all journals.

    std::vector<int> arguments = { args... };

    LogIndex endingPoint = journal(arguments[0])->getLogSize();
    LogIndex startingPoint = journal(arguments[0])->getLogStart();

    for(size_t i = 0; i < arguments.size(); i++) {
      startingPoint = std::max(startingPoint, journal(arguments[i])->getLogStart());
    }

    qdb_info("Cross-checking journals from entry #" << startingPoint << " to #" << endingPoint - 1);

    for(LogIndex index = startingPoint; index < endingPoint; index++) {
      RaftEntry entry;
      rocksdb::Status st = journal(arguments[0])->fetch(index, entry);
      if(!st.ok()) return false;

      if(!validateSingleEntry(index, entry.term, entry.request, args...)) {
        return false;
      }
    }

    qdb_info("Journal cross-checking successful!");
    return true;
  }

  template<typename... Args>
  bool validateSingleEntry(LogIndex index, RaftTerm term, const RedisRequest &request, const Args... args) {
    std::vector<int> arguments = { args... };

    for(size_t i = 0; i < arguments.size(); i++) {
      RaftEntry entry;

      rocksdb::Status st = journal(arguments[i])->fetch(index, entry);
      if(!st.ok()) return false;

      if(entry.request != request) {
        return false;
      }

      if(term >= 0 && entry.term != term) {
        return false;
      }
    }

    return true;
  }

  template<typename... Args>
  bool checkStateConsensusWithSnapshots(bool quiet, std::vector<RaftStateSnapshotPtr> &snapshots,
    const Args... args) {

    std::vector<int> arguments = { args... };
    snapshots.resize(arguments.size());

    for(size_t i = 0; i < arguments.size(); i++) {
      snapshots[i] = state(arguments[i])->getSnapshot();
    }

    for(size_t i = 1; i < snapshots.size(); i++) {
      if(snapshots[i]->leader.empty() || snapshots[i-1]->leader.empty()) return false;
      if(snapshots[i]->term != snapshots[i-1]->term) return false;
      if(snapshots[i]->leader != snapshots[i-1]->leader) return false;
    }

    // Exactly one should be leader, others followers
    size_t leaders = 0;
    size_t followers = 0;

    for(size_t i = 0; i < snapshots.size(); i++) {
      if(snapshots[i]->status == RaftStatus::LEADER) {
        leaders++;
      }
      else if(snapshots[i]->status == RaftStatus::FOLLOWER) {
        followers++;
      }
    }

    if(leaders != 1u) {
      return false;
    }

    if(followers != snapshots.size() - 1) {
      return false;
    }

    if(!quiet) {
      qdb_info("Achieved state consensus for term " << snapshots[0]->term << " with leader " << snapshots[0]->leader.toString());
    }

    return true;
  }

  template<typename... Args>
  bool checkStateConsensus(const Args... args) {
    std::vector<RaftStateSnapshotPtr> snapshots;
    return checkStateConsensusWithSnapshots(false, snapshots, args...);
  }

  template<typename... Args>
  bool checkStateConsensusQuiet(const Args... args) {
    std::vector<RaftStateSnapshotPtr> snapshots;
    return checkStateConsensusWithSnapshots(true, snapshots, args...);
  }


  int getServerID(const RaftServer &srv);
  std::vector<RaftServer> retrieveLeaders();
  int getLeaderID();

  qclient::SubscriptionOptions reasonableSubscriptionOptions(bool pushtypes = false);

private:
  std::string rocksdbPath(int id = 0);

  RaftClusterID clusterid;
  RaftTimeouts clusterTimeouts;

  // The list of nodes which are initially part of the cluster.
  std::vector<RaftServer> initialNodes;
  // The complete list of nodes we're playing with. Not all of them may be
  // officially part of the cluster, at first.
  std::vector<RaftServer> allNodes;

  std::map<int, TestNode*> testnodes;
};

// Given a test cluster, shut down the leader on regular intervals.
class ClusterDestabilizer {
public:
  ClusterDestabilizer(TestCluster *testCluster);
  ~ClusterDestabilizer();

  void main(ThreadAssistant &assistant);

private:
  TestCluster *mTestCluster;
  AssistedThread mThread;
};

// Convenience classes. Want to run tests on a simulated cluster of 3 nodes?
// Inherit your test fixture from here.
class TestCluster3Nodes : public TestCluster {
public:
  TestCluster3Nodes() : TestCluster("a9b9e979-5428-42e9-8a52-f675c39fdf80", {
    GlobalEnv::server(0),
    GlobalEnv::server(1),
    GlobalEnv::server(2)
  }) { };
};

// Just like the above, but with relaxed raft timeouts
class TestCluster3NodesRelaxedTimeouts : public TestCluster {
public:
  TestCluster3NodesRelaxedTimeouts() : TestCluster(relaxedTimeouts,
    "a9b9e979-5428-42e9-8a52-f675c39fdf80", {
    GlobalEnv::server(0),
    GlobalEnv::server(1),
    GlobalEnv::server(2)
  }) { };
};


class TestCluster5Nodes : public TestCluster {
public:
  TestCluster5Nodes() : TestCluster("a9b9e979-5428-42e9-8a52-f675c39fdf80", {
    GlobalEnv::server(0),
    GlobalEnv::server(1),
    GlobalEnv::server(2),
    GlobalEnv::server(3),
    GlobalEnv::server(4)
  }) { };
};

// A fixture which provides up to 10 raft nodes, but is initialized with
// just a single one.
class TestCluster10Nodes1Initial : public TestCluster {
public:
  TestCluster10Nodes1Initial() : TestCluster("a9b9e979-5428-42e9-8a52-f675c39fdf80", {
    GlobalEnv::server(0),
    GlobalEnv::server(1),
    GlobalEnv::server(2),
    GlobalEnv::server(3),
    GlobalEnv::server(4),
    GlobalEnv::server(5),
    GlobalEnv::server(6),
    GlobalEnv::server(7),
    GlobalEnv::server(8),
    GlobalEnv::server(9)
  }, 1) { };
};

class TestCluster3NodesFixture : public TestCluster3Nodes, public ::testing::Test {};
class TestCluster3NodesRelaxedTimeoutsFixture : public TestCluster3NodesRelaxedTimeouts, public ::testing::Test {};
class TestCluster5NodesFixture : public TestCluster5Nodes, public ::testing::Test {};
class TestCluster10Nodes1InitialFixture : public TestCluster10Nodes1Initial, public ::testing::Test {};

class SocketListener {
private:
  int s;
  struct sockaddr_in remote;
public:
  SocketListener(int port) {
    struct addrinfo hints, *servinfo, *p;
    int rv, yes = 1;

    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    if ((rv = getaddrinfo(NULL, std::to_string(port).c_str(), &hints, &servinfo)) != 0) {
      fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(rv));
      exit(1);
    }

    // loop through all the results and bind to the first we can
    for(p = servinfo; p != NULL; p = p->ai_next) {
      if ((s = socket(p->ai_family, p->ai_socktype, p->ai_protocol)) == -1) {
        perror("server: socket");
        continue;
      }
      if (setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &yes,sizeof(int)) == -1) {
        perror("setsockopt");
        exit(1);
      }
      if (bind(s, p->ai_addr, p->ai_addrlen) == -1) {
        close(s);
        perror("server: bind");
        continue;
      }
      break;
    }

    freeaddrinfo(servinfo); // all done with this structure

    if (p == NULL) {
      fprintf(stderr, "server: failed to bind\n");
      exit(1);
    }

    if (listen(s, 10) == -1) {
      perror("listen");
      exit(1);
    }
  }
  ~SocketListener() {
    ::shutdown(s, SHUT_RDWR);
    close(s);
  }

  int accept() {
    socklen_t remoteSize = sizeof(remote);
    return ::accept(s, (struct sockaddr *)&remote, &remoteSize);
  }
};

class IptablesHelper {
public:
  bool singleDropPackets(int port);
  bool singleAcceptPackets(int port);

};


}

#endif
