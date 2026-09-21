// Copyright 2026 rcllite contributors
// Licensed under the Apache License, Version 2.0
#include "rcllite/graph_executor.hpp"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "rcllite/dds/entities.hpp"
#include "rcllite/exception.hpp"
#include "rcllite/executor.hpp"
#include "rcllite/json_writer.hpp"
#include "rcllite/names.hpp"
#include "rcllite/node.hpp"
#include "rcllite/node_params.hpp"
#include "rcllite/node_registry.hpp"
#include "rcllite/qos.hpp"
#include "vila/config/configuration.h"

namespace rcl {
namespace {

using Json = vila::Json;

/// "00010000ab" -> {0x00,0x01,0x00,0x00,0xab}
std::vector<uint8_t> payload_from_hex(const std::string& hex, const std::string& what) {
  if (hex.size() % 2 != 0) {
    throw Error(vila::InvalidArguments("{}: odd hex length", what));
  }
  std::vector<uint8_t> bytes;
  bytes.reserve(hex.size() / 2);
  for (size_t i = 0; i < hex.size(); i += 2) {
    const auto nibble = [&](char c) -> int {
      if (c >= '0' && c <= '9') return c - '0';
      if (c >= 'a' && c <= 'f') return c - 'a' + 10;
      if (c >= 'A' && c <= 'F') return c - 'A' + 10;
      return -1;
    };
    const int hi = nibble(hex[i]);
    const int lo = nibble(hex[i + 1]);
    if (hi < 0 || lo < 0) {
      throw Error(vila::InvalidArguments("{}: invalid hex digit", what));
    }
    bytes.push_back(static_cast<uint8_t>((hi << 4) | lo));
  }
  return bytes;
}

/// Bare CDR-LE encapsulation header — the default payload (a valid empty
/// message for field-less types; receivers of typed topics will drop it).
std::vector<uint8_t> default_payload() { return {0x00, 0x01, 0x00, 0x00}; }

/// Accept the full DDS type name ("pkg::msg::dds_::Name_") or the friendly
/// ROS form ("pkg/msg/Name").
std::string normalize_type(const std::string& type) {
  if (type.find("::") != std::string::npos) {
    return type;
  }
  const size_t cut = type.rfind('/');
  if (cut == std::string::npos) {
    throw Error(vila::InvalidArguments(
        "type must be 'pkg/pkg/Name' or a full DDS name, got '{}'", type));
  }
  std::string dds_ns = type.substr(0, cut);
  const size_t slash = dds_ns.find('/');
  if (slash != std::string::npos) {
    dds_ns.replace(slash, 1, "::");
  }
  return dds_ns + "::dds_::" + type.substr(cut + 1) + "_";
}

QoS parse_qos(const Json& qos, QoS base) {
  if (!qos.is_object()) {
    return base;
  }
  if (qos.value("reliability", "reliable") == "best_effort") {
    base.set_best_effort();
  }
  if (qos.value("durability", "volatile") == "transient_local") {
    base.set_transient_local();
  }
  if (qos.contains("depth")) {
    base.keep_last(qos.at("depth").get<size_t>());
  }
  return base;
}

}  // namespace

struct GraphExecutor::Impl {
  // A raw publisher: opaque CDR writer on the node's participant; not a
  // waitable entity (publishing is push-only).
  struct RawPublisher {
    std::string topic;  // ROS name for reporting
    std::string type;
    std::unique_ptr<dds::Writer> writer;
    std::vector<uint8_t> payload;
    double rate_hz = 0.0;
    std::atomic<uint64_t> published{0};
  };

  // A raw subscription: counts valid samples, drops the bytes.
  struct RawSubscription : public EntityBase {
    std::string topic;
    std::string type;
    std::unique_ptr<dds::Reader> reader;
    std::atomic<uint64_t> received{0};

    dds_entity_t condition() const override { return reader->read_condition(); }

    void dispatch() override {
      reader->take([this](const uint8_t*, size_t, const dds_sample_info_t& info) {
        if (info.valid_data) {
          received.fetch_add(1);
        }
      });
    }
  };

  struct NodeEntry {
    std::string fq_name;
    std::string namespace_;
    std::shared_ptr<Node> node;
    std::vector<std::shared_ptr<RawPublisher>> publishers;
    std::vector<std::shared_ptr<RawSubscription>> subscriptions;
  };

