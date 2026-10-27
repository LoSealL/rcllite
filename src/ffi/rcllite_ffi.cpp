// Copyright 2026 rcllite contributors
// Licensed under the Apache License, Version 2.0
//
// Python bindings for node initialization and execution, built on TVM FFI
// (the apache-tvm-ffi mechanism vila uses).  Compile as a shared library
// (linkshared = True) and load it from Python:
//
//   import tvm_ffi
//   mod = tvm_ffi.load_module("bazel-bin/src/ffi/rcllite_ffi.dll")
//   node = mod.node_create("talker", "/")
//   mod.spin_once(node, 100)
//
// Nodes are addressed by int64 handles into a process-wide registry; each
// node owns a dedicated Executor so Node.spin()/cancel() map 1:1 onto the
// rcl::Executor API.  The registry keeps the C++ objects alive while
// Python holds the handle; node_destroy() cancels a concurrent spin() before
// releasing the entry.
//
// Pub/sub endpoints are type-agnostic CDR channels: create_publisher /
// create_subscription take the ROS type name ("std_msgs/msg/String") and raw
// CDR bytes (encapsulation header included); the typed codecs live on the
// Python side.  Subscription callbacks (void(Bytes)) run on the spin thread
// of the owning node — TVM FFI releases the GIL around the spin call and
// re-acquires it for the callback.
#include <tvm/ffi/extra/stl.h>  // std::vector typed-arg support (peers list)
#include <tvm/ffi/tvm_ffi.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "rcllite/dds/entities.hpp"
#include "rcllite/graph.hpp"
#include "rcllite/graph_executor.hpp"
#include "rcllite/logging.hpp"
#include "rcllite/names.hpp"
#include "rcllite/rcllite.hpp"

namespace {

struct NodeEntry;

// A raw publisher: opaque CDR writer on the node's participant.
struct PubEntry {
  std::string topic;
  std::string type;
  std::unique_ptr<rcl::dds::Writer> writer;
};

// A raw subscription: forwards each sample's CDR bytes to a Python callback
// (void(Bytes)) on the executor thread.  `closed` stops dispatch before the
// reader goes away; the entity object itself survives until the last holder
// (executor snapshot or registry) drops it.
struct SubEntry : public rcl::EntityBase {
  std::string topic;
  std::string type;
  std::unique_ptr<rcl::dds::Reader> reader;
  tvm::ffi::Function callback;
  std::atomic<bool> closed{false};

  dds_entity_t condition() const override { return reader->read_condition(); }

