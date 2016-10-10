// ----------------------------------------------------------------------
// File: Poller.cc
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

#include "Poller.hh"
#include "RedisParser.hh"
#include <sys/socket.h>
#include <sys/un.h>
#include <poll.h>

using namespace quarkdb;

Poller::Poller(const std::string &p, Dispatcher *dispatcher) : path(p) {
  s = socket(AF_UNIX, SOCK_STREAM, 0);
  local.sun_family = AF_UNIX;
  strcpy(local.sun_path, path.c_str());
  len = strlen(local.sun_path) + sizeof(local.sun_family);
  bind(s, (struct sockaddr *)&local, len);
  listen(s, 1);
  t = sizeof(remote);

  shutdown = false;
  threadsAlive = 0;
  mainThread = std::thread(&Poller::main, this, dispatcher);
}

Poller::~Poller() {
  shutdown = true;
  ::shutdown(s, SHUT_RDWR); // kill the socket, un-block if stuck on accept
  while(threadsAlive != 0) {
    shutdownFD.notify();
  }
  mainThread.join();
}

void Poller::main(Dispatcher *dispatcher) {
  ScopedAdder<int64_t> adder(threadsAlive);

  int fd = accept(s, (struct sockaddr *)&remote, &t);
  XrdBuffManager bufferManager(NULL, NULL);
  Link link(fd);
  RedisParser parser(&link, &bufferManager);

  struct pollfd polls[2];
  polls[0].fd = fd;
  polls[0].events = POLLIN;

  polls[1].fd = shutdownFD.getFD();
  polls[1].events = POLLIN;

  RedisRequest currentRequest;

  while(!shutdown) {
    poll(polls, 2, -1);

    while(true) {
      LinkStatus status = parser.fetch(currentRequest);
      if(status <= 0) break;
      dispatcher->dispatch(&link, currentRequest);
    }
  }

}