  explicit Impl(const std::string& json_topology) {
    Json doc;
    try {
      doc = vila::JsonFromDoc(json_topology);
    } catch (const std::exception& e) {
      throw Error(vila::InvalidArguments("topology is not valid JSON: {}", e.what()));
    }
    if (!doc.is_object() || !doc.contains("nodes") || !doc.at("nodes").is_array()) {
      throw Error(
          vila::InvalidArguments("topology must be an object with a 'nodes' array"));
    }
    if (doc.at("nodes").empty()) {
      throw Error(vila::InvalidArguments("topology has no nodes"));
    }
    for (const Json& spec : doc.at("nodes")) {
      build_node(spec);
    }
  }

  ~Impl() {
    running = false;
    executor.cancel();
    for (auto& thread : pub_threads) {
      if (thread.joinable()) {
        thread.join();
      }
    }
  }

  void build_node(const Json& spec) {
    if (!spec.is_object() || !spec.contains("name")) {
      throw Error(vila::InvalidArguments("every node needs a 'name'"));
    }
    NodeEntry entry;
    entry.namespace_ = spec.value("namespace", "/");
    const std::string name = spec.at("name").get<std::string>();
    // "class" opts into a registered behavior-carrying subclass; without it
    // the node is a plain skeleton driven purely by the config below.
    if (spec.contains("class")) {
      const std::string cls = spec.at("class").get<std::string>();
      entry.node = NodeRegistry::CreateByName(cls, name, entry.namespace_);
      if (!entry.node) {
        throw Error(vila::InvalidArguments(
            "node {}/{}: class '{}' is not registered; link its library "
            "(alwayslink = 1) into the hosting binary",
            entry.namespace_, name, cls));
      }
    } else {
      entry.node = std::make_shared<Node>(name, entry.namespace_);
    }
    entry.fq_name = entry.node->get_fully_qualified_name();
    executor.add_node(entry.node);

    if (spec.contains("parameters")) {
      const Json& params = spec.at("parameters");
      if (!params.is_object()) {
        throw Error(vila::InvalidArguments("node {}: 'parameters' must be an object",
                                           entry.fq_name));
      }
      for (auto it = params.begin(); it != params.end(); ++it) {
        declare_parameter(*entry.node, entry.fq_name, it.key(), it.value());
      }
    }

    if (spec.contains("publishers")) {
      for (const Json& pub : spec.at("publishers")) {
        build_publisher(entry, pub);
      }
    }
    if (spec.contains("subscriptions")) {
      for (const Json& sub : spec.at("subscriptions")) {
        build_subscription(entry, sub);
      }
    }
    nodes.push_back(std::move(entry));
  }

  static void declare_parameter(Node& node, const std::string& fq,
                                const std::string& name, const Json& value) {
    if (value.is_boolean()) {
      node.declare_parameter(name, value.get<bool>());
    } else if (value.is_number_integer()) {
      node.declare_parameter(name, value.get<int64_t>());
    } else if (value.is_number_float()) {
      node.declare_parameter(name, value.get<double>());
    } else if (value.is_string()) {
      node.declare_parameter(name, value.get<std::string>());
    } else {
      throw Error(vila::InvalidArguments(
          "node {}: parameter '{}' must be bool/int/double/string", fq, name));
    }
  }

  void build_publisher(NodeEntry& entry, const Json& spec) {
    const std::string what = "node " + entry.fq_name + " publisher";
    if (!spec.is_object() || !spec.contains("topic") || !spec.contains("type")) {
      throw Error(vila::InvalidArguments("{}: needs 'topic' and 'type'", what));
    }
    auto pub = std::make_shared<RawPublisher>();
    pub->type = normalize_type(spec.at("type").get<std::string>());
    const std::string ros_topic =
        expand_topic_name(spec.at("topic").get<std::string>(), entry.namespace_);
    pub->topic = ros_topic;
    pub->payload =
        spec.contains("payload_hex")
            ? payload_from_hex(spec.at("payload_hex").get<std::string>(), what)
            : default_payload();
    pub->rate_hz = spec.value("rate_hz", 0.0);
    pub->writer = std::make_unique<dds::Writer>(
        entry.node->participant().handle(), entry.node->participant().publisher(),
        ros_to_dds_topic_name(ros_topic), pub->type,
        parse_qos(spec.value("qos", Json::object()), QoS::Default()));
    entry.publishers.push_back(std::move(pub));
  }

