// ----------------------------------------------------------------------
// File: RaftTimeouts.cc
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

#include "raft/RaftTimeouts.hh"
#include "Utils.hh"
#include "utils/ParseUtils.hh"
#include <random>

using namespace quarkdb;

std::random_device RaftTimeouts::rd;
std::mt19937 RaftTimeouts::gen(rd());
std::mutex RaftTimeouts::genMutex;


RaftTimeouts quarkdb::relaxedTimeouts {milliseconds(5000), milliseconds(10000),
  milliseconds(500)};

RaftTimeouts quarkdb::defaultTimeouts {milliseconds(1000), milliseconds(1500),
  milliseconds(250)};

RaftTimeouts quarkdb::tightTimeouts {milliseconds(100), milliseconds(150),
  milliseconds(75)};

RaftTimeouts quarkdb::aggressiveTimeouts {milliseconds(50), milliseconds(75),
  milliseconds(5)};

RaftTimeouts::RaftTimeouts(const milliseconds &low, const milliseconds &high,
  const milliseconds &heartbeat)
: timeoutLow(low), timeoutHigh(high), heartbeatInterval(heartbeat), dist(low.count(), high.count()) {

}

milliseconds RaftTimeouts::getLow() const {
  return timeoutLow;
}

milliseconds RaftTimeouts::getHigh() const {
  return timeoutHigh;
}

milliseconds RaftTimeouts::getRandom() const {
  std::scoped_lock lock(genMutex);
  return std::chrono::milliseconds(dist(gen));
}

milliseconds RaftTimeouts::getHeartbeatInterval() const {
  return heartbeatInterval;
}

std::string RaftTimeouts::toString() const {
  return SSTR(getLow().count() << ":" << getHigh().count() << ":" << getHeartbeatInterval().count());
}

static bool parseError(const std::string &str) {
  qdb_critical("Unable to parse raft timeouts: " << str);
  return false;
}

bool RaftTimeouts::fromString(RaftTimeouts &ret, const std::string &str) {
  std::vector<std::string> parts = split(str, ":");

  if(parts.size() != 3) {
    return parseError(str);
  }

  int64_t low, high, heartbeat;

  if(!ParseUtils::parseInt64(parts[0], low)) {
    return parseError(str);
  }

  if(!ParseUtils::parseInt64(parts[1], high)) {
    return parseError(str);
  }

  if(!ParseUtils::parseInt64(parts[2], heartbeat)) {
    return parseError(str);
  }

  ret = RaftTimeouts(milliseconds(low), milliseconds(high), milliseconds(heartbeat));
  return true;
}

RaftHeartbeatTracker::RaftHeartbeatTracker(const RaftTimeouts t)
: timeouts(t) {
  refreshRandomTimeout();
}

void RaftHeartbeatTracker::heartbeat(std::chrono::steady_clock::time_point now) {
  std::scoped_lock lock(lastHeartbeatMutex);
  if(lastHeartbeat <= now) {
    lastHeartbeat = now;
  }
}

void RaftHeartbeatTracker::triggerTimeout() {
  std::scoped_lock lock(lastHeartbeatMutex);
  artificialTimeout = true;
}

TimeoutStatus RaftHeartbeatTracker::timeout(std::chrono::steady_clock::time_point now) {
  std::scoped_lock lock(lastHeartbeatMutex);
  if(artificialTimeout) {
    qdb_event("Triggering an artificial timeout.");
    artificialTimeout = false;
    return TimeoutStatus::kArtificial;
  }

  bool isTimeout = now - lastHeartbeat > randomTimeout;
  if(isTimeout) {
    return TimeoutStatus::kYes;
  }

  return TimeoutStatus::kNo;
}

milliseconds RaftHeartbeatTracker::getRandomTimeout() {
  std::scoped_lock lock(lastHeartbeatMutex);
  return randomTimeout;
}

milliseconds RaftHeartbeatTracker::refreshRandomTimeout() {
  std::scoped_lock lock(lastHeartbeatMutex);
  randomTimeout = timeouts.getRandom();
  return randomTimeout;
}

std::chrono::steady_clock::time_point RaftHeartbeatTracker::getLastHeartbeat() {
  std::scoped_lock lock(lastHeartbeatMutex);
  return lastHeartbeat;
}
