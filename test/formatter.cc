// ----------------------------------------------------------------------
// File: response-formatter.cc
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

#include "Formatter.hh"
#include "redis/ArrayResponseBuilder.hh"
#include "qclient/ResponseBuilder.hh"
#include "qclient/QClient.hh"
#include <gtest/gtest.h>

using namespace quarkdb;
using namespace qclient;

TEST(Response, T1) {
  ASSERT_EQ(Formatter::err("test").val, "-ERR test\r\n");
  ASSERT_EQ(Formatter::ok().val, "+OK\r\n");
  ASSERT_EQ(Formatter::pong().val, "+PONG\r\n");
  ASSERT_EQ(Formatter::null().val, "$-1\r\n");
  ASSERT_EQ(Formatter::status("test").val, "+test\r\n");
  ASSERT_EQ(Formatter::noauth("asdf").val, "-NOAUTH asdf\r\n");
  ASSERT_EQ(
    Formatter::multiply(Formatter::noauth("you shall not pass"), 3).val,
    "-NOAUTH you shall not pass\r\n-NOAUTH you shall not pass\r\n-NOAUTH you shall not pass\r\n"
  );
}

TEST(ArrayResponseBuilder, BasicSanity) {
  ArrayResponseBuilder builder(3, false);
  ASSERT_THROW(builder.buildResponse(), FatalException);

  builder.push_back(Formatter::ok());
  builder.push_back(Formatter::integer(999));
  builder.push_back(Formatter::string("whee"));
  ASSERT_THROW(builder.push_back(Formatter::integer(123)), FatalException);

  RedisEncodedResponse resp = builder.buildResponse();
  ASSERT_EQ(resp.val, "*3\r\n+OK\r\n:999\r\n$4\r\nwhee\r\n");
}

TEST(Formatter, subscribe) {
  qclient::ResponseBuilder builder;
  builder.feed(Formatter::subscribe(false, "channel-name", 3).val);

  redisReplyPtr ans;
  ASSERT_EQ(builder.pull(ans), qclient::ResponseBuilder::Status::kOk);

  ASSERT_EQ(qclient::describeRedisReply(ans),
    "1) \"subscribe\"\n"
    "2) \"channel-name\"\n"
    "3) (integer) 3\n");
}

TEST(Formatter, PushSubscribe) {
  qclient::ResponseBuilder builder;
  builder.feed(Formatter::subscribe(true, "channel-name", 3).val);

  redisReplyPtr ans;
  ASSERT_EQ(builder.pull(ans), qclient::ResponseBuilder::Status::kOk);
  ASSERT_EQ(ans->type, REDIS_REPLY_PUSH);

  ASSERT_EQ(qclient::describeRedisReply(ans),
    "1) \"pubsub\"\n"
    "2) \"subscribe\"\n"
    "3) \"channel-name\"\n"
    "4) (integer) 3\n");
}

TEST(Formatter, psubscribe) {
  qclient::ResponseBuilder builder;
  builder.feed(Formatter::psubscribe(false, "channel-*", 4).val);

  redisReplyPtr ans;
  ASSERT_EQ(builder.pull(ans), qclient::ResponseBuilder::Status::kOk);

  ASSERT_EQ(qclient::describeRedisReply(ans),
    "1) \"psubscribe\"\n"
    "2) \"channel-*\"\n"
    "3) (integer) 4\n");
}

TEST(Formatter, PushPsubscribe) {
  qclient::ResponseBuilder builder;
  builder.feed(Formatter::psubscribe(true, "channel-*", 4).val);

  redisReplyPtr ans;
  ASSERT_EQ(builder.pull(ans), qclient::ResponseBuilder::Status::kOk);
  ASSERT_EQ(ans->type, REDIS_REPLY_PUSH);

  ASSERT_EQ(qclient::describeRedisReply(ans),
    "1) \"pubsub\"\n"
    "2) \"psubscribe\"\n"
    "3) \"channel-*\"\n"
    "4) (integer) 4\n");
}


TEST(Formatter, unsubscribe) {
  qclient::ResponseBuilder builder;
  builder.feed(Formatter::unsubscribe(false, "channel-name", 5).val);

  redisReplyPtr ans;
  ASSERT_EQ(builder.pull(ans), qclient::ResponseBuilder::Status::kOk);

  ASSERT_EQ(qclient::describeRedisReply(ans),
    "1) \"unsubscribe\"\n"
    "2) \"channel-name\"\n"
    "3) (integer) 5\n");
}

TEST(Formatter, PushUnsubscribe) {
  qclient::ResponseBuilder builder;
  builder.feed(Formatter::unsubscribe(true, "channel-name", 5).val);

  redisReplyPtr ans;
  ASSERT_EQ(builder.pull(ans), qclient::ResponseBuilder::Status::kOk);
  ASSERT_EQ(ans->type, REDIS_REPLY_PUSH);

  ASSERT_EQ(qclient::describeRedisReply(ans),
    "1) \"pubsub\"\n"
    "2) \"unsubscribe\"\n"
    "3) \"channel-name\"\n"
    "4) (integer) 5\n");
}


TEST(Formatter, message) {
  qclient::ResponseBuilder builder;
  builder.feed(Formatter::message(false, "channel", "payload").val);

  redisReplyPtr ans;
  ASSERT_EQ(builder.pull(ans), qclient::ResponseBuilder::Status::kOk);

  ASSERT_EQ(qclient::describeRedisReply(ans),
    "1) \"message\"\n"
    "2) \"channel\"\n"
    "3) \"payload\"\n");
}

