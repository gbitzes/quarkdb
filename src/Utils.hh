// ----------------------------------------------------------------------
// File: Utils.hh
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

#ifndef __QUARKDB_UTILS_H__
#define __QUARKDB_UTILS_H__

#include <iostream>
#include <sstream>
#include <vector>
#include <set>
#include <atomic>
#include <chrono>
#include <mutex>

#include "utils/Macros.hh"
#include "Common.hh"

namespace quarkdb {


// Controls whether stacktraces are printed on serious errors
// (critical, and exceptions)
// True by default when running a real instance, but false during tests,
// as many error conditions are simulated there, and we'd make the output
// unreadable.
void setStacktraceOnError(bool val);

bool my_strtod(std::string_view str, double &ret);
std::vector<std::string> split(std::string_view data, std::string token);
bool startswith(const std::string &str, const std::string &prefix);
bool parseServer(std::string_view str, RaftServer &srv);
bool parseServers(std::string_view str, std::vector<RaftServer> &servers);
std::string serializeNodes(const std::vector<RaftServer> &nodes);
bool caseInsensitiveEquals(std::string_view str1, std::string_view str2);

inline std::string boolToString(bool b) {
  if(b) return "TRUE";
  return "FALSE";
}

inline std::string vecToString(const std::vector<std::string> &vec) {
  std::ostringstream ss;
  ss << "[";
  for(size_t i = 0; i < vec.size(); i++) {
    ss << vec[i];
    if(i != vec.size()-1) ss << ", ";
  }
  ss << "]";
  return ss.str();
}

// given a vector, checks whether all elements are unique
template<class T>
bool checkUnique(const std::vector<T> &v) {
  for(size_t i = 0; i < v.size(); i++) {
    for(size_t j = 0; j < v.size(); j++) {
      if(i != j && v[i] == v[j]) {
        return false;
      }
    }
  }
  return true;
}

template<class T>
bool contains(const std::vector<T> &v, const T& element) {
  for(size_t i = 0; i <  v.size(); i++) {
    if(v[i] == element) return true;
  }
  return false;
}

template<class T>
bool erase_element(std::vector<T> &v, const T& element) {
  auto it = v.begin();
  while(it != v.end()) {
    if(*it == element) {
      v.erase(it);
      return true;
    }
    it++;
  }
  return false;
}

template<class T>
bool all_identical(const std::vector<T> &v) {
  for(size_t i = 1; i < v.size(); i++) {
    if( !(v[i] == v[i-1]) ) return false;
  }
  return true;
}

}

#endif
