// Copyright 2026 rcllite contributors
// Licensed under the Apache License, Version 2.0
#ifndef RCLLITE__NAMES_HPP_
#define RCLLITE__NAMES_HPP_

#include <string>

namespace rcl {

/// Remap a fully-qualified ROS 2 topic name ("/foo/bar") to the DDS topic
/// name used by all ROS 2 rmw implementations ("rt/foo/bar").
std::string ros_to_dds_topic_name(const std::string& ros_fqn);

/// "/foo/bar" -> "rq/foo/barRequest"
std::string ros_to_dds_request_topic_name(const std::string& ros_fqn);

/// "/foo/bar" -> "rr/foo/barReply"
std::string ros_to_dds_reply_topic_name(const std::string& ros_fqn);

/// Join a node namespace ("/ns" or "/") and node name into the fully
/// qualified node name ("/ns/name").  The namespace must start with '/' and
/// the name must be a single token.
std::string make_fq_node_name(const std::string& ns, const std::string& name);

/// Expand "foo" in namespace "/ns" into "/ns/foo"; "/foo" stays "/foo".
std::string expand_topic_name(const std::string& topic, const std::string& ns);

}  // namespace rcl

#endif  // RCLLITE__NAMES_HPP_