  void dispatch() override {
    if (closed.load()) {
      return;
    }
    reader->take(
        [this](const uint8_t* payload, size_t size, const dds_sample_info_t& info) {
          if (!info.valid_data || closed.load()) {
            return;
          }
          try {
            callback(tvm::ffi::Bytes(reinterpret_cast<const char*>(payload), size));
          } catch (const std::exception& e) {
            // A raising Python callback must not kill the spin loop nor leak the
            // remaining batch (the serdata ref is dropped by Reader::take).
            RCLLITE_LOGW("python subscription callback failed: {}", e.what());
          }
        });
  }
};

struct NodeEntry {
  std::shared_ptr<rcl::Node> node;
  std::unique_ptr<rcl::Executor> executor;
  std::unordered_map<int64_t, std::shared_ptr<PubEntry>> pubs;
  std::unordered_map<int64_t, std::shared_ptr<SubEntry>> subs;
};

std::mutex g_registry_mtx;
std::unordered_map<int64_t, std::shared_ptr<NodeEntry>> g_registry;
int64_t g_next_handle = 1;  // never reused; 0 stays an invalid handle

std::shared_ptr<NodeEntry> lookup(int64_t handle) {
  std::lock_guard<std::mutex> lock(g_registry_mtx);
  const auto it = g_registry.find(handle);
  if (it == g_registry.end()) {
    TVM_FFI_THROW(ValueError) << "unknown node handle: " << handle;
  }
  return it->second;
}

int64_t NodeCreate_(const std::string& name, const std::string& namespace_) {
  auto entry = std::make_shared<NodeEntry>();
  entry->node = std::make_shared<rcl::Node>(name, namespace_);
  entry->executor = std::make_unique<rcl::Executor>();
  entry->executor->add_node(entry->node);

  std::lock_guard<std::mutex> lock(g_registry_mtx);
  const int64_t handle = g_next_handle++;
  g_registry.emplace(handle, std::move(entry));
  return handle;
}

void NodeDestroy_(int64_t handle) {
  std::shared_ptr<NodeEntry> entry;
  {
    std::lock_guard<std::mutex> lock(g_registry_mtx);
    const auto it = g_registry.find(handle);
    if (it == g_registry.end()) {
      return;  // idempotent destroy
    }
    entry = it->second;
    g_registry.erase(it);
  }
  // Unblocks a spin() running on another thread before the entry (and with
  // it the last reference outside that spin) goes away.
  entry->executor->cancel();
}

std::string NodeName_(int64_t handle) { return lookup(handle)->node->get_name(); }

std::string NodeNamespace_(int64_t handle) {
  return lookup(handle)->node->get_namespace();
}

std::string NodeFqName_(int64_t handle) {
  return lookup(handle)->node->get_fully_qualified_name();
}

void NodeSpin_(int64_t handle) { lookup(handle)->executor->spin(); }

bool NodeSpinOnce_(int64_t handle, int64_t timeout_ms) {
  return lookup(handle)->executor->spin_once(std::chrono::milliseconds(timeout_ms));
}

void NodeCancel_(int64_t handle) { lookup(handle)->executor->cancel(); }

// --- pub/sub ----------------------------------------------------------------

/// FFI-domain QoS: depth + reliable (default) + transient_local latch.
rcl::QoS qos_from_args(int64_t depth, bool reliable, bool transient_local) {
  rcl::QoS qos;
  qos.keep_last(static_cast<size_t>(depth));
  if (!reliable) {
    qos.set_best_effort();
  }
  if (transient_local) {
    qos.set_transient_local();
  }
  return qos;
}

/// A well-formed CDR payload starts with the 4-byte CDR-LE encapsulation
/// header; reject anything else loudly (opaque writers would happily ship
/// garbage that receivers silently drop).
void check_cdr_payload(const tvm::ffi::Bytes& payload, const std::string& topic) {
  if (payload.size() < 4 || payload.data()[0] != 0x00 || payload.data()[1] != 0x01) {
    TVM_FFI_THROW(ValueError)
        << "payload for '" << topic << "' is not a CDR-LE buffer (expected the "
        << "4-byte encapsulation header 00 01 00 00 produced by the message codecs)";
  }
}

void InitDds_(int64_t tcp_port, const std::vector<std::string>& peers,
              int64_t domain_id, const std::string& interface) {
  if (tcp_port > 0xFFFF) {
    TVM_FFI_THROW(ValueError) << "tcp_port out of range: " << tcp_port;
  }
  if (domain_id > 232) {
    TVM_FFI_THROW(ValueError) << "domain_id out of range (0-232): " << domain_id;
  }
  std::string xml;
  if (tcp_port > 0) {
    // Dial exactly the given peers; never list our own port (runbook rule:
    // the peers list must not include ports that are not running).
    const std::string own = "127.0.0.1:" + std::to_string(tcp_port);
    std::vector<std::string> dial;
    for (const auto& peer : peers) {
      if (peer != own) {
        dial.push_back(peer);
      }
    }
    xml = rcl::dds::tcp_config_xml(static_cast<uint16_t>(tcp_port), dial, interface);
  }
  const std::optional<uint32_t> domain =
      domain_id >= 0 ? std::optional<uint32_t>(static_cast<uint32_t>(domain_id))
                     : std::nullopt;
  rcl::dds::Context::configure(domain, xml);
}

int64_t PubCreate_(int64_t node_handle, const std::string& topic,
                   const std::string& type_name, int64_t depth, bool reliable,
                   bool transient_local) {
  const std::string dds_type = rcl::dds_type_name(type_name);
  const std::lock_guard<std::mutex> lock(g_registry_mtx);
  const auto it = g_registry.find(node_handle);
  if (it == g_registry.end()) {
    TVM_FFI_THROW(ValueError) << "unknown node handle: " << node_handle;
  }
  NodeEntry& entry = *it->second;
  auto pub = std::make_shared<PubEntry>();
  pub->topic = topic;
  pub->type = dds_type;
  pub->writer = std::make_unique<rcl::dds::Writer>(
      entry.node->participant().handle(), entry.node->participant().publisher(),
      rcl::ros_to_dds_topic_name(topic), dds_type,
      qos_from_args(depth, reliable, transient_local));
  const int64_t handle = g_next_handle++;
  entry.pubs.emplace(handle, std::move(pub));
  return handle;
}

void PubPublish_(int64_t handle, const tvm::ffi::Bytes& payload) {
  std::shared_ptr<PubEntry> pub;
  {
    // Held across the write so a concurrent pub_destroy of the same handle
    // cannot rip the writer out from under dds_writecdr.
    const std::lock_guard<std::mutex> lock(g_registry_mtx);
    for (const auto& [_, entry] : g_registry) {
      const auto it = entry->pubs.find(handle);
      if (it != entry->pubs.end()) {
        pub = it->second;
        break;
      }
    }
    if (!pub) {
      TVM_FFI_THROW(ValueError) << "unknown publisher handle: " << handle;
    }
    check_cdr_payload(payload, pub->topic);
    if (!pub->writer->write(reinterpret_cast<const uint8_t*>(payload.data()),
                            payload.size())) {
      TVM_FFI_THROW(ValueError) << "publish failed on '" << pub->topic << "'";
    }
  }
}

void PubDestroy_(int64_t handle) {
  const std::lock_guard<std::mutex> lock(g_registry_mtx);
  for (const auto& [_, entry] : g_registry) {
    if (entry->pubs.erase(handle) > 0) {
      return;
    }
  }
}

int64_t SubCreate_(int64_t node_handle, const std::string& topic,
                   const std::string& type_name, int64_t depth, bool reliable,
                   bool transient_local,
                   tvm::ffi::TypedFunction<void(tvm::ffi::Bytes)> callback) {
  const std::string dds_type = rcl::dds_type_name(type_name);
  const std::lock_guard<std::mutex> lock(g_registry_mtx);
  const auto it = g_registry.find(node_handle);
  if (it == g_registry.end()) {
    TVM_FFI_THROW(ValueError) << "unknown node handle: " << node_handle;
  }
  NodeEntry& entry = *it->second;
  auto sub = std::make_shared<SubEntry>();
  sub->topic = topic;
  sub->type = dds_type;
  sub->callback = callback.packed();
  sub->reader = std::make_unique<rcl::dds::Reader>(
      entry.node->participant().handle(), entry.node->participant().subscriber(),
      rcl::ros_to_dds_topic_name(topic), dds_type,
      qos_from_args(depth, reliable, transient_local));
  entry.node->add_entity(sub);
  const int64_t handle = g_next_handle++;
  entry.subs.emplace(handle, std::move(sub));
  return handle;
}

void SubDestroy_(int64_t handle) {
  std::shared_ptr<SubEntry> sub;
  std::shared_ptr<rcl::Node> node;
  {
    const std::lock_guard<std::mutex> lock(g_registry_mtx);
    for (const auto& [_, entry] : g_registry) {
      const auto it = entry->subs.find(handle);
      if (it != entry->subs.end()) {
        sub = it->second;
        node = entry->node;
        entry->subs.erase(it);
        break;
      }
    }
  }
  if (sub) {
    // Stop callbacks first; the entity (and its reader) live on until the
    // executor's last snapshot drops it, so a concurrent dispatch stays safe.
    sub->closed.store(true);
    node->remove_entity(sub);
  }
}

std::mutex g_graph_mtx;
std::unique_ptr<rcl::GraphMonitor> g_graph;

void GraphStart_() {
  const std::lock_guard<std::mutex> lock(g_graph_mtx);
  if (!g_graph) {
    g_graph = std::make_unique<rcl::GraphMonitor>();
  }
}

void GraphStop_() {
  const std::lock_guard<std::mutex> lock(g_graph_mtx);
  g_graph.reset();
}

std::string GraphSnapshot_() {
  std::unique_lock<std::mutex> lock(g_graph_mtx);
  if (!g_graph) {
    TVM_FFI_THROW(ValueError) << "graph monitor not started";
  }
  // Release the registry lock while building the document: the monitor is
  // thread-safe internally and graph_start/stop stay responsive.
  rcl::GraphMonitor* monitor = g_graph.get();
  lock.unlock();
  return monitor->snapshot_json();
}

std::mutex g_graph_exec_mtx;
std::unordered_map<int64_t, std::unique_ptr<rcl::GraphExecutor>> g_graph_execs;
int64_t g_next_graph_exec = 1;

int64_t GraphExecCreate_(const std::string& json_topology) {
  // Build first so a config error throws before registering anything.
  auto executor = std::make_unique<rcl::GraphExecutor>(json_topology);
  const std::lock_guard<std::mutex> lock(g_graph_exec_mtx);
  const int64_t handle = g_next_graph_exec++;
  g_graph_execs.emplace(handle, std::move(executor));
  return handle;
}

rcl::GraphExecutor* graph_exec_lookup(int64_t handle) {
  const std::lock_guard<std::mutex> lock(g_graph_exec_mtx);
  const auto it = g_graph_execs.find(handle);
  if (it == g_graph_execs.end()) {
    TVM_FFI_THROW(ValueError) << "unknown graph executor handle: " << handle;
  }
  return it->second.get();
}

void GraphExecSpin_(int64_t handle) { graph_exec_lookup(handle)->spin(); }

bool GraphExecSpinOnce_(int64_t handle, int64_t timeout_ms) {
  return graph_exec_lookup(handle)->spin_once(std::chrono::milliseconds(timeout_ms));
}

void GraphExecCancel_(int64_t handle) { graph_exec_lookup(handle)->cancel(); }

std::string GraphExecStatus_(int64_t handle) {
  return graph_exec_lookup(handle)->status_json();
}

std::string GraphExecRegisteredNodes_() {
  return rcl::GraphExecutor::registered_nodes_json();
}

void GraphExecDestroy_(int64_t handle) {
  std::unique_ptr<rcl::GraphExecutor> executor;
  {
    const std::lock_guard<std::mutex> lock(g_graph_exec_mtx);
    const auto it = g_graph_execs.find(handle);
    if (it == g_graph_execs.end()) {
      return;  // idempotent destroy
    }
    executor = std::move(it->second);
    g_graph_execs.erase(it);
  }
  // Destroy outside the registry lock: teardown joins publisher threads.
  executor.reset();
}

}  // namespace