TEST(Formatter, PushMessage) {
  qclient::ResponseBuilder builder;
  builder.feed(Formatter::message(true, "channel", "payload").val);

  redisReplyPtr ans;
  ASSERT_EQ(builder.pull(ans), qclient::ResponseBuilder::Status::kOk);
  ASSERT_EQ(ans->type, REDIS_REPLY_PUSH);

  ASSERT_EQ(qclient::describeRedisReply(ans),
    "1) \"pubsub\"\n"
    "2) \"message\"\n"
    "3) \"channel\"\n"
    "4) \"payload\"\n");
}

TEST(Formatter, pmessage) {
  qclient::ResponseBuilder builder;
  builder.feed(Formatter::pmessage(false, "pattern", "channel", "payload").val);

  redisReplyPtr ans;
  ASSERT_EQ(builder.pull(ans), qclient::ResponseBuilder::Status::kOk);

  ASSERT_EQ(qclient::describeRedisReply(ans),
    "1) \"pmessage\"\n"
    "2) \"pattern\"\n"
    "3) \"channel\"\n"
    "4) \"payload\"\n");
}

TEST(Formatter, PushPMessage) {
  qclient::ResponseBuilder builder;
  builder.feed(Formatter::pmessage(true, "pattern", "channel", "payload").val);

  redisReplyPtr ans;
  ASSERT_EQ(builder.pull(ans), qclient::ResponseBuilder::Status::kOk);
  ASSERT_EQ(ans->type, REDIS_REPLY_PUSH);

  ASSERT_EQ(qclient::describeRedisReply(ans),
    "1) \"pubsub\"\n"
    "2) \"pmessage\"\n"
    "3) \"pattern\"\n"
    "4) \"channel\"\n"
    "5) \"payload\"\n");
}

TEST(Formatter, VersionedVector) {
  qclient::ResponseBuilder builder;
  builder.feed(Formatter::versionedVector(999, {"one", "two", "three", "four" } ).val);

  redisReplyPtr ans;
  ASSERT_EQ(builder.pull(ans), qclient::ResponseBuilder::Status::kOk);

  ASSERT_EQ(qclient::describeRedisReply(ans),
    "1) (integer) 999\n"
    "2) 1) \"one\"\n"
    "   2) \"two\"\n"
    "   3) \"three\"\n"
    "   4) \"four\"\n");
}

TEST(Formatter, EmptyVersionedVector) {
  qclient::ResponseBuilder builder;
  builder.feed(Formatter::versionedVector(888, {} ).val);

  redisReplyPtr ans;
  ASSERT_EQ(builder.pull(ans), qclient::ResponseBuilder::Status::kOk);

  ASSERT_EQ(qclient::describeRedisReply(ans),
    "1) (integer) 888\n"
    "2) (empty list or set)\n");
}

TEST(Formatter, VectorOfVectors) {
  std::vector<std::string> headers;
  std::vector<std::vector<std::string>> data;

  headers.emplace_back("SECTION 1");
  ASSERT_THROW(Formatter::vectorsWithHeaders(headers, data), FatalException);

  std::vector<std::string> vec = { "one", "two", "three" };
  data.emplace_back(vec);

  headers.emplace_back("SECTION 2");
  vec = { "four", "five", "six" };
  data.emplace_back(vec);

  redisReplyPtr ans = qclient::ResponseBuilder::parseRedisEncodedString(Formatter::vectorsWithHeaders(headers, data).val);
  ASSERT_EQ(qclient::describeRedisReply(ans),
    "1) 1) SECTION 1\n"
    "   2) 1) one\n"
    "      2) two\n"
    "      3) three\n"
    "2) 1) SECTION 2\n"
    "   2) 1) four\n"
    "      2) five\n"
    "      3) six\n");
}

TEST(Formatter, NodeHealth) {
  std::vector<HealthIndicator> indicators;
  indicators.emplace_back(HealthStatus::kRed, "CHICKEN-INVASION", "Imminent");
  indicators.emplace_back(HealthStatus::kGreen, "BEARS", "Sleeping");

  NodeHealth nodeHealth("1.33.7", "example.com:7777", indicators);

  qclient::ResponseBuilder builder;
  builder.feed(Formatter::nodeHealth(nodeHealth).val);

  redisReplyPtr ans;
  ASSERT_EQ(builder.pull(ans), qclient::ResponseBuilder::Status::kOk);

  ASSERT_EQ(qclient::describeRedisReply(ans),
    "1) NODE-HEALTH RED\n"
    "2) NODE example.com:7777\n"
    "3) VERSION 1.33.7\n"
    "4) ----------\n"
    "5) RED    >> CHICKEN-INVASION Imminent\n"
    "6) GREEN  >> BEARS Sleeping\n"
  );
}

TEST(Formatter, VHashRevision) {
  std::vector<std::string> contents = { "key1", "value1", "key2", "value2" };
  std::vector<std::pair<std::string_view, std::string_view>> batch = { {contents[0], contents[1]}, {contents[2], contents[3]} };

  ASSERT_EQ(qclient::ResponseBuilder::parseAndDescribeRedisEncodedString(Formatter::vhashRevision(5, batch).val),
    "1) (integer) 5\n"
    "2) 1) \"key1\"\n"
    "   2) \"value1\"\n"
    "   3) \"key2\"\n"
    "   4) \"value2\"\n"
  );
}
