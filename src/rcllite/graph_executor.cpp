// Copyright 2026 rcllite contributors
// Licensed under the Apache License, Version 2.0
#include "rcllite/graph_executor.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "rcllite/dds/entities.hpp"
#include "rcllite/exception.hpp"
#include "rcllite/executor.hpp"
#include "rcllite/json_writer.hpp"
#include "rcllite/logging.hpp"
#include "rcllite/names.hpp"
#include "rcllite/node.hpp"
#include "rcllite/node_params.hpp"
#include "rcllite/node_registry.hpp"
#include "rcllite/qos.hpp"
#include "vila/config/configuration.h"

namespace rcl {
namespace {

using Json = vila::Json;
using vila::InternalError;
using vila::InvalidArguments;

/// "00010000ab" -> {0x00,0x01,0x00,0x00,0xab}
std::vector<uint8_t> payload_from_hex(const std::string& hex, const std::string& what) {
  if (hex.size() % 2 != 0) {
    throw Error(InvalidArguments("{}: odd hex length", what));
  }
  std::vector<uint8_t> bytes;
  bytes.reserve(hex.size() / 2);
  const auto nibble = [](char c) -> int {
    if (c >= '0' && c <= '9') {
      return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
      return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
      return c - 'A' + 10;
    }
    return -1;
  };
  for (size_t i = 0; i < hex.size(); i += 2) {
    const int hi = nibble(hex[i]);
    const int lo = nibble(hex[i + 1]);
    if (hi < 0 || lo < 0) {
      throw Error(InvalidArguments("{}: invalid hex digit", what));
    }
    bytes.push_back(static_cast<uint8_t>(hi * 16 + lo));
  }
  return bytes;
}

/// Bare CDR-LE encapsulation header — the default payload (a valid empty
/// message for field-less types; receivers of typed topics will drop it).
std::vector<uint8_t> default_payload() { return {0x00, 0x01, 0x00, 0x00}; }

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

/// DDS plumbing every node carries; it is never data flow.
bool is_plumbing_topic(const std::string& topic) {
  return topic == "/parameter_events" || topic == "/ros_discovery_info" ||
         topic == "/rosout";
}

/// Sort + dedup an adjacency list.
void sort_unique(std::vector<int>& vec) {
  std::sort(vec.begin(), vec.end());
  vec.erase(std::unique(vec.begin(), vec.end()), vec.end());
}

/// The lifecycle's executor presence: a guard-triggered waitable whose
/// dispatch runs one scheduling pass on the spin thread -- lifecycle
/// events flow through the same reactor as every subscription, no private
/// scheduler thread.
class LifecycleTrigger : public EntityBase {
 public:
  explicit LifecycleTrigger(std::function<void()> pass) : pass_(std::move(pass)) {}

  dds_entity_t condition() const override { return guard_.handle(); }

  void dispatch() override {
    guard_.take();
    pass_();
  }

  /// Wake the executor from any thread.
  void trigger() { guard_.trigger(); }

 private:
  dds::GuardCondition guard_;
  std::function<void()> pass_;
};

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

    /// Write one payload; count successful sends.
    void send() {
      if (writer->write(payload.data(), payload.size()) == dds::WriteStatus::Ok) {
        published.fetch_add(1);
      }
    }
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
    std::string ns;
    std::shared_ptr<Node> node;
    bool is_class = false;  // participates in the finish order
    std::vector<std::shared_ptr<RawPublisher>> publishers;
    std::vector<std::shared_ptr<RawSubscription>> subscriptions;
  };

  explicit Impl(const std::string& json_topology) {
    Json doc;
    try {
      doc = vila::JsonFromDoc(json_topology);
    } catch (const std::exception& e) {
      throw Error(InvalidArguments("topology is not valid JSON: {}", e.what()));
    }
    if (!doc.is_object() || !doc.contains("nodes") || !doc.at("nodes").is_array()) {
      throw Error(InvalidArguments("topology must be an object with a 'nodes' array"));
    }
    if (doc.at("nodes").empty()) {
      throw Error(InvalidArguments("topology has no nodes"));
    }
    for (const Json& spec : doc.at("nodes")) {
      build_node(spec);
    }
    for (size_t i = 0; i < nodes.size(); ++i) {
      if (nodes[i].is_class) {
        class_indices.push_back(i);
      }
    }
    // Sized before the gate can open: a fast source may report its batch
    // before the scheduler starts.
    batch_done.assign(class_indices.size(), 0);
    deinit_state.assign(class_indices.size(), Pending);
    deinit_drain_s = doc.value("deinit_drain", 0.3);
    // Created before the sinks: a node may report its batch during on_init.
    lifecycle_trigger = std::make_shared<LifecycleTrigger>([this] { schedule_pass(); });
    install_lifecycle_sinks();
    run_on_init();
    start_publishing();  // skeleton one-shots precede the first streamed frame
    set_gate(GateActive);
    build_lifecycle_graph();
    // The lifecycle advances on the executor's spin (a guard-triggered
    // waitable); this constructor-end pass covers the zero-class edge.
    executor.add_waitable(lifecycle_trigger);
    schedule_pass();
  }

