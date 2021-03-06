//----------------------------------------------------------------------
// File: Connection.hh
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

#ifndef QUARKDB_CONNECTION_H
#define QUARKDB_CONNECTION_H

#include "Link.hh"
#include "RedisParser.hh"
#include "BufferedWriter.hh"
#include "Formatter.hh"
#include "redis/MultiHandler.hh"
#include "redis/Authenticator.hh"
#include "pubsub/SubscriptionTracker.hh"
#include "utils/Synchronized.hh"
#include <queue>

namespace rocksdb {
  class Status;
}

namespace quarkdb {

//------------------------------------------------------------------------------
// Keeps track of a list of pending requests, which can optionally be related
// to a Connection.
//
// Why "optionally"? There's no guarantee that by the time a pending request
// is ready to be serviced, the connection will still be alive! The client
// might have disconnected in the meantime, even after issuing writes that
// have already been appended to the raft journal.
//------------------------------------------------------------------------------

class Connection;
class RedisDispatcher;
class PendingQueue {
public:
  PendingQueue(Connection *c) : conn(c) {}
  ~PendingQueue() {}

  void detachConnection() {
    std::scoped_lock lock(mtx);
    conn = nullptr;
  }

  LinkStatus flushPending(const RedisEncodedResponse &msg);
  LinkStatus appendResponse(RedisEncodedResponse &&raw);
  LinkStatus addPendingTransaction(RedisDispatcher *dispatcher, Transaction &&tx, LogIndex index = -1);
  LogIndex dispatchPending(RedisDispatcher *dispatcher, LogIndex commitIndex);
  bool appendIfAttached(RedisEncodedResponse &&raw);
  bool appendIfAttachedNoLock(RedisEncodedResponse &&raw);
  size_t subscriptions = 0u;

  void subscribe(const std::string &item);
  void psubscribe(const std::string &item);

  void unsubscribe(const std::string &item);
  void punsubscribe(const std::string &item);

  bool addMessageIfAttached(const std::string &channel, std::string_view payload);
  bool addPatternMessageIfAttached(const std::string &pattern, std::string_view channel, std::string_view payload);

  void activatePushTypes();
  bool hasPushTypesActive() const;

private:
  LinkStatus appendResponseNoLock(RedisEncodedResponse &&raw);
  Connection *conn;
  std::mutex mtx;

  //----------------------------------------------------------------------------
  // Information about a pending request, which can be either a read or a write.
  // Every write corresponds to exactly one entry in the raft journal. Naturally,
  // we have to wait until it's committed before responding to the client.
  //
  // But why do reads need to wait, too? If a read request is made right after a
  // write with pipelining, we have to give the responses in the correct order,
  // so a read has to be queued until the write that's blocking us has finished.
  //
  // The queue will usually look like this:
  // write, read, read, read, write, read, read, read, write, write
  //
  // All read requests are being blocked by one or more writes, and each write
  // corresponds to a unique raft journal entry.
  //
  // Reads will be processed as soon as they aren't being blocked by a write. If
  // all a client does is read, the queue will not be used.
  //----------------------------------------------------------------------------

  struct PendingRequest {
    Transaction tx;
    RedisEncodedResponse rawResp; // if not empty, we're just storing a raw, pre-formatted response
    LogIndex index = -1; // the corresponding entry in the raft journal - only relevant for write requests
  };

  LogIndex lastIndex = -1;
  std::queue<PendingRequest> pending;
  SubscriptionTracker subscriptionTracker;
  std::atomic<bool> supportsPushTypes {false};
};

//------------------------------------------------------------------------------
// Keeps track of connection-specific state.
//------------------------------------------------------------------------------
class Dispatcher; class InFlightTracker;
class RedisEncodedResponse;
class Authenticator;

class Connection {
public:
  Connection(Link *link);
  ~Connection();
  std::string describe() const;
  std::string getID() const { return uuid; }

  LinkStatus raw(RedisEncodedResponse &&encoded);
  LinkStatus moved(int64_t shardId, const RaftServer &location);
  LinkStatus err(std::string_view msg);
  LinkStatus errArgs(std::string_view cmd);
  LinkStatus pong();
  LinkStatus string(std::string_view str);
  LinkStatus fromStatus(const rocksdb::Status &status);
  LinkStatus status(std::string_view msg);
  LinkStatus ok();
  LinkStatus null();
  LinkStatus integer(int64_t number);
  LinkStatus vector(const std::vector<std::string> &vec);
  LinkStatus statusVector(const std::vector<std::string> &vec);
  LinkStatus scan(std::string_view marker, const std::vector<std::string> &vec);
  LinkStatus noauth(std::string_view msg);

  bool monitor = false;
  void setMonitor() {
    // There's no function setting monitor back to false. This is intentional,
    // there's no going back after issuing 'MONITOR'.
    monitor = true;
  }

  bool raftStaleReads = false;
  bool raftAuthorization = false;
  bool authorization = false;
  std::unique_ptr<Authenticator> authenticator;

  LinkStatus processRequests(Dispatcher *dispatcher, const InFlightTracker &tracker);
  void setResponseBuffering(bool value);
  void flush();

  LinkStatus addPendingTransaction(RedisDispatcher *dispatcher, Transaction &&tx, LogIndex index = -1) {
    return pendingQueue->addPendingTransaction(dispatcher, std::move(tx), index);
  }

  LinkStatus flushPending(const RedisEncodedResponse &msg) {
    return pendingQueue->flushPending(msg);
  }

  LogIndex dispatchPending(RedisDispatcher *dispatcher, LogIndex commitIndex) {
    return pendingQueue->dispatchPending(dispatcher, commitIndex);
  }

  std::shared_ptr<PendingQueue> getQueue() {
    return pendingQueue;
  }

  bool hasPushTypesActive() {
    return pendingQueue->hasPushTypesActive();
  }

  bool isLocalhost() const {
    return localhost;
  }

  class FlushGuard {
  public:
    FlushGuard(Connection *c) : conn(c) { }
    ~FlushGuard() { if(conn) { conn->flush(); } }
  private:
    Connection *conn;
  };

  void activatePushTypes();
  bool hasPushTypesActive() const;

  static void setPhantomBatchLimit(size_t newval);

  void setName(std::string_view name);
  std::string getName() const;

private:
  BufferedWriter writer;

  RedisRequest currentRequest;
  RedisParser parser;
  std::shared_ptr<PendingQueue> pendingQueue;

  std::string description;
  std::string uuid;
  bool localhost;

  Synchronized<std::string> clientName;

  MultiHandler multiHandler;
  friend class PendingQueue;
};


}

#endif
