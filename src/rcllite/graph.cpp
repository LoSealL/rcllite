// Copyright 2026 rcllite contributors
// Licensed under the Apache License, Version 2.0
#include "rcllite/graph.hpp"

#include <atomic>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include "dds/dds.h"
#include "rcllite/cdr.hpp"
#include "rcllite/dds/entities.hpp"
#include "rcllite/json_writer.hpp"
#include "rcllite/msg_traits.hpp"
#include "rcllite/names.hpp"
#include "rcllite_types/rcl_interfaces/log.hpp"
#include "rcllite_types/rmw_dds_common/participant_entities_info.hpp"

namespace rcl {
namespace {

/// Ring bound for the rt/rosout records kept in the snapshot.
constexpr size_t kMaxLogs = 512;
/// Batch size for built-in topic takes.
constexpr size_t kBuiltinBatch = 32;

struct GuidLess {
  bool operator()(const dds_guid_t& a, const dds_guid_t& b) const {
    return std::memcmp(a.v, b.v, sizeof(a.v)) < 0;
  }
};

bool guid_eq(const dds_guid_t& a, const dds_guid_t& b) {
  return std::memcmp(a.v, b.v, sizeof(a.v)) == 0;
}

/// The 24-byte (humble) Gid wire type only carries a 16-byte GUID.
bool gid_matches(const rmw_dds_common::msg::Gid& gid, const dds_guid_t& g) {
  return gid.data.size() >= sizeof(g.v) &&
         std::memcmp(gid.data.data(), g.v, sizeof(g.v)) == 0;
}

std::string guid_hex(const dds_guid_t& g) {
  static const char* digits = "0123456789abcdef";
  std::string out;
  out.reserve(sizeof(g.v) * 2);
  for (const uint8_t byte : g.v) {
    out += digits[byte >> 4];
    out += digits[byte & 0xf];
  }
  return out;
}

/// "enclave=/ns/name;" -> "/ns/name" ("" when absent or empty).
std::string parse_enclave(const std::string& user_data) {
  const std::string prefix = "enclave=";
  const size_t begin = user_data.find(prefix);
  if (begin == std::string::npos) {
    return "";
  }
  const size_t name_start = begin + prefix.size();
  const size_t semi = user_data.find(';', name_start);
  const size_t name_end = semi == std::string::npos ? user_data.size() : semi;
  return user_data.substr(name_start, name_end - name_start);
}

/// Topics that carry real traffic but would spray edges between every node
/// and every consumer; kept in the raw data, excluded from graph edges.
bool is_plumbing_topic(const std::string& dds_topic) {
  return dds_topic == "ros_discovery_info" || dds_topic == "rt/rosout" ||
         dds_topic == "rt/parameter_events" || dds_topic.rfind("DCPS", 0) == 0 ||
         dds_topic.rfind("d*", 0) == 0;
}

}  // namespace

struct GraphMonitor::Impl {
  struct EndpointInfo {
    dds_guid_t participant{};
    std::string topic;
    std::string type;
    bool writer = false;
  };

