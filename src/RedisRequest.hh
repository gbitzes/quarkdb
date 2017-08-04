// ----------------------------------------------------------------------
// File: RedisRequest.hh
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

#ifndef __QUARKDB_REDIS_REQUEST_H__
#define __QUARKDB_REDIS_REQUEST_H__

#include "Commands.hh"
#include <string>
#include <vector>

namespace quarkdb {

class RedisRequest {
public:
  using container = std::vector<std::string>;
  using iterator = container::iterator;
  using const_iterator = container::const_iterator;

  RedisRequest(std::initializer_list<std::string> list) {
    for(auto it = list.begin(); it != list.end(); it++) {
      contents.push_back(*it);
    }
    parseCommand();
  }

  RedisRequest() {}

  size_t size() const {
    return contents.size();
  }

  std::string&& move(size_t i) {
    invalidateCommand();
    return std::move(contents[i]);
  }

  const std::string& operator[](size_t i) const {
    return contents[i];
  }

  bool operator==(const RedisRequest &rhs) const {
    return contents == rhs.contents;
  }

  bool operator!=(const RedisRequest &rhs) const {
    return !(contents == rhs.contents);
  }

  void clear() {
    invalidateCommand();
    contents.clear();
  }

  void emplace_back(std::string &&src) {
    contents.emplace_back(std::move(src));
    if(contents.size() == 1) parseCommand();
  }

  void emplace_back(const char* buf, size_t size) {
    contents.emplace_back(buf, size);
    if(contents.size() == 1) parseCommand();
  }

  const_iterator begin() const {
    return contents.begin();
  }

  const_iterator end() const {
    return contents.end();
  }

  void reserve(size_t size) {
    contents.reserve(size);
  }

  RedisCommand getCommand() const {
    return command;
  }

  CommandType getCommandType() const {
    return commandType;
  }

private:
  std::vector<std::string> contents;
  RedisCommand command = RedisCommand::INVALID;
  CommandType commandType = CommandType::INVALID;

  void parseCommand();
  void invalidateCommand() {
    command = RedisCommand::INVALID;
    commandType = CommandType::INVALID;
  }

};

}

#endif