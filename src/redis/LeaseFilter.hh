// ----------------------------------------------------------------------
// File: LeaseFilter.hh
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

#ifndef QUARKDB_REDIS_LEASE_FILTER_HH
#define QUARKDB_REDIS_LEASE_FILTER_HH

namespace quarkdb {

class RedisRequest;
class RedisEncodedResponse;
class Transaction;

using ClockValue = uint64_t;

class LeaseFilter {
public:

  //----------------------------------------------------------------------------
  // Transform LEASE_GET and LEASE_ACQUIRE into TIMESTAMPED_LEASE_GET and
  // TIMESTAMPED_LEASE_ACQUIRE, respectively.
  //----------------------------------------------------------------------------
  static void transform(RedisRequest &req, ClockValue timestamp);
  static void transform(Transaction &tx, ClockValue timestamp);
};

}

#endif
