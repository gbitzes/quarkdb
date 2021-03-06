// ----------------------------------------------------------------------
// File: Utils.cc
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

#include "utils/Stacktrace.hh"
#include "Utils.hh"
#include "utils/ParseUtils.hh"

#include <climits>
#include <memory.h>
#include <math.h>
#include <sys/stat.h>

namespace quarkdb {

bool stacktraceOnError = true;
void setStacktraceOnError(bool val) {
  stacktraceOnError = val;
}

std::string errorStacktrace(bool crash) {
  if(!stacktraceOnError) {
    return "";
  }

  std::string suffixMessage;

  if(!crash) {
    suffixMessage = " ----- The above stacktrace does NOT signify a crash! It's used to show the location of a serious error.";
  }

  return SSTR(" ----- " << getStacktrace() << suffixMessage);
}

std::mutex logMutex;

bool caseInsensitiveEquals(std::string_view str1, std::string_view str2) {
  if(str1.size() != str2.size()) return false;
  for(size_t i = 0; i < str1.size(); i++) {
    if(tolower(str1[i]) != tolower(str2[i])) return false;
  }
  return true;
}

bool my_strtod(std::string_view str, double &ret) {
  char *endptr = NULL;
  ret = strtod(str.data(), &endptr);
  if(endptr != str.data() + str.size() || ret == HUGE_VAL || ret == -HUGE_VAL) {
    return false;
  }
  return true;
}

std::vector<std::string> split(std::string_view dataView, std::string token) {
    std::string data(dataView);
    std::vector<std::string> output;
    size_t pos = std::string::npos;
    do {
        pos = data.find(token);
        output.push_back(data.substr(0, pos));
        if(std::string::npos != pos)
            data = data.substr(pos + token.size());
    } while (std::string::npos != pos);
    return output;
}

bool startswith(const std::string &str, const std::string &prefix) {
  if(prefix.size() > str.size()) return false;

  for(size_t i = 0; i < prefix.size(); i++) {
    if(str[i] != prefix[i]) return false;
  }
  return true;
}

bool parseServer(std::string_view str, RaftServer &srv) {
  std::vector<std::string> parts = split(str, ":");

  if(parts.size() != 2) return false;

  int64_t port;
  if(!ParseUtils::parseInt64(parts[1], port)) return false;

  srv = RaftServer{ parts[0], (int) port };
  return true;
}

bool parseServers(std::string_view str, std::vector<RaftServer> &servers) {
  servers = {};
  std::vector<std::string> parts = split(str, ",");

  for(size_t i = 0; i < parts.size(); i++) {
    RaftServer srv;
    if(!parseServer(parts[i], srv)) return false;
    servers.push_back(srv);
  }

  return checkUnique(servers);
}

std::string serializeNodes(const std::vector<RaftServer> &nodes) {
  std::stringstream ss;
  for(size_t i = 0; i < nodes.size(); i++) {
    ss << nodes[i].toString();
    if(i != nodes.size()-1) ss << ",";
  }
  return ss.str();
}

}
