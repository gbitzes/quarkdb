// ----------------------------------------------------------------------
// File: Raft.cc
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

#include "utils/IntToBinaryString.hh"
#include "raft/RaftTalker.hh"
#include "raft/RaftContactDetails.hh"
#include "raft/RaftTimeouts.hh"
#include "Version.hh"
#include <qclient/Logger.hh>

namespace quarkdb {

class RaftHandshake : public qclient::Handshake {
public:
  virtual ~RaftHandshake() override {}

  RaftHandshake(const RaftContactDetails &cd)
  : contactDetails(cd) {
    restart();
  }

  virtual std::vector<std::string> provideHandshake() override {
    return {"RAFT_HANDSHAKE", VERSION_FULL_STRING, contactDetails.getClusterID(), contactDetails.getRaftTimeouts().toString() };
  }

  virtual Status validateResponse(const redisReplyPtr &reply) override {
    if(!reply) {
      return Status::INVALID;
    }

    if(reply->type != REDIS_REPLY_STATUS) {
      return Status::INVALID;
    }

    if(std::string(reply->str, reply->len) != "OK") {
      return Status::INVALID;
    }

    return Status::VALID_COMPLETE;
  }

  virtual void restart() override {}

  virtual std::unique_ptr<Handshake> clone() const {
    return std::unique_ptr<Handshake>(new RaftHandshake(contactDetails));
  }

private:
  const RaftContactDetails &contactDetails;
};

//------------------------------------------------------------------------------
// VersionHandshake is used to determine which QDB version a node is at.
//------------------------------------------------------------------------------
class VersionHandshake : public qclient::Handshake {
public:

  VersionHandshake() {
    version = "N/A";
  }

  virtual std::vector<std::string> provideHandshake() override {
    return {"QUARKDB_VERSION"};
  }

  virtual Status validateResponse(const redisReplyPtr &reply) override {
    std::unique_lock<std::mutex> lock(mtx);
    version = "N/A";

    if(!reply) {
      return Status::INVALID;
    }

    if(reply->type != REDIS_REPLY_STRING) {
      // cannot parse output of quarkdb-version.. maybe the other node
      // is running a really old version without support for quarkdb-version
      // command.
      // TODO(gbitzes): Eventually make this an error, and refuse to communicate
      // if the other node is *that* old.
      return Status::VALID_COMPLETE;
    }

    version = std::string(reply->str, reply->len);
    return Status::VALID_COMPLETE;
  }

  virtual void restart() override {
    std::unique_lock<std::mutex> lock(mtx);
    version = "N/A";
  }

  std::string getVersion() const {
    std::unique_lock<std::mutex> lock(mtx);
    return version;
  }

  virtual std::unique_ptr<Handshake> clone() const {
    return std::unique_ptr<Handshake>(new VersionHandshake());
  }

private:
  mutable std::mutex mtx;
  std::string version;
};

class QuarkDBLogger : public qclient::Logger {
public:

  QuarkDBLogger() {
    logLevel = qclient::LogLevel::kWarn;
  }

  void print(LogLevel level, int line, const std::string &file, const std::string &msg) override {
      ___log("QCLIENT (" << logLevelToString(level) << "): " << msg);
  }
};

RaftTalker::RaftTalker(const RaftServer &server_, const RaftContactDetails &contactDetails, std::string_view name)
: server(server_) {

  qclient::Options opts;

  opts.transparentRedirects = false;
  opts.retryStrategy = qclient::RetryStrategy::NoRetries();
  opts.backpressureStrategy = qclient::BackpressureStrategy::Default();
  opts.logger.reset(new QuarkDBLogger());

  opts.chainHmacHandshake(contactDetails.getPassword());
  opts.chainHandshake(std::unique_ptr<Handshake>(new RaftHandshake(contactDetails)));
  opts.chainHandshake(std::unique_ptr<Handshake>(new qclient::SetClientNameHandshake(std::string(name))));

  // Make a version handshake - capture ownership inside QClient, but keep pointer
  // to it here.
  versionHandshake = new VersionHandshake();
  opts.chainHandshake(std::unique_ptr<Handshake>(versionHandshake));

  qcl.reset(new QClient(server.hostname, server.port, std::move(opts)));
}

std::string RaftTalker::getNodeVersion() {
  return versionHandshake->getVersion();
}

std::future<redisReplyPtr> RaftTalker::heartbeat(RaftTerm term, const RaftServer &leader) {
  RedisRequest payload;

  payload.emplace_back("RAFT_HEARTBEAT");
  payload.emplace_back(std::to_string(term));
  payload.emplace_back(leader.toString());

  return qcl->execute(payload);
}

std::future<redisReplyPtr> RaftTalker::appendEntries(
  RaftTerm term, RaftServer leader, LogIndex prevIndex,
  RaftTerm prevTerm, LogIndex commit,
  const std::vector<RaftSerializedEntry> &entries) {

  if(term < prevTerm) {
    qdb_throw(SSTR("term < prevTerm.. " << prevTerm << "," << term));
  }

  RedisRequest payload;
  payload.reserve(3 + entries.size());

  payload.emplace_back("RAFT_APPEND_ENTRIES");
  payload.emplace_back(leader.toString());

  char buffer[sizeof(int64_t) * 5];
  intToBinaryString(term,             buffer + 0*sizeof(int64_t));
  intToBinaryString(prevIndex,        buffer + 1*sizeof(int64_t));
  intToBinaryString(prevTerm,         buffer + 2*sizeof(int64_t));
  intToBinaryString(commit,           buffer + 3*sizeof(int64_t));
  intToBinaryString(entries.size(),   buffer + 4*sizeof(int64_t));

  payload.emplace_back(buffer, 5*sizeof(int64_t));

  for(size_t i = 0; i < entries.size(); i++) {
    payload.push_back(entries[i]);
    qdb_assert(RaftEntry::fetchTerm(entries[i]) <= term);
  }

  return qcl->execute(payload);
}

std::future<redisReplyPtr> RaftTalker::requestVote(const RaftVoteRequest &req, bool preVote) {
  RedisRequest payload;

  if(preVote) {
    payload.emplace_back("RAFT_REQUEST_PRE_VOTE");
  }
  else {
    payload.emplace_back("RAFT_REQUEST_VOTE");
  }

  payload.emplace_back(std::to_string(req.term));
  payload.emplace_back(req.candidate.toString());
  payload.emplace_back(std::to_string(req.lastIndex));
  payload.emplace_back(std::to_string(req.lastTerm));

  return qcl->execute(payload);
}

std::future<redisReplyPtr> RaftTalker::fetch(LogIndex index) {
  RedisRequest payload;

  payload.emplace_back("RAFT_FETCH");
  payload.emplace_back(std::to_string(index));

  return qcl->execute(payload);
}

std::future<redisReplyPtr> RaftTalker::resilveringStart(const ResilveringEventID &id) {
  return qcl->exec("quarkdb_start_resilvering", id);
}

std::future<redisReplyPtr> RaftTalker::resilveringCopy(const ResilveringEventID &id, const std::string &filename, const std::string &contents) {
  return qcl->exec("quarkdb_resilvering_copy_file", id, filename, contents);
}

std::future<redisReplyPtr> RaftTalker::resilveringFinish(const ResilveringEventID &id) {
  return qcl->exec("quarkdb_finish_resilvering", id);
}

std::future<redisReplyPtr> RaftTalker::resilveringCancel(const ResilveringEventID &id, const std::string &reason) {
  return qcl->exec("quarkdb_cancel_resilvering");
}

}
