// ----------------------------------------------------------------------
// File: RaftTalker.hh
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

#ifndef __QUARKDB_RAFT_TALKER_H__
#define __QUARKDB_RAFT_TALKER_H__

#include "qclient/QClient.hh"
#include "raft/RaftCommon.hh"
#include <mutex>

namespace qclient {
  class QClient; class Options;
}

namespace quarkdb {
using namespace qclient;

class RaftTimeouts; class RaftContactDetails;
class VersionHandshake;

using ResilveringEventID = std::string;

class RaftTalker {
public:
  RaftTalker(const RaftServer &server, const RaftContactDetails &contactDetails, std::string_view name);
  std::future<redisReplyPtr> appendEntries(RaftTerm term, RaftServer leader, LogIndex prevIndex,
                                           RaftTerm prevTerm, LogIndex commit,
                                           const std::vector<RaftSerializedEntry> &entries);

  std::future<redisReplyPtr> requestVote(const RaftVoteRequest &req, bool preVote = false);
  std::future<redisReplyPtr> fetch(LogIndex index);

  std::future<redisReplyPtr> resilveringStart(const ResilveringEventID &id);
  std::future<redisReplyPtr> resilveringCopy(const ResilveringEventID &id, const std::string &filename, const std::string &contents);
  std::future<redisReplyPtr> resilveringFinish(const ResilveringEventID &id);
  std::future<redisReplyPtr> resilveringCancel(const ResilveringEventID &id, const std::string &reason);

  std::future<redisReplyPtr> heartbeat(RaftTerm term, const RaftServer &leader);
  RaftServer getServer() { return server; }
  std::string getNodeVersion();

private:
  RaftServer server;
  std::unique_ptr<QClient> qcl;
  VersionHandshake *versionHandshake = nullptr;
};

}

#endif