  ~Impl() {
    running.store(false, std::memory_order_relaxed);
    executor.cancel();
    for (auto& thread : deinit_threads) {
      if (thread.joinable()) {
        thread.join();
      }
    }
    for (auto& thread : pub_threads) {
      if (thread.joinable()) {
        thread.join();
      }
    }
  }

  void build_node(const Json& spec) {
    if (!spec.is_object() || !spec.contains("name")) {
      throw Error(InvalidArguments("every node needs a 'name'"));
    }
    NodeEntry entry;
    entry.ns = spec.value("namespace", "/");
    const std::string name = spec.at("name").get<std::string>();
    // "class" opts into a registered behavior-carrying subclass; without it
    // the node is a plain skeleton driven purely by the config below.
    if (spec.contains("class")) {
      const std::string cls = spec.at("class").get<std::string>();
      entry.node = NodeRegistry::CreateByName(cls, name, entry.ns);
      if (!entry.node) {
        throw Error(InvalidArguments(
            "node {}/{}: class '{}' is not registered; link its library "
            "(alwayslink = 1) into the hosting binary",
            entry.ns, name, cls));
      }
      entry.is_class = true;
    } else {
      entry.node = std::make_shared<Node>(name, entry.ns);
    }
    entry.fq_name = entry.node->get_fully_qualified_name();
    executor.add_node(entry.node);

    if (spec.contains("parameters")) {
      const Json& params = spec.at("parameters");
      if (!params.is_object()) {
        throw Error(
            InvalidArguments("node {}: 'parameters' must be an object", entry.fq_name));
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
      throw Error(InvalidArguments(
          "node {}: parameter '{}' must be bool/int/double/string", fq, name));
    }
  }

  void build_publisher(NodeEntry& entry, const Json& spec) {
    const std::string what = "node " + entry.fq_name + " publisher";
    if (!spec.is_object() || !spec.contains("topic") || !spec.contains("type")) {
      throw Error(InvalidArguments("{}: needs 'topic' and 'type'", what));
    }
    auto pub = std::make_shared<RawPublisher>();
    pub->type = dds_type_name(spec.at("type").get<std::string>());
    const std::string ros_topic =
        expand_topic_name(spec.at("topic").get<std::string>(), entry.ns);
    pub->topic = ros_topic;
    pub->payload =
        spec.contains("payload_hex")
            ? payload_from_hex(spec.at("payload_hex").get<std::string>(), what)
            : default_payload();
    pub->rate_hz = spec.value("rate_hz", 0.0);
    QoS qos = parse_qos(spec.value("qos", Json::object()), QoS::Default());
    if (spec.contains("wait")) {
      const Json& wait = spec.at("wait");
      if (wait.is_string() && wait.get<std::string>() == "forever") {
        qos.wait_forever();
      } else if (wait.is_number()) {
        const std::chrono::duration<double> secs{wait.get<double>()};
        qos.wait_for(std::chrono::duration_cast<std::chrono::nanoseconds>(secs));
      } else {
        throw Error(InvalidArguments("{}: 'wait' must be 'forever' or seconds", what));
      }
    }
    pub->writer = std::make_unique<dds::Writer>(
        entry.node->participant().handle(), entry.node->participant().publisher(),
        ros_to_dds_topic_name(ros_topic), pub->type, qos);
    entry.publishers.push_back(std::move(pub));
  }

  void build_subscription(NodeEntry& entry, const Json& spec) {
    const std::string what = "node " + entry.fq_name + " subscription";
    if (!spec.is_object() || !spec.contains("topic") || !spec.contains("type")) {
      throw Error(InvalidArguments("{}: needs 'topic' and 'type'", what));
    }
    auto sub = std::make_shared<RawSubscription>();
    sub->type = dds_type_name(spec.at("type").get<std::string>());
    const std::string ros_topic =
        expand_topic_name(spec.at("topic").get<std::string>(), entry.ns);
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
        if (pub->rate_hz <= 0.0) {
          pub->send();
          continue;
        }
        pub_threads.emplace_back([this, pub] {
          const std::chrono::duration<double> period{1.0 / pub->rate_hz};
          auto deadline = std::chrono::steady_clock::now();
          while (running.load(std::memory_order_relaxed)) {
            pub->send();
            deadline +=
                std::chrono::duration_cast<std::chrono::steady_clock::duration>(period);
            std::this_thread::sleep_until(deadline);
          }
        });
      }
    }
  }

  // --- lifecycle --------------------------------------------------------------
  // All lifecycle state is indexed by CLASS index (position in class_indices);
  // only run_deinit() maps back to the entry in `nodes`.

  enum GateState : uint8_t { GatePending, GateActive, GateBroken };
  enum DeinitState : uint8_t { Pending, Deiniting, Done };

  /// Route every class node's lifecycle events here (before on_init).
  void install_lifecycle_sinks() {
    for (size_t ci = 0; ci < class_indices.size(); ++ci) {
      Node::LifecycleSink sink;
      sink.batch_finished = [this, ci](Node&) { report_batch_finished(ci); };
      sink.shutdown_requested = [this] {
        std::function<void()> callback;
        {
          std::lock_guard<std::mutex> lock(lifecycle_mtx);
          callback = on_shutdown_callback;
        }
        if (callback) {
          callback();
        }
      };
      sink.wait_graph_active = [this] { return wait_gate(); };
      nodes[class_indices[ci]].node->set_lifecycle_sink(std::move(sink));
    }
  }

  /// on_init for every node, in topology order; a throw unwinds the graph:
  /// parked source workers must be released (broken gate) so their nodes'
  /// destructors can join them while the error still surfaces.
  void run_on_init() {
    for (NodeEntry& entry : nodes) {
      const std::string& fq = entry.fq_name;
      try {
        entry.node->on_init();
      } catch (const std::exception& e) {
        set_gate(GateBroken);
        executor.cancel();
        throw Error(InternalError("{}: on_init threw: {}", fq, e.what()));
      } catch (...) {
        set_gate(GateBroken);
        executor.cancel();
        throw Error(InternalError("{}: on_init threw unknown exception", fq));
      }
    }
  }

  void set_gate(GateState state) {
    {
      std::lock_guard<std::mutex> lock(gate_mtx);
      gate_state = state;
    }
    gate_cv.notify_all();
  }

  bool wait_gate() {
    std::unique_lock<std::mutex> lock(gate_mtx);
    gate_cv.wait(lock, [this] { return gate_state != GatePending; });
    return gate_state == GateActive;
  }

  /// The finish-order adjacency over the class nodes: a feeds b when b
  /// subscribes a topic a publishes (endpoint declarations, no discovery).
  void build_lifecycle_graph() {
    const size_t count = class_indices.size();
    feeders.assign(count, {});
    std::unordered_map<std::string, std::vector<int>> publishers_of;
    for (size_t a = 0; a < count; ++a) {
      for (const auto& topic : nodes[class_indices[a]].node->pub_topics()) {
        if (!is_plumbing_topic(topic)) {
          publishers_of[topic].push_back(static_cast<int>(a));
        }
      }
    }
    for (size_t b = 0; b < count; ++b) {
      for (const auto& topic : nodes[class_indices[b]].node->sub_topics()) {
        const auto it = publishers_of.find(topic);
        if (is_plumbing_topic(topic) || it == publishers_of.end()) {
          continue;
        }
        for (int a : it->second) {
          if (a != static_cast<int>(b)) {
            feeders[b].push_back(a);
          }
        }
      }
      sort_unique(feeders[b]);
    }
    build_sccs();
    for (size_t b = 0; b < count; ++b) {
      std::string joined;
      for (int a : feeders[b]) {
        const Node& feeder = *nodes[class_indices[a]].node;
        joined += (joined.empty() ? "" : ", ") + feeder.get_name();
      }
      RCLLITE_LOGI("graph_executor: finish order: {} deinits after [{}]",
                   nodes[class_indices[b]].node->get_name(), joined);
    }
  }

  /// Tarjan's SCC pass: mutual-pacing cycles (a lockstep source pair) would
  /// deadlock a plain topological walk, so cycle members collapse into one
  /// unit -- each reports batch-done independently, then they deinit
  /// together, and the condensation still orders everything else.
  void build_sccs() {
    const size_t count = feeders.size();
    scc_of.assign(count, -1);
    std::vector<int> low(count), disc(count, -1);
    std::vector<int> stack;
    std::vector<bool> on_stack(count, false);
    int timer = 0;
    // Generic self-recursive lambda: no std::function type erasure per call.
    auto visit = [&](auto&& self, int v) -> void {
      disc[v] = low[v] = timer++;
      stack.push_back(v);
      on_stack[v] = true;
      for (int w : feeders[v]) {
        if (disc[w] < 0) {
          self(self, w);
          low[v] = std::min(low[v], low[w]);
        } else if (on_stack[w]) {
          low[v] = std::min(low[v], disc[w]);
        }
      }
      if (low[v] == disc[v]) {
        std::vector<int> members;
        for (;;) {
          const int w = stack.back();
          stack.pop_back();
          on_stack[w] = false;
          scc_of[w] = static_cast<int>(scc_members.size());
          members.push_back(w);
          if (w == v) {
            break;
          }
        }
        scc_members.push_back(std::move(members));
      }
    };
    for (size_t v = 0; v < count; ++v) {
      if (disc[v] < 0) {
        visit(visit, static_cast<int>(v));
      }
    }
    // Condensation feeders (deduped, no self edges).
    scc_feeders.assign(scc_members.size(), {});
    for (size_t b = 0; b < count; ++b) {
      for (int a : feeders[b]) {
        if (scc_of[a] != scc_of[b]) {
          scc_feeders[scc_of[b]].push_back(scc_of[a]);
        }
      }
    }
    for (auto& feeds : scc_feeders) {
      sort_unique(feeds);
    }
    for (const auto& members : scc_members) {
      if (members.size() < 2) {
        continue;
      }
      std::string joined;
      for (int m : members) {
        joined += (joined.empty() ? "" : ", ") + nodes[class_indices[m]].fq_name;
      }
      RCLLITE_LOGW(
          "graph_executor: cyclic wiring collapsed into one finish "
          "unit: [{}] (members deinit together)",
          joined);
    }
  }

  void report_batch_finished(size_t class_index) {
    {
      std::lock_guard<std::mutex> lock(lifecycle_mtx);
      if (batch_done[class_index]) {
        return;  // repeats never re-schedule
      }
      batch_done[class_index] = true;
    }
    lifecycle_trigger->trigger();
  }

  /// Every member of the unit reported its batch (lifecycle_mtx held).
  bool unit_reported(int unit) const {
    const auto& m = scc_members[unit];
    return std::all_of(m.begin(), m.end(), [this](int i) { return batch_done[i]; });
  }

  /// Every feeder unit fully deinitialized (lifecycle_mtx held).  A feeder
  /// unit counts as done only when EVERY member deinitialized -- an SCC unit
  /// finalizes together; any-member would let a fast sibling release
  /// consumers mid-finalize.
  bool feeders_deinited(int unit) const {
    const auto deinited = [this](int m) { return deinit_state[m] == Done; };
    for (int f : scc_feeders[unit]) {
      const auto& members = scc_members[f];
      if (!std::all_of(members.begin(), members.end(), deinited)) {
        return false;
      }
    }
    return true;
  }

  /// Start every eligible deinit on its own thread (lifecycle_mtx held).
  void schedule_locked() {
    for (size_t ci = 0; ci < class_indices.size(); ++ci) {
      if (deinit_state[ci] != Pending) {
        continue;
      }
      const int unit = scc_of[ci];
      if (!unit_reported(unit) || !feeders_deinited(unit)) {
        continue;
      }
      deinit_state[ci] = Deiniting;
      deinit_threads.emplace_back([this, ci] { run_deinit(ci); });
    }
  }

  void run_deinit(size_t class_index) {
    if (deinit_drain_s > 0.0) {
      std::this_thread::sleep_for(std::chrono::duration<double>(deinit_drain_s));
    }
    NodeEntry& entry = nodes[class_indices[class_index]];
    RCLLITE_LOGI("graph_executor: {}: deinit", entry.fq_name);
    try {
      entry.node->on_deinit();
    } catch (const std::exception& e) {
      RCLLITE_LOGE("graph_executor: {}: on_deinit threw ({}) -- continuing",
                   entry.fq_name, e.what());
    } catch (...) {
      RCLLITE_LOGE(
          "graph_executor: {}: on_deinit threw an unknown exception "
          "-- continuing",
          entry.fq_name);
    }
    {
      std::lock_guard<std::mutex> lock(lifecycle_mtx);
      deinit_state[class_index] = Done;
    }
    lifecycle_trigger->trigger();  // a consumer may be eligible now
  }

  bool all_done_locked() const {
    const auto& states = deinit_state;
    return std::all_of(states.begin(), states.end(), [](auto s) { return s == Done; });
  }

  /// One scheduling pass: start every eligible deinit on its own thread,
  /// then fire the graph-finished callback once every class node is done.
  /// Runs on the executor's spin thread (the LifecycleTrigger dispatch) --
  /// no private scheduler thread anywhere.
  void schedule_pass() {
    std::function<void()> finished_callback;
    {
      std::lock_guard<std::mutex> lock(lifecycle_mtx);
      schedule_locked();
      if (all_done_locked() && !finished_fired) {
        finished_fired = true;
        finished_callback = on_finished_callback;
      }
    }
    if (finished_callback) {
      finished_callback();
    }
  }

  // Declared first so it is destroyed last: the executor holds shared node
  // references that must drop after the endpoint owners are gone.
  Executor executor;
  std::vector<NodeEntry> nodes;
  std::atomic<bool> running{true};
  std::vector<std::thread> pub_threads;

  // Lifecycle state (all under lifecycle_mtx).  Declared after `nodes` so
  // the joins in ~Impl run before the nodes destruct: deinit threads touch
  // node state and must never outlive it.
  std::mutex gate_mtx;              ///< Guards gate_state.
  std::condition_variable gate_cv;  ///< Wakes parked source workers.
  GateState gate_state = GatePending;

  mutable std::mutex lifecycle_mtx;
  std::vector<size_t> class_indices;          ///< Class idx -> entry idx; immutable.
  std::vector<std::vector<int>> feeders;      ///< Class idx -> feeder idxs.
  std::vector<int> scc_of;                    ///< Class idx -> SCC unit.
  std::vector<std::vector<int>> scc_members;  ///< Unit -> class idxs.
  std::vector<std::vector<int>> scc_feeders;  ///< Unit -> feeder units.
  std::vector<char> batch_done;               ///< Class idx -> reported.
  std::vector<DeinitState> deinit_state;      ///< Class idx -> state.
  double deinit_drain_s = 0.3;
  bool finished_fired = false;
  std::function<void()> on_finished_callback;
  std::function<void()> on_shutdown_callback;
  std::shared_ptr<LifecycleTrigger> lifecycle_trigger;  ///< The executor-side hook.
  std::vector<std::thread> deinit_threads;  ///< Joined in ~Impl before the nodes die.
};

