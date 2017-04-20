// ----------------------------------------------------------------------
// File: utils.cc
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

#include <gtest/gtest.h>
#include "Utils.hh"

using namespace quarkdb;

TEST(Utils, binary_string_int_conversion) {
  EXPECT_EQ(intToBinaryString(1), std::string("\x00\x00\x00\x00\x00\x00\x00\x01", 8));
  EXPECT_EQ(binaryStringToInt("\x00\x00\x00\x00\x00\x00\x00\x01"), 1);

  EXPECT_EQ(binaryStringToInt(intToBinaryString(1).data()), 1);
  EXPECT_EQ(binaryStringToInt(intToBinaryString(2).c_str()), 2);
  EXPECT_EQ(binaryStringToInt(intToBinaryString(123415).c_str()), 123415);
  EXPECT_EQ(binaryStringToInt(intToBinaryString(17465798).c_str()), 17465798);
  EXPECT_EQ(binaryStringToInt(intToBinaryString(16583415634).c_str()), 16583415634);
  EXPECT_EQ(binaryStringToInt(intToBinaryString(-1234169761).c_str()), -1234169761);
}

TEST(Utils, binary_string_unsigned_int_conversion) {
  EXPECT_EQ(unsignedIntToBinaryString(1u), std::string("\x00\x00\x00\x00\x00\x00\x00\x01", 8));
  EXPECT_EQ(binaryStringToUnsignedInt("\x00\x00\x00\x00\x00\x00\x00\x01"), 1u);

  EXPECT_EQ(binaryStringToUnsignedInt(unsignedIntToBinaryString(1u).data()), 1u);
  EXPECT_EQ(binaryStringToUnsignedInt(unsignedIntToBinaryString(2u).c_str()), 2u);
  EXPECT_EQ(binaryStringToUnsignedInt(unsignedIntToBinaryString(123415u).c_str()), 123415u);
  EXPECT_EQ(binaryStringToUnsignedInt(unsignedIntToBinaryString(17465798u).c_str()), 17465798u);
  EXPECT_EQ(binaryStringToUnsignedInt(unsignedIntToBinaryString(16583415634u).c_str()), 16583415634u);
  EXPECT_EQ(binaryStringToUnsignedInt(unsignedIntToBinaryString(18446744073709551613u).c_str()), 18446744073709551613u);

  uint64_t big_number = std::numeric_limits<uint64_t>::max() / 2;
  EXPECT_EQ(binaryStringToUnsignedInt(unsignedIntToBinaryString(big_number).c_str()), big_number);
}

TEST(Utils, pathJoin) {
  ASSERT_EQ(pathJoin("/home/", "test"), "/home/test");
  ASSERT_EQ(pathJoin("/home", "test"), "/home/test");
  ASSERT_EQ(pathJoin("", "home"), "/home");
  ASSERT_EQ(pathJoin("/home", ""), "/home");
}
