// ----------------------------------------------------------------------
// File: RaftCommon.hh
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

#ifndef __QUARKDB_RAFT_COMMON_H__
#define __QUARKDB_RAFT_COMMON_H__

#include "RedisRequest.hh"
#include "utils/TimeFormatting.hh"
#include "Common.hh"
#include "utils/Macros.hh"
#include "health/HealthIndicator.hh"
#include "Utils.hh"

namespace quarkdb {

enum class RaftStatus {
  LEADER,
  FOLLOWER,
  CANDIDATE,
  SHUTDOWN
};
std::string statusToString(RaftStatus st);

inline void append_int_to_string(int64_t source, std::ostringstream &target) {
  char buff[sizeof(source)];
  memcpy(&buff, &source, sizeof(source));
  target.write(buff, sizeof(source));
}

inline int64_t fetch_int_from_string(const char *pos) {
  int64_t result;
  memcpy(&result, pos, sizeof(result));
  return result;
}

using RaftSerializedEntry = std::string;

struct RaftEntry {
  RaftTerm term;
  RedisRequest request;

  RaftEntry() {}

  RaftEntry(RaftTerm term_, RedisRequest&& req) : term(term_), request(std::move(req)) {}
  RaftEntry(RaftTerm term_, const RedisRequest& req) : term(term_), request(req) {}

  template<typename... Args>
  RaftEntry(RaftTerm term_, Args&&... args) : term(term_), request{args...} {}

  RaftSerializedEntry serialize() const {
    std::ostringstream ss;
    append_int_to_string(term, ss);

    for(size_t i = 0; i < request.size(); i++) {
      append_int_to_string(request[i].size(), ss);
      ss << request[i];
    }

    return ss.str();
  }

  static void deserialize(RaftEntry &entry, std::string_view data) {
    entry.request.clear();
    entry.term = fetch_int_from_string(data.data());

    const char *pos = data.data() + sizeof(term);
    const char *end = data.data() + data.size();

    while(pos < end) {
      int64_t len = fetch_int_from_string(pos);
      pos += sizeof(len);

      entry.request.emplace_back(pos, len);
      pos += len;
    }
  }

  static int64_t fetchTerm(std::string_view data) {
    return fetch_int_from_string(data.data());
  }

  bool operator==(const RaftEntry &rhs) const {
    return term == rhs.term && request == rhs.request;
  }

  bool operator!=(const RaftEntry &rhs) const {
    return !(*this == rhs);
  }
};

struct RaftEntryWithIndex {
  RaftEntry entry;
  LogIndex index;

  RaftEntryWithIndex(const RaftEntry &entr, LogIndex idx)
  : entry(entr), index(idx) {}
};

inline std::ostream& operator<<(std::ostream& out, const RaftEntry& entry) {
  out << "term: " << entry.term << " -> " << entry.request;
  return out;
}

struct RaftHeartbeatRequest {
  RaftTerm term;
  RaftServer leader;
};

// The response to the node which sent us a heartbeat: our current term,
// whether we recognize the heartbeat-sender as leader, and if not, the reason
// why.
struct RaftHeartbeatResponse {
  RaftTerm term;
  bool nodeRecognizedAsLeader;
  std::string err;

  std::vector<std::string> toVector() {
    std::vector<std::string> ret;
    ret.push_back(std::to_string(term));
    ret.push_back(std::to_string(nodeRecognizedAsLeader));
    ret.push_back(err);
    return ret;
  }
};

struct RaftAppendEntriesRequest {
  RaftTerm term;
  RaftServer leader;
  LogIndex prevIndex;
  RaftTerm prevTerm;
  LogIndex commitIndex;

  std::vector<RaftEntry> entries;
};

struct RaftAppendEntriesResponse {
  RaftAppendEntriesResponse(RaftTerm tr, LogIndex ind, bool out, const std::string &er)
  : term(tr), logSize(ind), outcome(out), err(er) {}

  RaftAppendEntriesResponse() {}

  RaftTerm term = -1;
  LogIndex logSize = -1;
  bool outcome = false;
  std::string err;

