// ----------------------------------------------------------------------
// File: BufferedWriter.cc
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

#include "BufferedWriter.hh"
#include "Link.hh"
using namespace quarkdb;

BufferedWriter::BufferedWriter(Link *link_)
: link(link_) {
}

BufferedWriter::~BufferedWriter() {

}

void BufferedWriter::setActive(bool newval) {
  std::lock_guard<std::recursive_mutex> lock(mtx);
  flush();
  active = newval;
}

void BufferedWriter::flush() {
  std::lock_guard<std::recursive_mutex> lock(mtx);

  if(!link) return;
  if(bufferedBytes == 0) return;

  link->Send(buffer, bufferedBytes);
  bufferedBytes = 0;
}

LinkStatus BufferedWriter::send(std::string &&raw) {
  std::lock_guard<std::recursive_mutex> lock(mtx);

  if(!link) return 1;
  if(!active) return link->Send(raw);

  if(raw.size() + bufferedBytes > OUTPUT_BUFFER_SIZE) {
    this->flush();
    if(raw.size() > OUTPUT_BUFFER_SIZE) {
      return link->Send(raw); // response too large for output buffer
    }
  }

  memcpy(buffer + bufferedBytes, raw.c_str(), raw.size());
  bufferedBytes += raw.size();
  return 1;
}