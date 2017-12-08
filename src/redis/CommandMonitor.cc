// ----------------------------------------------------------------------
// File: CommandMonitor.cc
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

#include "CommandMonitor.hh"
#include "../Formatter.hh"
using namespace quarkdb;

CommandMonitor::CommandMonitor() {

}

void CommandMonitor::broadcast(const RedisRequest &received) {
  if(!active) return;

  std::lock_guard<std::mutex> lock(mtx);
  auto it = monitors.begin();

  while(it != monitors.end()) {
    bool stillAlive = (*it)->appendIfAttached(Formatter::status(received.toString()));

    if(!stillAlive) {
      it = monitors.erase(it);
    }
    else {
      it++;
    }
  }

  if(monitors.size() == 0) active = false;
}

void CommandMonitor::addRegistration(Connection *c) {
  std::lock_guard<std::mutex> lock(mtx);

  monitors.push_back(c->getQueue());
  c->setMonitor();
  active = true;
}

size_t CommandMonitor::size() {
  std::lock_guard<std::mutex> lock(mtx);
  return monitors.size();
}