  Impl() : ppant(dds::Context::instance().domain_id(), "") {
    // The monitor never announces itself; its endpoints are filtered out by
    // self_guid so the graph shows only the observed system.
    dds_get_guid(ppant.handle(), &self_guid);

    bi_part = dds_create_reader(ppant.handle(), DDS_BUILTIN_TOPIC_DCPSPARTICIPANT,
                                nullptr, nullptr);
    bi_pub = dds_create_reader(ppant.handle(), DDS_BUILTIN_TOPIC_DCPSPUBLICATION,
                               nullptr, nullptr);
    bi_sub = dds_create_reader(ppant.handle(), DDS_BUILTIN_TOPIC_DCPSSUBSCRIPTION,
                               nullptr, nullptr);
    if (bi_part < 0 || bi_pub < 0 || bi_sub < 0) {
      throw Error(vila::InternalError("failed to create built-in topic readers"));
    }
    rc_part = dds_create_readcondition(bi_part, DDS_ANY_STATE);
    rc_pub = dds_create_readcondition(bi_pub, DDS_ANY_STATE);
    rc_sub = dds_create_readcondition(bi_sub, DDS_ANY_STATE);

    discovery_rd = std::make_unique<dds::Reader>(
        ppant.handle(), ppant.subscriber(), "ros_discovery_info",
        MessageType<rmw_dds_common::msg::ParticipantEntitiesInfo>::dds_name(),
        QoS().set_transient_local().keep_last(1));
    rosout_rd = std::make_unique<dds::Reader>(
        ppant.handle(), ppant.subscriber(), ros_to_dds_topic_name("/rosout"),
        MessageType<rcl_interfaces::msg::Log>::dds_name(),
        QoS().set_transient_local().keep_last(kMaxLogs));

    waitset = dds_create_waitset(DDS_CYCLONEDDS_HANDLE);
    if (waitset < 0 ||
        dds_waitset_attach(waitset, stop_guard.handle(), kStopSlot) < 0 ||
        dds_waitset_attach(waitset, rc_part, kBiPartSlot) < 0 ||
        dds_waitset_attach(waitset, rc_pub, kBiPubSlot) < 0 ||
        dds_waitset_attach(waitset, rc_sub, kBiSubSlot) < 0 ||
        dds_waitset_attach(waitset, discovery_rd->read_condition(), kDiscoverySlot) <
            0 ||
        dds_waitset_attach(waitset, rosout_rd->read_condition(), kRosoutSlot) < 0) {
      throw Error(vila::InternalError("failed to set up the graph waitset"));
    }
  }

  ~Impl() {
    running = false;
    stop_guard.trigger();
    if (thread.joinable()) {
      thread.join();
    }
    dds_delete(waitset);
  }

  // waitset attachment slots
  static constexpr dds_attach_t kStopSlot = 1;
  static constexpr dds_attach_t kBiPartSlot = 2;
  static constexpr dds_attach_t kBiPubSlot = 3;
  static constexpr dds_attach_t kBiSubSlot = 4;
  static constexpr dds_attach_t kDiscoverySlot = 5;
  static constexpr dds_attach_t kRosoutSlot = 6;

  void run() {
    constexpr size_t kMaxTriggered = 8;
    dds_attach_t triggered[kMaxTriggered];
    while (running.load()) {
      const dds_return_t n =
          dds_waitset_wait(waitset, triggered, kMaxTriggered, DDS_INFINITY);
      if (n < 0) {
        break;
      }
      for (dds_return_t i = 0; i < n; ++i) {
        switch (triggered[i]) {
          case kStopSlot:
            if (stop_guard.take()) {
              return;
            }
            break;
          case kBiPartSlot:
            poll_participants();
            break;
          case kBiPubSlot:
            poll_builtin(bi_pub, true);
            break;
          case kBiSubSlot:
            poll_builtin(bi_sub, false);
            break;
          case kDiscoverySlot:
            poll_discovery();
            break;
          case kRosoutSlot:
            poll_rosout();
            break;
          default:
            break;
        }
      }
    }
  }

  void poll_participants() {
    // ptrs[i] == NULL makes ddsc loan the sample storage and hand back
    // pointers into it; dds_return_loan is then the single owner-side free.
    // (Passing our own buffers shallow-copies the internal char*/qos
    // pointers, and freeing those twice corrupts the heap.)
    void* ptrs[kBuiltinBatch] = {};
    dds_sample_info_t infos[kBuiltinBatch];
    const dds_return_t n = dds_take(bi_part, ptrs, infos, kBuiltinBatch, kBuiltinBatch);
    if (n <= 0) {
      return;
    }
    const std::lock_guard<std::mutex> lock(mtx);
    for (dds_return_t i = 0; i < n; ++i) {
      const auto* sample = static_cast<const dds_builtintopic_participant_t*>(ptrs[i]);
      if (infos[i].instance_state == DDS_ALIVE_INSTANCE_STATE) {
        std::string enclave;
        if (sample->qos != nullptr) {
          void* value = nullptr;
          size_t size = 0;
          if (dds_qget_userdata(sample->qos, &value, &size) && value != nullptr) {
            enclave = parse_enclave(std::string(static_cast<const char*>(value), size));
          }
          if (value != nullptr) {
            dds_free(value);
          }
        }
        participants[sample->key] = std::move(enclave);
      } else {
        // A departed participant takes its discovery entry and endpoints
        // with it; this also retires stale ros_discovery_info instances,
        // whose own dispose samples are invalid-data and filtered by take().
        const dds_guid_t key = sample->key;
        participants.erase(key);
        discovery.erase(key);
        for (auto it = endpoints.begin(); it != endpoints.end();) {
          if (guid_eq(it->second.participant, key)) {
            it = endpoints.erase(it);
          } else {
            ++it;
          }
        }
      }
    }
    dds_return_loan(bi_part, ptrs, n);
  }