  void build_subscription(NodeEntry& entry, const Json& spec) {
    const std::string what = "node " + entry.fq_name + " subscription";
    if (!spec.is_object() || !spec.contains("topic") || !spec.contains("type")) {
      throw Error(vila::InvalidArguments("{}: needs 'topic' and 'type'", what));
    }
    auto sub = std::make_shared<RawSubscription>();
    sub->type = normalize_type(spec.at("type").get<std::string>());
    const std::string ros_topic =
        expand_topic_name(spec.at("topic").get<std::string>(), entry.namespace_);
    sub->topic = ros_topic;
    sub->reader = std::make_unique<dds::Reader>(
        entry.node->participant().handle(), entry.node->participant().subscriber(),
        ros_to_dds_topic_name(ros_topic), sub->type,
        parse_qos(spec.value("qos", Json::object()), QoS::Default()));
    entry.node->add_entity(sub);
    entry.subscriptions.push_back(std::move(sub));
  }

  /// One-shot payload sends + one thread per periodic publisher.
  void start_publishing() {
    for (NodeEntry& entry : nodes) {
      for (const auto& pub : entry.publishers) {
        if (pub->rate_hz > 0.0) {
          pub_threads.emplace_back([this, pub] {
            const auto period = std::chrono::duration<double>(1.0 / pub->rate_hz);
            auto deadline = std::chrono::steady_clock::now();
            while (running.load()) {
              if (pub->writer->write(pub->payload.data(), pub->payload.size())) {
                pub->published.fetch_add(1);
              }
              deadline +=
                  std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                      period);
              std::this_thread::sleep_until(deadline);
            }
          });
        } else {
          if (pub->writer->write(pub->payload.data(), pub->payload.size())) {
            pub->published.fetch_add(1);
          }
        }
      }
    }
  }

  // Declared first so it is destroyed last: the executor holds shared node
  // references that must drop after the endpoint owners are gone.
  Executor executor;
  std::vector<NodeEntry> nodes;
  std::atomic<bool> running{true};
  std::vector<std::thread> pub_threads;
};

GraphExecutor::GraphExecutor(const std::string& json_topology)
    : impl_(std::make_unique<Impl>(json_topology)) {
  impl_->start_publishing();
}

GraphExecutor::~GraphExecutor() = default;

std::string GraphExecutor::registered_nodes_json() {
  const auto registered = NodeRegistry::GetRegisteredNames();
  std::vector<std::string> names(registered.begin(), registered.end());
  std::sort(names.begin(), names.end());
  std::string out = "[";
  bool first = true;
  for (const auto& name : names) {
    out += first ? "" : ",";
    out += json_escape_string(name);
    first = false;
  }
  return out + "]";
}

void GraphExecutor::spin() { impl_->executor.spin(); }

bool GraphExecutor::spin_once(std::chrono::nanoseconds timeout) {
  return impl_->executor.spin_once(timeout);
}

void GraphExecutor::cancel() { impl_->executor.cancel(); }

std::string GraphExecutor::status_json() const {
  std::string out = "{\"nodes\":[";
  bool first_node = true;
  for (const auto& entry : impl_->nodes) {
    out += first_node ? "{" : ",{";
    out += "\"name\":" + json_escape_string(entry.fq_name);
    out += ",\"publishers\":[";
    bool first = true;
    for (const auto& pub : entry.publishers) {
      out += first ? "{" : ",{";
      out += "\"topic\":" + json_escape_string(pub->topic);
      out += ",\"type\":" + json_escape_string(pub->type);
      out += ",\"published\":" + std::to_string(pub->published.load());
      out += "}";
      first = false;
    }
    out += "],\"subscriptions\":[";
    first = true;
    for (const auto& sub : entry.subscriptions) {
      out += first ? "{" : ",{";
      out += "\"topic\":" + json_escape_string(sub->topic);
      out += ",\"type\":" + json_escape_string(sub->type);
      out += ",\"received\":" + std::to_string(sub->received.load());
      out += "}";
      first = false;
    }
    out += "]}";
    first_node = false;
  }
  out += "]}";
  return out;
}

}  // namespace rcl