  std::vector<std::string> toVector() {
    std::vector<std::string> ret;
    ret.push_back(std::to_string(term));
    ret.push_back(std::to_string(logSize));
    ret.push_back(std::to_string(outcome));
    ret.push_back(err);
    return ret;
  }
};

struct RaftVoteRequest {
  RaftTerm term;
  RaftServer candidate;
  LogIndex lastIndex;
  RaftTerm lastTerm;

  std::string describe(bool preVote) const {
    std::ostringstream ss;

    if(preVote) {
      ss << "pre-vote request ";
    }
    else {
      ss << "vote request ";
    }

    ss << "[candidate=" << candidate.toString() <<
          ", term=" << term <<
          ", lastIndex=" << lastIndex <<
          ", lastTerm=" << lastTerm <<
          "]";

    return ss.str();
  }

};

enum class RaftVote {
  VETO = -1,
  REFUSED = 0,
  GRANTED = 1
};

struct RaftVoteResponse {
  RaftVoteResponse(RaftTerm tr, RaftVote vt) : term(tr), vote(vt) {}
  RaftVoteResponse() : term(0), vote(RaftVote::VETO) {}

  RaftTerm term;
  RaftVote vote;

  std::vector<std::string> toVector() {
    std::vector<std::string> ret;
    ret.push_back(std::to_string(term));

    if(vote == RaftVote::GRANTED) {
      ret.push_back("granted");
    }
    else if(vote == RaftVote::REFUSED) {
      ret.push_back("refused");
    }
    else if(vote == RaftVote::VETO) {
      ret.push_back("veto");
    }
    else {
      qdb_throw("unable to convert vote to string in RaftVoteResponse::toVector");
    }

    return ret;
  }
};

enum class ElectionOutcome {
  kElected,
  kNotElected,
  kVetoed
};

inline size_t calculateQuorumSize(size_t members) {
  return (members / 2) + 1;
}

struct ReplicaStatus {
  RaftServer target;
  bool online;
  LogIndex logSize;
  std::string version;
  std::string resilveringProgress;

  ReplicaStatus() {}
  ReplicaStatus(const RaftServer &trg, bool onl, LogIndex indx, const std::string &ver = "N/A", const std::string &resilv = "")
  : target(trg), online(onl), logSize(indx), version(ver), resilveringProgress(resilv) {}

  bool upToDate(LogIndex leaderLogSize) const {
    if(!online) return false;
    if(logSize < 0) return false;
    return (leaderLogSize - logSize < 30000);
  }

  std::string toString(LogIndex currentLogSize) const {
    std::ostringstream ss;
    toString(ss, currentLogSize);
    return ss.str();
  }

  void toString(std::ostringstream &ss, LogIndex currentLogSize) const {
    ss << target.toString() << " ";

    if(online) {
      ss << "| ONLINE | ";

      if(!resilveringProgress.empty()) {
        ss << "RESILVERING-PROGRESS " << resilveringProgress << " | ";
      }
      else if(upToDate(currentLogSize)) {
        ss << "UP-TO-DATE | ";
      }
      else {
        ss << "LAGGING    | ";
      }

      ss << "LOG-SIZE ";

      if(logSize < 0) {
        ss << "N/A";
      }
      else {
        ss << logSize;
      }

      ss << " | VERSION " << version;
    }
    else {
      ss << "| OFFLINE";
    }
  }
};

struct ReplicationStatus {
  std::vector<ReplicaStatus> replicas;
  bool shakyQuorum = false;

  size_t replicasOnline() {
    size_t ret = 0;

    for(size_t i = 0; i < replicas.size(); i++) {
      if(replicas[i].online) {
        ret++;
      }
    }

    return ret;
  }

  size_t replicasUpToDate(LogIndex leaderLogSize) {
    size_t ret = 0;

    for(size_t i = 0; i < replicas.size(); i++) {
      if(replicas[i].upToDate(leaderLogSize)) {
        ret++;
      }
    }

    return ret;
  }

  bool quorumUpToDate(LogIndex leaderLogSize) {
    if(replicas.size() == 1) return true;
    return calculateQuorumSize(replicas.size()) <= replicasUpToDate(leaderLogSize);
  }

