// ----------------------------------------------------------------------
// File: BufferedReader.cc
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

#include "memory/PinnedBuffer.hh"
#include "BufferedReader.hh"
#include "Utils.hh"

using namespace quarkdb;

BufferedReader::BufferedReader(Link *lp, size_t bsize)
: link(lp), buffer_size(bsize) {
  position_read = 0;
  position_write = 0;
  buffers.emplace_back(MemoryRegion::Construct(buffer_size));
}

BufferedReader::~BufferedReader() {
  while(!buffers.empty()) {
    buffers.pop_front();
  }
}

LinkStatus BufferedReader::readFromLink(size_t limit) {
  int total_bytes = 0;
  while(true) {
    // how many bytes can I write to the end of the last buffer?
    int available_space = buffer_size - position_write;

    // non-blocking read
    LinkStatus rlen = link->Recv(buffers.back()->data() + position_write, available_space, 0);
    if(rlen < 0) return rlen; // an error occured, propagate to caller

    total_bytes += rlen;
    // we asked for available_space bytes, we got fewer. Means no more data to read
    if(rlen < available_space) {
      position_write += rlen;
      return total_bytes;
    }

    // we have more data to read, but no more space. Need to allocate buffer
    buffers.emplace_back(MemoryRegion::Construct(buffer_size));
    position_write = 0;

    if(total_bytes > (int) limit) return total_bytes;
  }
}

LinkStatus BufferedReader::canConsume(size_t len) {
  // we have n buffers, thus n*buffer_size bytes to read
  size_t available_bytes = buffers.size() * buffer_size;

  // .. minus, of course, the read and write markers for the first and last buffers
  available_bytes -= position_read;
  available_bytes -= buffer_size - position_write;
  if(available_bytes >= len) return available_bytes;

  // since we don't have enough bytes, try to read from the link
  int rlink = readFromLink(len - available_bytes);
  if(rlink < 0) return rlink; // an error occurred, propagate

  available_bytes += rlink;
  if(available_bytes >= len) return available_bytes;
  return 0; // nope, not enough data
}

LinkStatus BufferedReader::consumeInternal(size_t len, std::string &str) {
  // assumption: str is len bytes long
  str.clear();
  str.reserve(len);

  // we can safely assume there's at least len bytes to read
  size_t remaining = len;
  while(remaining > 0) {
    // how many bytes to read from current buffer?
    size_t available_bytes = buffer_size - position_read;
    if(available_bytes >= remaining) {
      available_bytes = remaining;
    }
    remaining -= available_bytes;

    // add them
    str.append(buffers.front()->data() + position_read, available_bytes);
    position_read += available_bytes;

    if(position_read >= buffer_size) {
      // an entire buffer has been consumed
      buffers.pop_front();
      position_read = 0;
    }
  }
  return len;
}

LinkStatus BufferedReader::consume(size_t len, std::string &str) {
  LinkStatus status = canConsume(len);
  if(status <= 0) return status;

  return consumeInternal(len, str);
}

LinkStatus BufferedReader::consume(size_t len, PinnedBuffer &buf) {
  LinkStatus status = canConsume(len);
  if(status <= 0) return status;

  // can we simply point "buf" to our MemoryRegion?
  size_t available_bytes = buffer_size - position_read;
  if(available_bytes >= len) {
    // Yes! Fast path, simply make a PinnedBuffer which references our
    // MemoryRegion.
    buf = PinnedBuffer(buffers.front(), buffers.front()->data() + position_read, len);
    position_read += len;
    return len;
  }

  // nope, use internal buffer
  buf = PinnedBuffer();
  return consumeInternal(len, buf.getInternalBuffer());
}
