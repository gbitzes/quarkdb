// ----------------------------------------------------------------------
// File: BufferedReader.hh
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

#ifndef QUARKDB_BUFFERED_READER_HH
#define QUARKDB_BUFFERED_READER_HH

#include "memory/RingAllocator.hh"
#include <deque>
#include <string>

#include "Link.hh"

namespace quarkdb {

class BufferedReader {
public:
  BufferedReader(Link *lp, size_t bsize = 1024 * 32);
  ~BufferedReader();

  //----------------------------------------------------------------------------
  // Read exactly len bytes from the link. An all-or-nothing operation -
  // either it succeeds and we get len bytes, or there's not enough data on the
  // link yet and we get nothing.
  //----------------------------------------------------------------------------
  LinkStatus consume(size_t len, std::string &str);

  //----------------------------------------------------------------------------
  // Read exactly len bytes from the link. An all-or-nothing operation -
  // either it succeeds and we get len bytes, or there's not enough data on the
  // link yet and we get nothing.
  //
  // We are given a PinnedBuffer - if we're lucky, we'll be able to avoid
  // any dynamic memory allocations, and reference the data directly to our
  // MemoryRegion.
  //
  // This is not always possible - in such case, use the buffer's internal
  // storage to copy the data.
  //----------------------------------------------------------------------------
  LinkStatus consume(size_t len, PinnedBuffer &buf);

private:
  Link *link;

  //----------------------------------------------------------------------------
  // We use a deque of buffers for reading from the socket.
  // We always append new buffers to this deque - once a buffer is full, we
  // allocate a new one. Once the contents of a buffer have been parsed, we
  // release it.
  //----------------------------------------------------------------------------

  std::deque<std::shared_ptr<MemoryRegion>> buffers;
  size_t position_read; // always points to the buffer at the front
  size_t position_write; // always points to the buffer at the end
  const size_t buffer_size;


  //----------------------------------------------------------------------------
  // Read from the link as much data as is currently available, up to some
  // limit. We might exceed this limit internally, but not by much.
  //----------------------------------------------------------------------------
  LinkStatus readFromLink(size_t limit);

  //----------------------------------------------------------------------------
  // Is it possible to consume len bytes?
  // Returns 0 if not, negative on error, or the number of bytes that is
  // possible to read if and only if that amount is greater than len
  //----------------------------------------------------------------------------
  LinkStatus canConsume(size_t len);

  //----------------------------------------------------------------------------
  // Internal consume function - does not check canConsume first
  //----------------------------------------------------------------------------
  LinkStatus consumeInternal(size_t len, std::string &str);
};

}

#endif