GraphExecutor::GraphExecutor(const std::string& json_topology)
    : impl_(std::make_unique<Impl>(json_topology)) {}

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

void GraphExecutor::set_on_finished(std::function<void()> callback) {
  std::lock_guard<std::mutex> lock(impl_->lifecycle_mtx);
  impl_->on_finished_callback = std::move(callback);
}

void GraphExecutor::set_on_shutdown(std::function<void()> callback) {
  std::lock_guard<std::mutex> lock(impl_->lifecycle_mtx);
  impl_->on_shutdown_callback = std::move(callback);
}

bool GraphExecutor::finished() const {
  std::lock_guard<std::mutex> lock(impl_->lifecycle_mtx);
  return impl_->all_done_locked();
}

std::vector<std::shared_ptr<Node>> GraphExecutor::nodes() const {
  std::vector<std::shared_ptr<Node>> out;
  out.reserve(impl_->nodes.size());
  for (const auto& entry : impl_->nodes) {
    out.push_back(entry.node);
  }
  return out;
}

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
      out += '}';
      first = false;
    }
    out += "],\"subscriptions\":[";
    first = true;
    for (const auto& sub : entry.subscriptions) {
      out += first ? "{" : ",{";
      out += "\"topic\":" + json_escape_string(sub->topic);
      out += ",\"type\":" + json_escape_string(sub->type);
      out += ",\"received\":" + std::to_string(sub->received.load());
      out += '}';
      first = false;
    }
    out += "]}";
    first_node = false;
  }
  out += "]}";
  return out;
}

}  // namespace rcl
