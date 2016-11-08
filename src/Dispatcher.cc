//-----------------------------------------------------------------------
// File: Dispatcher.cc
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

#include "Dispatcher.hh"
#include "Response.hh"
#include "Utils.hh"

using namespace quarkdb;

RedisDispatcher::RedisDispatcher(RocksDB &rocksdb) : store(rocksdb) {
}

LinkStatus RedisDispatcher::dispatch(Connection *conn, RedisRequest &req, LogIndex commit) {
  auto it = redis_cmd_map.find(req[0]);
  if(it != redis_cmd_map.end()) return dispatch(conn, req, it->second.first, commit);

  return conn->err(SSTR("unknown command " << quotes(req[0])));
}

LinkStatus RedisDispatcher::dispatch(Connection *conn, RedisRequest &request, RedisCommand cmd, LogIndex commit) {
  switch(cmd) {
    case RedisCommand::PING: {
      if(request.size() > 2) return conn->errArgs(request[0]);
      if(request.size() == 1) return conn->pong();

      return conn->string(request[1]);
    }
    case RedisCommand::DEBUG: {
      if(request.size() != 2) return conn->errArgs(request[0]);
      if(request[1] == "segfault" || request[1] == "SEGFAULT") {
        *( (int*) nullptr) = 5; // bye bye, commit seppuku
      }

      return conn->err(SSTR("unknown argument '" << request[1] << "'"));
    }
    case RedisCommand::FLUSHALL: {
      if(request.size() != 1) return conn->errArgs(request[0]);
      rocksdb::Status st = store.flushall(commit);
      return conn->fromStatus(st);
    }
    case RedisCommand::GET: {
      if(request.size() != 2) return conn->errArgs(request[0]);

      std::string value;
      rocksdb::Status st = store.get(request[1], value);
      if(st.IsNotFound()) return conn->null();
      if(!st.ok()) return conn->fromStatus(st);
      return conn->string(value);
    }
    case RedisCommand::SET: {
      if(request.size() != 3) return conn->errArgs(request[0]);
      rocksdb::Status st = store.set(request[1], request[2], commit);
      return conn->fromStatus(st);
    }
    case RedisCommand::EXISTS: {
      if(request.size() <= 1) return conn->errArgs(request[0]);
      int64_t count = 0;
      for(size_t i = 1; i < request.size(); i++) {
        rocksdb::Status st = store.exists(request[i]);
        if(st.ok()) count++;
        else if(!st.IsNotFound()) return conn->fromStatus(st);
      }
      return conn->integer(count);
    }
    case RedisCommand::DEL: {
      if(request.size() <= 1) return conn->errArgs(request[0]);

      int64_t count = 0;
      for(size_t i = 1; i < request.size(); i++) {
        rocksdb::Status st = store.del(request[i], commit);
        if(st.ok()) count++;
        else if(!st.IsNotFound()) return conn->fromStatus(st);
      }
      return conn->integer(count);
    }
    case RedisCommand::KEYS: {
      if(request.size() != 2) return conn->errArgs(request[0]);
      std::vector<std::string> ret;
      rocksdb::Status st = store.keys(request[1], ret);
      if(!st.ok()) return conn->fromStatus(st);
      return conn->vector(ret);
    }
    case RedisCommand::HGET: {
      if(request.size() != 3) return conn->errArgs(request[0]);

      std::string value;
      rocksdb::Status st = store.hget(request[1], request[2], value);
      if(st.IsNotFound()) return conn->null();
      else if(!st.ok()) return conn->fromStatus(st);
      return conn->string(value);
    }
    case RedisCommand::HSET: {
      if(request.size() != 4) return conn->errArgs(request[0]);

      // Mild race condition here.. if the key doesn't exist, but another thread modifies
      // it in the meantime the user gets a response of 1, not 0

      rocksdb::Status existed = store.hexists(request[1], request[2]);
      if(!existed.ok() && !existed.IsNotFound()) return conn->fromStatus(existed);

      rocksdb::Status st = store.hset(request[1], request[2], request[3], commit);
      if(!st.ok()) return conn->fromStatus(st);

      if(existed.ok()) return conn->integer(0);
      return conn->integer(1);
    }
    case RedisCommand::HEXISTS: {
      if(request.size() != 3) return conn->errArgs(request[0]);
      rocksdb::Status st = store.hexists(request[1], request[2]);
      if(st.ok()) return conn->integer(1);
      if(st.IsNotFound()) return conn->integer(0);
      return conn->fromStatus(st);
    }
    case RedisCommand::HKEYS: {
      if(request.size() != 2) return conn->errArgs(request[0]);
      std::vector<std::string> keys;
      rocksdb::Status st = store.hkeys(request[1], keys);
      if(!st.ok()) return conn->fromStatus(st);
      return conn->vector(keys);
    }
    case RedisCommand::HGETALL: {
      if(request.size() != 2) return conn->errArgs(request[0]);
      std::vector<std::string> vec;
      rocksdb::Status st = store.hgetall(request[1], vec);
      if(!st.ok()) return conn->fromStatus(st);
      return conn->vector(vec);
    }
    case RedisCommand::HINCRBY: {
      if(request.size() != 4) return conn->errArgs(request[0]);
      int64_t ret = 0;
      rocksdb::Status st = store.hincrby(request[1], request[2], request[3], ret, commit);
      if(!st.ok()) return conn->fromStatus(st);
      return conn->integer(ret);
    }
    case RedisCommand::HDEL: {
      if(request.size() <= 2) return conn->errArgs(request[0]);
      int64_t count = 0;
      for(size_t i = 2; i < request.size(); i++) {
        rocksdb::Status st = store.hdel(request[1], request[i], commit);
        if(st.ok()) count++;
        else if(!st.IsNotFound()) return conn->fromStatus(st);
      }
      return conn->integer(count);
    }
    case RedisCommand::HLEN: {
      if(request.size() != 2) return conn->errArgs(request[0]);
      size_t len;
      rocksdb::Status st = store.hlen(request[1], len);
      if(!st.ok()) return conn->fromStatus(st);
      return conn->integer(len);
    }
    case RedisCommand::HVALS: {
      if(request.size() != 2) return conn->errArgs(request[0]);
      std::vector<std::string> values;
      rocksdb::Status st = store.hvals(request[1], values);
      return conn->vector(values);
    }
    case RedisCommand::HSCAN: {
      if(request.size() != 3) return conn->errArgs(request[0]);
      if(request[2] != "0") return conn->err("invalid cursor");

      std::vector<std::string> vec;
      rocksdb::Status st = store.hgetall(request[1], vec);
      if(!st.ok()) return conn->fromStatus(st);

      return conn->scan("0", vec);
    }
    case RedisCommand::SADD: {
      if(request.size() <= 2) return conn->err(request[0]);
      int64_t count = 0;
      for(size_t i = 2; i < request.size(); i++) {
        int64_t tmp = 0;
        rocksdb::Status st = store.sadd(request[1], request[i], tmp, commit);
        if(!st.ok()) return conn->fromStatus(st);
        count += tmp;
      }
      return conn->integer(count);
    }
    case RedisCommand::SISMEMBER: {
      if(request.size() != 3) return conn->errArgs(request[0]);
      rocksdb::Status st = store.sismember(request[1], request[2]);
      if(st.ok()) return conn->integer(1);
      if(st.IsNotFound()) return conn->integer(0);
      return conn->fromStatus(st);
    }
    case RedisCommand::SREM: {
      if(request.size() <= 2) return conn->errArgs(request[0]);
      int64_t count = 0;
      for(size_t i = 2; i < request.size(); i++) {
        rocksdb::Status st = store.srem(request[1], request[i], commit);
        if(st.ok()) count++;
        else if(!st.IsNotFound()) return conn->fromStatus(st);
      }
      return conn->integer(count);
    }
    case RedisCommand::SMEMBERS: {
      if(request.size() != 2) return conn->errArgs(request[0]);
      std::vector<std::string> members;
      rocksdb::Status st = store.smembers(request[1], members);
      if(!st.ok()) return conn->fromStatus(st);
      return conn->vector(members);
    }
    case RedisCommand::SCARD: {
      if(request.size() != 2) return conn->errArgs(request[0]);
      size_t count;
      rocksdb::Status st = store.scard(request[1], count);
      if(!st.ok()) return conn->fromStatus(st);
      return conn->integer(count);
    }
    case RedisCommand::SSCAN: {
      if(request.size() != 3) return conn->errArgs(request[0]);
      if(request[2] != "0") return conn->err("invalid cursor");
      std::vector<std::string> members;
      rocksdb::Status st = store.smembers(request[1], members);
      if(!st.ok()) return conn->fromStatus(st);
      return conn->scan("0", members);
    }
    default:
      return conn->err(SSTR("internal dispatching error for " << quotes(request[0]) << " - raft not enabled?"));
  }
}
