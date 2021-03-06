// ----------------------------------------------------------------------
// File: RequestCounter.cc
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

#include "Utils.hh"
#include "utils/RequestCounter.hh"
#include "Commands.hh"
#include "redis/Transaction.hh"
using namespace quarkdb;

RequestCounter::RequestCounter(std::chrono::seconds intv)
  : interval(intv), historical(100), thread(&RequestCounter::mainThread, this) {
  thread.setName("request-count-reporter");
}


void RequestCounter::account(const RedisRequest &req, Statistics *stats) {
  if(req.getCommandType() == CommandType::READ) {
    stats->reads++;
  }
  else if(req.getCommandType() == CommandType::WRITE) {
    stats->writes++;
  }
}

void RequestCounter::account(const RedisRequest &req) {
  Statistics *stats = aggregator.getStats();
  account(req, stats);
}

void RequestCounter::account(const Transaction &transaction) {
  Statistics *stats = aggregator.getStats();

  if(transaction.containsWrites()) {
    stats->txreadwrite++;
  }
  else {
    stats->txread++;
  }

  for(size_t i = 0; i < transaction.size(); i++) {
    account(transaction[i], stats);
  }
}

std::string RequestCounter::toRate(int64_t val) {
  return SSTR("(" << val / interval.count() << " Hz)");
}

void RequestCounter::setReportingStatus(bool val) {
  activated = val;
}

Statistics RequestCounter::getOverallStats() {
  return aggregator.getOverallStats();
}

void RequestCounter::mainThread(ThreadAssistant &assistant) {
  while(!assistant.terminationRequested()) {

    Statistics local = aggregator.getOverallStatsSinceLastTime();

    if(local.reads != 0 || local.writes != 0) {
      paused = false;
      if(activated) {
        qdb_info("During the last " << interval.count() << " seconds, I serviced " << local.reads << " reads " << toRate(local.reads) <<  ", and " << local.writes << " writes " << toRate(local.writes) << " over " << local.txreadwrite << " write transactions");
      }
    }
    else if(!paused) {
      paused = true;
      if(activated) {
        qdb_info("No reads or writes during the last " << interval.count() << " seconds - will report again once load re-appears.");
      }
    }

    historical.push(local, std::chrono::system_clock::now());
    assistant.wait_for(interval);
  }
}

void RequestCounter::fillHistorical(std::vector<std::string> &headers,
  std::vector<std::vector<std::string>> &data) {

  headers.clear();
  data.clear();

  headers.emplace_back("TOTALS");
  data.emplace_back(aggregator.getOverallStats().serialize());

  historical.serialize(headers, data);
}
