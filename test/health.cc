// ----------------------------------------------------------------------
// File: health.cc
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

#include "health/HealthIndicator.hh"

#include <gtest/gtest.h>
using namespace quarkdb;

TEST(HealthStatus, ToString) {
  ASSERT_EQ(healthStatusAsString(HealthStatus::kGreen), "GREEN");
  ASSERT_EQ(healthStatusAsString(HealthStatus::kYellow), "YELLOW");
  ASSERT_EQ(healthStatusAsString(HealthStatus::kRed), "RED");
}

TEST(HealthIndicator, BasicSanity) {
  HealthIndicator ind1(HealthStatus::kGreen, "Available space", "120 GB");
  ASSERT_EQ(ind1.getStatus(), HealthStatus::kGreen);
  ASSERT_EQ(ind1.getDescription(), "Available space");
  ASSERT_EQ(ind1.getMessage(), "120 GB");

  ASSERT_EQ(ind1.toString(), "[GREEN] Available space: 120 GB");
}