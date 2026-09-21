// Copyright 2026 rcllite contributors
// Licensed under the Apache License, Version 2.0
#include "rcllite/names.hpp"

#include <gtest/gtest.h>

#include "rcllite/exception.hpp"

namespace {

using rcl::expand_topic_name;
using rcl::make_fq_node_name;
using rcl::ros_to_dds_reply_topic_name;
using rcl::ros_to_dds_request_topic_name;
using rcl::ros_to_dds_topic_name;

TEST(Names, TopicMapping) {
  EXPECT_EQ(ros_to_dds_topic_name("/chatter"), "rt/chatter");
  EXPECT_EQ(ros_to_dds_topic_name("/ns/deep/topic"), "rt/ns/deep/topic");
}

TEST(Names, ServiceTopicMapping) {
  EXPECT_EQ(ros_to_dds_request_topic_name("/add_two_ints"), "rq/add_two_intsRequest");
  EXPECT_EQ(ros_to_dds_reply_topic_name("/add_two_ints"), "rr/add_two_intsReply");
  EXPECT_EQ(ros_to_dds_request_topic_name("/ns/svc"), "rq/ns/svcRequest");
}

TEST(Names, FullyQualifiedName) {
  EXPECT_EQ(make_fq_node_name("/", "talker"), "/talker");
  EXPECT_EQ(make_fq_node_name("/ns", "talker"), "/ns/talker");
}

TEST(Names, ExpandTopicName) {
  EXPECT_EQ(expand_topic_name("chatter", "/"), "/chatter");
  EXPECT_EQ(expand_topic_name("chatter", "/robot"), "/robot/chatter");
  EXPECT_EQ(expand_topic_name("/abs", "/robot"), "/abs");
}

TEST(Names, RejectsInvalidInput) {
  EXPECT_THROW(ros_to_dds_topic_name("chatter"),
               rcl::Error);                                  // missing leading slash
  EXPECT_THROW(ros_to_dds_topic_name("/a//b"), rcl::Error);  // empty token
  EXPECT_THROW(make_fq_node_name("/", "a/b"), rcl::Error);   // name with slash
  EXPECT_THROW(expand_topic_name("foo/bar", "/ns"), rcl::Error);  // sub-namespace
  EXPECT_THROW(expand_topic_name("", "/ns"), rcl::Error);         // empty topic
}

}  // namespace