  void poll_builtin(dds_entity_t reader, bool writer) {
    void* ptrs[kBuiltinBatch] = {};  // loaned samples, see poll_participants
    dds_sample_info_t infos[kBuiltinBatch];
    const dds_return_t n = dds_take(reader, ptrs, infos, kBuiltinBatch, kBuiltinBatch);
    if (n <= 0) {
      return;
    }
    const std::lock_guard<std::mutex> lock(mtx);
    for (dds_return_t i = 0; i < n; ++i) {
      const auto* sample = static_cast<const dds_builtintopic_endpoint_t*>(ptrs[i]);
      if (infos[i].instance_state == DDS_ALIVE_INSTANCE_STATE) {
        EndpointInfo info;
        info.participant = sample->participant_key;
        info.topic = sample->topic_name != nullptr ? sample->topic_name : "";
        info.type = sample->type_name != nullptr ? sample->type_name : "";
        info.writer = writer;
        endpoints[sample->key] = std::move(info);
      } else {
        endpoints.erase(sample->key);
      }
    }
    dds_return_loan(reader, ptrs, n);
  }

  void poll_discovery() {
    discovery_rd->take(
        [this](const uint8_t* payload, size_t size, const dds_sample_info_t&) {
          CdrReader reader(payload, size);
          rmw_dds_common::msg::ParticipantEntitiesInfo msg{};
          if (!rmw_dds_common::msg::ParticipantEntitiesInfo::deserialize(msg, reader)) {
            return;
          }
          dds_guid_t key{};
          std::memcpy(key.v, msg.gid.data.data(), sizeof(key.v));
          const std::lock_guard<std::mutex> lock(mtx);
          discovery[key] = std::move(msg);
        });
  }

  void poll_rosout() {
    rosout_rd->take(
        [this](const uint8_t* payload, size_t size, const dds_sample_info_t&) {
          CdrReader reader(payload, size);
          rcl_interfaces::msg::Log msg{};
          if (!rcl_interfaces::msg::Log::deserialize(msg, reader)) {
            return;
          }
          const std::lock_guard<std::mutex> lock(mtx);
          logs.push_back(std::move(msg));
          while (logs.size() > kMaxLogs) {
            logs.pop_front();
          }
        });
  }

  /// Node owning `endpoint`: the discovery protocol's GID lists are
  /// authoritative, the participant enclave is rcllite's own signal.
  std::pair<std::string, const char*> resolve_node(const dds_guid_t& participant,
                                                   const dds_guid_t& endpoint) const {
    const auto dit = discovery.find(participant);
    if (dit != discovery.end()) {
      for (const auto& node_info : dit->second.node_entities_info_seq) {
        const auto owns =
            [&endpoint](const std::vector<rmw_dds_common::msg::Gid>& gids) {
              for (const auto& gid : gids) {
                if (gid_matches(gid, endpoint)) {
                  return true;
                }
              }
              return false;
            };
        if (owns(node_info.writer_gid_seq) || owns(node_info.reader_gid_seq)) {
          return {make_fq_node_name(node_info.node_namespace, node_info.node_name),
                  "discovery"};
        }
      }
    }
    const auto pit = participants.find(participant);
    if (pit != participants.end() && !pit->second.empty()) {
      return {pit->second, "user_data"};
    }
    return {"participant:" + guid_hex(participant), "anonymous"};
  }

