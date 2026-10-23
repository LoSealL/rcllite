// Copyright 2026 rcllite contributors
// Licensed under the Apache License, Version 2.0
#include "rcllite/names.hpp"

#include "rcllite/exception.hpp"

namespace {

/// ROS 2 topic names must be non-empty and consist of "name characters"
/// (alphanumerics, '_') separated by single slashes.
void validate_fqn(const std::string& fqn) {
  if (fqn.empty() || fqn.front() != '/') {
    throw rcl::Error(
        vila::InvalidArguments("fully qualified name must start with '/': {}", fqn));
  }
  if (fqn.size() > 1 && fqn.back() == '/') {
    throw rcl::Error(
        vila::InvalidArguments("fully qualified name must not end with '/': {}", fqn));
  }
  if (fqn.find("//") != std::string::npos) {
    throw rcl::Error(
        vila::InvalidArguments("fully qualified name contains empty token: {}", fqn));
  }
}

}  // namespace

namespace rcl {

std::string ros_to_dds_topic_name(const std::string& ros_fqn) {
  validate_fqn(ros_fqn);
  return "rt" + ros_fqn;
}

std::string ros_to_dds_request_topic_name(const std::string& ros_fqn) {
  validate_fqn(ros_fqn);
  return "rq" + ros_fqn + "Request";
}

std::string ros_to_dds_reply_topic_name(const std::string& ros_fqn) {
  validate_fqn(ros_fqn);
  return "rr" + ros_fqn + "Reply";
}

std::string make_fq_node_name(const std::string& ns, const std::string& name) {
  if (name.empty() || name.find('/') != std::string::npos) {
    throw rcl::Error(vila::InvalidArguments("invalid node name: {}", name));
  }
  if (ns.empty() || ns.front() != '/' || (ns.size() > 1 && ns.back() == '/')) {
    throw rcl::Error(vila::InvalidArguments("invalid namespace: {}", ns));
  }
  if (ns == "/") {
    return "/" + name;
  }
  return ns + "/" + name;
}

std::string dds_type_name(const std::string& type) {
  if (type.find("::") != std::string::npos) {
    return type;
  }
  const size_t cut = type.rfind('/');
  if (cut == std::string::npos) {
    throw rcl::Error(vila::InvalidArguments(
        "type must be 'pkg/pkg/Name' or a full DDS name, got '{}'", type));
  }
  std::string dds_ns = type.substr(0, cut);
  const size_t slash = dds_ns.find('/');
  if (slash != std::string::npos) {
    dds_ns.replace(slash, 1, "::");
  }
  return dds_ns + "::dds_::" + type.substr(cut + 1) + "_";
}

std::string expand_topic_name(const std::string& topic, const std::string& ns) {
  if (topic.empty()) {
    throw rcl::Error(vila::InvalidArguments("topic name must not be empty"));
  }
  if (topic.front() == '/') {
    return topic;
  }
  if (topic.find('/') != std::string::npos) {
    // "foo/bar" style private namespaces are not supported by rcllite.
    throw rcl::Error(vila::InvalidArguments(
        "relative topic with sub-namespace is not supported: {}", topic));
  }
  if (ns == "/" || ns.empty()) {
    return "/" + topic;
  }
  return ns + "/" + topic;
}

}  // namespace rcl