TVM_FFI_DLL_EXPORT_TYPED_FUNC(node_create, NodeCreate_)
TVM_FFI_DLL_EXPORT_TYPED_FUNC(node_destroy, NodeDestroy_)
TVM_FFI_DLL_EXPORT_TYPED_FUNC(node_name, NodeName_)
TVM_FFI_DLL_EXPORT_TYPED_FUNC(node_namespace, NodeNamespace_)
TVM_FFI_DLL_EXPORT_TYPED_FUNC(node_fully_qualified_name, NodeFqName_)
TVM_FFI_DLL_EXPORT_TYPED_FUNC(node_spin, NodeSpin_)
TVM_FFI_DLL_EXPORT_TYPED_FUNC(node_spin_once, NodeSpinOnce_)
TVM_FFI_DLL_EXPORT_TYPED_FUNC(node_cancel, NodeCancel_)
TVM_FFI_DLL_EXPORT_TYPED_FUNC(init_dds, InitDds_)
TVM_FFI_DLL_EXPORT_TYPED_FUNC(pub_create, PubCreate_)
TVM_FFI_DLL_EXPORT_TYPED_FUNC(pub_publish, PubPublish_)
TVM_FFI_DLL_EXPORT_TYPED_FUNC(pub_destroy, PubDestroy_)
TVM_FFI_DLL_EXPORT_TYPED_FUNC(sub_create, SubCreate_)
TVM_FFI_DLL_EXPORT_TYPED_FUNC(sub_destroy, SubDestroy_)
TVM_FFI_DLL_EXPORT_TYPED_FUNC(graph_start, GraphStart_)
TVM_FFI_DLL_EXPORT_TYPED_FUNC(graph_stop, GraphStop_)
TVM_FFI_DLL_EXPORT_TYPED_FUNC(graph_snapshot, GraphSnapshot_)
TVM_FFI_DLL_EXPORT_TYPED_FUNC(graph_exec_create, GraphExecCreate_)
TVM_FFI_DLL_EXPORT_TYPED_FUNC(graph_exec_spin, GraphExecSpin_)
TVM_FFI_DLL_EXPORT_TYPED_FUNC(graph_exec_spin_once, GraphExecSpinOnce_)
TVM_FFI_DLL_EXPORT_TYPED_FUNC(graph_exec_cancel, GraphExecCancel_)
TVM_FFI_DLL_EXPORT_TYPED_FUNC(graph_exec_status, GraphExecStatus_)
TVM_FFI_DLL_EXPORT_TYPED_FUNC(graph_exec_registered_nodes, GraphExecRegisteredNodes_)
TVM_FFI_DLL_EXPORT_TYPED_FUNC(graph_exec_destroy, GraphExecDestroy_)