  dds::Participant ppant;
  dds_entity_t bi_part = DDS_RETCODE_ERROR;
  dds_entity_t bi_pub = DDS_RETCODE_ERROR;
  dds_entity_t bi_sub = DDS_RETCODE_ERROR;
  dds_entity_t rc_part = DDS_RETCODE_ERROR;
  dds_entity_t rc_pub = DDS_RETCODE_ERROR;
  dds_entity_t rc_sub = DDS_RETCODE_ERROR;
  std::unique_ptr<dds::Reader> discovery_rd;
  std::unique_ptr<dds::Reader> rosout_rd;
  dds::GuardCondition stop_guard;
  dds_entity_t waitset = DDS_RETCODE_ERROR;
  dds_guid_t self_guid{};
  std::thread thread;
  std::atomic<bool> running{true};

  std::mutex mtx;
  std::map<dds_guid_t, std::string, GuidLess> participants;  // guid -> enclave
  std::map<dds_guid_t, EndpointInfo, GuidLess> endpoints;
  std::map<dds_guid_t, rmw_dds_common::msg::ParticipantEntitiesInfo, GuidLess>
      discovery;
  std::deque<rcl_interfaces::msg::Log> logs;
};

GraphMonitor::GraphMonitor() : impl_(std::make_unique<Impl>()) {
  impl_->thread = std::thread([impl = impl_.get()]() { impl->run(); });
}

GraphMonitor::~GraphMonitor() = default;

std::string GraphMonitor::snapshot_json() {
  Impl& impl = *impl_;
  const std::lock_guard<std::mutex> lock(impl.mtx);

  // Resolve every live endpoint to its owning node first.
  struct NodeAttrib {
    int rank = 0;  // 3 discovery, 2 user_data, 1 anonymous
    const char* source = "anonymous";
  };
  std::map<dds_guid_t,
           std::pair<Impl::EndpointInfo, std::pair<std::string, const char*>>, GuidLess>
      live;
  std::map<std::string, NodeAttrib> nodes;
  const auto note_node = [&nodes](const std::string& name, const char* source) {
    int rank = std::strcmp(source, "discovery") == 0
                   ? 3
                   : (std::strcmp(source, "user_data") == 0 ? 2 : 1);
    NodeAttrib& attrib = nodes[name];
    if (rank > attrib.rank) {
      attrib.rank = rank;
      attrib.source = source;
    }
  };

  for (const auto& [guid, info] : impl.endpoints) {
    if (guid_eq(info.participant, impl.self_guid)) {
      continue;  // the observer itself stays invisible
    }
    auto owner = impl.resolve_node(info.participant, guid);
    note_node(owner.first, owner.second);
    live.emplace(guid, std::make_pair(info, std::move(owner)));
  }
  for (const auto& [guid, info] : impl.discovery) {
    if (guid_eq(guid, impl.self_guid)) {
      continue;
    }
    for (const auto& node_info : info.node_entities_info_seq) {
      note_node(make_fq_node_name(node_info.node_namespace, node_info.node_name),
                "discovery");
    }
  }
  for (const auto& [guid, enclave] : impl.participants) {
    if (guid_eq(guid, impl.self_guid) || enclave.empty()) {
      continue;
    }
    note_node(enclave, "user_data");
  }

  // Aggregate endpoints per DDS topic, then emit data-flow edges.
  struct TopicAgg {
    std::string type;
    std::set<std::string> writers;  // node names
    std::set<std::string> readers;
  };
  std::map<std::string, TopicAgg> topics;
  for (const auto& [guid, entry] : live) {
    const Impl::EndpointInfo& info = entry.first;
    const std::string& node = entry.second.first;
    TopicAgg& agg = topics[info.topic];
    agg.type = info.type;
    (info.writer ? agg.writers : agg.readers).insert(node);
  }

  std::set<std::tuple<std::string, std::string, std::string, const char*>> edges;
  for (const auto& [dds_topic, agg] : topics) {
    if (is_plumbing_topic(dds_topic)) {
      continue;
    }
    if (dds_topic.rfind("rq/", 0) == 0 && dds_topic.size() > 10 &&
        dds_topic.compare(dds_topic.size() - 7, 7, "Request") == 0) {
      // Service request topic: clients write, the server reads.
      const std::string service = "/" + dds_topic.substr(3, dds_topic.size() - 3 - 7);
      for (const auto& client : agg.writers) {
        for (const auto& server : agg.readers) {
          edges.emplace(client, server, service, "service");
        }
      }
    } else if (dds_topic.rfind("rt/", 0) == 0 && dds_topic.size() > 3) {
      const std::string topic = "/" + dds_topic.substr(3);
      for (const auto& writer : agg.writers) {
        for (const auto& reader : agg.readers) {
          edges.emplace(writer, reader, topic, "topic");
        }
      }
    }
  }

  const auto dump_names = [](const std::set<std::string>& names) {
    std::string list = "[";
    bool inner = true;
    for (const auto& name : names) {
      list += inner ? "" : ",";
      list += json_escape_string(name);
      inner = false;
    }
    return list + "]";
  };

  std::string out = "{";
  out += "\"nodes\":[";
  {
    bool first = true;
    for (const auto& [name, attrib] : nodes) {
      out += first ? "{" : ",{";
      out += "\"name\":" + json_escape_string(name);
      out += ",\"source\":" + json_escape_string(attrib.source);
      out += "}";
      first = false;
    }
  }
  out += "],\"topics\":[";
  {
    bool first = true;
    for (const auto& [dds_topic, agg] : topics) {
      if (dds_topic.rfind("rt/", 0) != 0 || dds_topic.size() < 4 ||
          is_plumbing_topic(dds_topic)) {
        continue;
      }
      out += first ? "{" : ",{";
      out += "\"topic\":" + json_escape_string("/" + dds_topic.substr(3));
      out += ",\"type\":" + json_escape_string(agg.type);
      out += ",\"publishers\":" + dump_names(agg.writers);
      out += ",\"subscriptions\":" + dump_names(agg.readers);
      out += "}";
      first = false;
    }
  }
  out += "],\"services\":[";
  {
    bool first = true;
    for (const auto& [dds_topic, agg] : topics) {
      if (dds_topic.rfind("rq/", 0) != 0 || dds_topic.size() < 11 ||
          dds_topic.compare(dds_topic.size() - 7, 7, "Request") != 0) {
        continue;
      }
      out += first ? "{" : ",{";
      out += "\"service\":" +
             json_escape_string("/" + dds_topic.substr(3, dds_topic.size() - 3 - 7));
      out += ",\"type\":" + json_escape_string(agg.type);
      out += ",\"servers\":" + dump_names(agg.readers);
      out += ",\"clients\":" + dump_names(agg.writers);
      out += "}";
      first = false;
    }
  }
  out += "],\"edges\":[";
  {
    bool first = true;
    for (const auto& [source, target, name, kind] : edges) {
      out += first ? "{" : ",{";
      out += "\"source\":" + json_escape_string(source);
      out += ",\"target\":" + json_escape_string(target);
      out += ",\"topic\":" + json_escape_string(name);
      out += ",\"kind\":" + json_escape_string(kind);
      out += "}";
      first = false;
    }
  }
  out += "],\"endpoints\":[";
  {
    bool first = true;
    for (const auto& [guid, entry] : live) {
      const Impl::EndpointInfo& info = entry.first;
      out += first ? "{" : ",{";
      out += "\"gid\":\"" + guid_hex(guid) + "\"";
      out += ",\"participant\":\"" + guid_hex(info.participant) + "\"";
      out += ",\"node\":" + json_escape_string(entry.second.first);
      out += ",\"topic\":" + json_escape_string(info.topic);
      out += ",\"type\":" + json_escape_string(info.type);
      out +=
          ",\"kind\":" + json_escape_string(info.writer ? "publisher" : "subscription");
      out += "}";
      first = false;
    }
  }
  out += "],\"logs\":[";
  {
    bool first = true;
    for (const auto& log : impl.logs) {
      out += first ? "{" : ",{";
      out += "\"stamp\":" +
             std::to_string(static_cast<int64_t>(log.stamp.sec) * 1000000000 +
                            static_cast<int64_t>(log.stamp.nanosec));
      out += ",\"level\":" + std::to_string(static_cast<int>(log.level));
      out += ",\"name\":" + json_escape_string(log.name);
      out += ",\"msg\":" + json_escape_string(log.msg);
      out += ",\"file\":" + json_escape_string(log.file);
      out += ",\"function\":" + json_escape_string(log.function);
      out += ",\"line\":" + std::to_string(log.line);
      out += "}";
      first = false;
    }
  }
  out += "]}";
  return out;
}

}  // namespace rcl