  ReplicaStatus getReplicaStatus(const RaftServer &replica) {
    for(size_t i = 0; i < replicas.size(); i++) {
      if(replicas[i].target == replica) {
        return replicas[i];
      }
    }

    qdb_throw("Replica " << replica.toString() << " not found");
  }

  void removeReplica(const RaftServer &replica) {
    for(size_t i = 0; i < replicas.size(); i++) {
      if(replicas[i].target == replica) {
        replicas.erase(replicas.begin()+i);
        return;
      }
    }

    qdb_throw("Replica " << replica.toString() << " not found");
  }

  void removeReplicas(const std::vector<RaftServer> &replicas) {
    for(size_t i = 0; i < replicas.size(); i++) {
      removeReplica(replicas[i]);
    }
  }

  void addReplica(const ReplicaStatus &replica) {
    for(size_t i = 0; i < replicas.size(); i++) {
      if(replicas[i].target == replica.target) {
        qdb_throw("Targer " << replica.target.toString() << " already exists in the list");
      }
    }
    replicas.push_back(replica);
  }

  bool contains(const RaftServer &replica) {
    for(size_t i = 0; i < replicas.size(); i++) {
      if(replicas[i].target == replica) return true;
    }
    return false;
  }
};

struct RaftInfo {
  RaftClusterID clusterID;
  RaftServer myself;
  RaftServer leader;
  HealthStatus nodeHealthStatus;
  FsyncPolicy fsyncPolicy;
  LogIndex membershipEpoch;
  std::vector<RaftServer> nodes;
  std::vector<RaftServer> observers;
  RaftTerm term;
  LogIndex logStart;
  LogIndex logSize;
  RaftStatus status;
  LogIndex commitIndex;
  LogIndex lastApplied;
  size_t blockedWrites;
  int64_t lastStateChange;
  ReplicationStatus replicationStatus;
  std::string myVersion;

  std::vector<std::string> toVector() {
    std::vector<std::string> ret;
    ret.push_back(SSTR("TERM " << term));
    ret.push_back(SSTR("LOG-START " << logStart));
    ret.push_back(SSTR("LOG-SIZE " << logSize));
    ret.push_back(SSTR("LEADER " << leader.toString()));
    ret.push_back(SSTR("CLUSTER-ID " << clusterID));
    ret.push_back(SSTR("COMMIT-INDEX " << commitIndex));
    ret.push_back(SSTR("LAST-APPLIED " << lastApplied));
    ret.push_back(SSTR("BLOCKED-WRITES " << blockedWrites));
    ret.push_back(SSTR("LAST-STATE-CHANGE " << lastStateChange << " (" << formatTime(std::chrono::seconds(lastStateChange)) << ")"));

    ret.push_back("----------");
    ret.push_back(SSTR("MYSELF " << myself.toString()));
    ret.push_back(SSTR("VERSION " << myVersion));
    ret.push_back(SSTR("STATUS " << statusToString(status)));
    ret.push_back(SSTR("NODE-HEALTH " << healthStatusAsString(nodeHealthStatus)));
    ret.push_back(SSTR("JOURNAL-FSYNC-POLICY " << fsyncPolicyToString(fsyncPolicy)));

    ret.push_back("----------");
    ret.push_back(SSTR("MEMBERSHIP-EPOCH " << membershipEpoch));
    ret.push_back(SSTR("NODES " << serializeNodes(nodes)));
    ret.push_back(SSTR("OBSERVERS " << serializeNodes(observers)));
    ret.push_back(SSTR("QUORUM-SIZE " << calculateQuorumSize(nodes.size())));

    if(!replicationStatus.replicas.empty()) {
      ret.push_back("----------");
    }

    for(auto it = replicationStatus.replicas.begin(); it != replicationStatus.replicas.end(); it++) {
      std::ostringstream ss;
      ss << "REPLICA ";
      it->toString(ss, logSize);

      ret.push_back(ss.str());
    }

    return ret;
  }
};

}

#endif
