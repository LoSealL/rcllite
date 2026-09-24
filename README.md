# rcllite

English | [中文](README_CN.md)

A lightweight, cross-platform (Windows / Linux) ROS 2 compatible client library.
Ported after the layered architecture of
[ros2/rcl](https://github.com/ros2/rcl), [ros2/rmw](https://github.com/ros2/rmw)
and [ros2/rclcpp](https://github.com/ros2/rclcpp), keeping only the
communication core, and interoperating with standard ROS 2 nodes at the DDS
wire-protocol level.

```
+-----------------------------------------------------------+
|  User code (rclcpp-style API, namespace rcl::)         |
+-----------------------------------------------------------+
|  rcllite core     Node / Publisher / Subscription /        |
|                   Service / Client / Parameter / Executor  |
+-----------------------------------------------------------+
|  DDS abstraction  participant / writer / reader / QoS map  |
|                   + opaque CDR sertype/serdata (raw I/O)   |
+-----------------------------------------------------------+
|  Eclipse CycloneDDS 0.10.x  (pure C, replaceable)          |
+-----------------------------------------------------------+
|  vila (base deps: vila::Logger logging / vila::Status err) |
+-----------------------------------------------------------+
```

## Interoperability with standard ROS 2 nodes

The following matrix has been **verified item by item between ros:humble
(Docker, RMW_IMPLEMENTATION=rmw_cyclonedds_cpp) and a rcllite Linux build**:

| Feature | Requirement | Verified result |
|---|---|---|
| Topic pub/sub | any rmw | ✅ rcllite talker → `ros2 topic echo`; `ros2 topic pub` → rcllite listener |
| Service request/response | `rmw_cyclonedds_cpp` | ✅ `ros2 service call /add_two_ints` → rcllite server (21+21=42); rcllite client → ROS 2 demo server (41+1=42) |
| Parameter | `rmw_cyclonedds_cpp` | ✅ `ros2 param list/get/set` directly on rcllite nodes; remote `set` takes effect locally |
| `ros2 node list` | `rmw_cyclonedds_cpp` | ✅ nodes visible (implements the `ros_discovery_info` / `ParticipantEntitiesInfo` graph announcement protocol) |
| `ros2 topic list` / `service list` | any rmw | ✅ endpoints natively visible |

Notes:
- Topics use standard CDR (XCDR1/CDR-LE) with topic name `rt/<fqn>` and type
  name `pkg::msg::dds_::Name_`, identical across all rmw implementations.
- The service header mirrors rmw_cyclonedds byte for byte (8B client id +
  8B sequence). Service headers are inherently incompatible across ROS 2 rmw
  implementations (a known upstream limitation), hence interoperability is
  targeted at the CycloneDDS rmw.
- Node visibility implements the `rmw_dds_common` `ros_discovery_info`
  protocol (transient local). Note that the `Gid` field size varies by ROS 2
  release: **`char[24]` on humble, `char[16]` on iron/jazzy/rolling**. The
  interface definitions are pinned to the upstream humble branch
  (MODULE.bazel); to target jazzy/rolling, simply switch the
  `rmw_dds_common` pin to the matching branch (Gid is `char[16]`).
- Serialization follows the empirically observed rmw behavior: **sequence
  length prefixes are always 4-byte aligned** (uint32 aligns itself), while
  each element keeps its own natural alignment.
- The `ros2 node info` endpoint list is currently empty (per-entity GIDs are
  not reported).

## Building (Bazel)

Uses [bazelisk](https://github.com/bazelbuild/bazelisk) + Bazel 8.8.0 (version
pinned via `.bazelversion`), with Bzlmod managing dependencies: vila and
googletest come in via BCR / git_override; CycloneDDS 0.10.4 has no upstream
Bazel support, so its sources are pulled with `new_git_repository`
(`use_repo_rule`) and built by the BUILD files under `bazel/cyclonedds/`
(including the 4 cmake-generated public headers in `generated/dds/`).

```bash
bazelisk build //...          # all targets (libs + tests + examples + FFI shared lib)
bazelisk test //tests/...     # unit tests (mock ddsc, no network, all non-manual tests by default)
bazelisk run //examples:rcllite_talker
bazelisk run //examples/graph:abc_graph   # A→B→C pipeline (registry + topology executor)
```

Testing happens at two levels:

- **Unit tests** (run by default): the seven suites
  `client/executor/service/qos/names/cdr/graph_executor` swap the CycloneDDS
  C API at link time via `tests/mock/mock_ddsc` — no sockets, no discovery,
  fully deterministic. They cover scenarios that are hard to reproduce
  reliably on a real network: async callbacks, timeout paths, error injection
  (write failures, malformed samples), dynamic executor attach/detach, QoS
  translation, topology config validation / registry instantiation, etc.
- **End-to-end tests** (`manual` tag, real network, run manually):
  `bazelisk test //tests:comms_test //tests:qos_e2e_test`
  covering full-stack communication (pub/sub, services, parameters) and QoS
  behavior (best-effort delivery, transient-local late-joiner replay) over a
  real RTPS loopback.

On Windows Bazel auto-detects MSVC; Linux/macOS use gcc/clang. The common
per-compiler flags (MSVC `/W4 /utf-8 /bigobj ...`, gcc/clang
`-Wall -Wextra -Wpedantic`) are centralized in `build_defs.bzl`. Code style:
`clang-format`, Google-based with an 88-column limit (`.clang-format`).

## Python bindings (pyrcllite)

A thin wrapper on TVM FFI (same mechanism as vila), covering three
capabilities — node init/spin, live graph sniffing, and topology execution:

```bash
pip install apache-tvm-ffi            # FFI runtime (also needed by the Bazel build)
pip install -e ".[graph,launch]"      # optional: networkx graphs / PyYAML topology
bazelisk build //src/ffi:rcllite_ffi  # binding shared library (bazel-bin/src/ffi/)
```

```python
import pyrcllite

node = pyrcllite.Node("talker")  # visible to ros2 node list as soon as it comes up
node.spin_once(100)  # or node.spin() + node.cancel()

with pyrcllite.GraphMonitor() as m:  # sniff the live graph
    m.save("graph.json")
with pyrcllite.GraphExecutor(
    "topology.yaml"
) as e:  # bring up a whole topology in one process
    e.spin_once(100)
```

- **GraphMonitor** / `python -m pyrcllite`: passively observes all rcllite /
  ROS 2 nodes on the domain (fusion of DDS built-in topics +
  `ros_discovery_info` + `rt/rosout`), producing a directed data-flow graph
  (JSON / networkx GraphML) and node logs.
- **GraphExecutor**: reads a full topology config (a "grand collection" of
  node.yamls) and brings up every node in this process (parameters / pub /
  sub / QoS / publish rate); the `class` field instantiates behavior node
  classes registered via `RCLLITE_REGISTER_NODE` (constructor behavior +
  config assembly layered on top).

See [python/README.md](python/README.md) for details. The C++ glue lives in
`src/ffi/rcllite_ffi.cpp`, the observer in `src/rcllite/graph.hpp`, the
topology executor in `src/rcllite/graph_executor.hpp`, and the node registry
in `src/rcllite/node_registry.hpp`.

## Usage

```cpp
#include "rcllite/rcllite.hpp"
#include "rcllite_types/rcl_interfaces/parameter.hpp"

int main()
{
  auto node = std::make_shared<rcl::Node>("talker");
  auto pub = node->create_publisher<rcl_interfaces::msg::Parameter>("chatter");

  rcl_interfaces::msg::Parameter msg;
  msg.name = "hello";
  pub->publish(msg);

  // When subscribers may not exist yet (volatile QoS samples written while
  // no reader is matched never reach late joiners — transient-local do):
  pub->wait_for_subscribers();                           // block until the first subscriber matches
  bool sent = pub->publish(msg, std::chrono::seconds(1)); // backpressure: false = timed out, nothing written

  node->create_subscription<rcl_interfaces::msg::Parameter>(
    "echo", [](const rcl_interfaces::msg::Parameter & m) { /* ... */ });

  rcl::spin(node);   // or assemble your own Executor
}
```

Services and parameters:

```cpp
auto server = node->create_service<rcl_interfaces::srv::GetParameters>(
  "get_parameters",
  [](const auto & req) {
    rcl_interfaces::srv::GetParameters::Response resp;
    /* fill in resp.values ... */
    return resp;
  });

auto client = node->create_client<rcl_interfaces::srv::GetParameters>("get_parameters");
client->wait_for_service();
auto resp = client->call(req);            // synchronous (requires spinning in another thread)
client->async_send_request(req, on_resp); // asynchronous (callback on the executor thread)

node->declare_parameter("speed", int64_t{42});
node->set_parameter("speed", rcl::param::to_value(int64_t{7}));
long speed = node->get_parameter<int64_t>("speed");
```

`examples/` contains a talker / listener / GetParameters server and client /
parameter demos.

## Message types

- **Official interfaces are generated at build time**: `builtin_interfaces`,
  `rcl_interfaces` (all msg/srv needed by parameters), `rosgraph_msgs`,
  `rmw_dds_common` (graph announcement). The `.msg`/`.srv` sources are pulled
  straight from upstream by Bazel via `new_git_repository` (pinned to the
  humble branch); the `rcllite_msg_library` rule in `bazel/rules/msg.bzl`
  (analogous to rules_proto) invokes `tools/msg_codegen.py` to generate C++
  headers on the fly — **the repo commits no .msg copies or generated
  headers**. The Python interpreter uses the rules_python hermetic
  toolchain, with no reliance on the host.
- To generate types for other interface packages (to interop with the
  corresponding ROS 2 release):

```python
# msg/BUILD.bazel
load("//bazel/rules:msg.bzl", "rcllite_msg_library")

rcllite_msg_library(
    name="my_pkg",
    package="my_pkg",
    files=["@my_interfaces//:my_pkg/msg/MyType.msg"],
    deps=[":builtin_interfaces"],  # nested type dependencies
)
```

The generated headers provide CDR serialization byte-for-byte identical to
ROS 2 (nested structs, fixed/variable-size arrays, bounded sequences,
default values). `wstring` is not supported yet.

## Threading model

- `Publisher::publish` / `Client::async_send_request` may be called from any
  thread; the blocking variants (`publish(msg, timeout)`,
  `wait_for_subscribers`) additionally stall the calling thread.
- Callbacks (subscriptions, services, parameter services) are dispatched on
  the `Executor::spin` thread; within a single executor callbacks are
  naturally serialized — no locking needed.
- Synchronous `Client::call` requires the executor to spin in another thread.

## QoS subset

`rcl::QoS` supports reliability (reliable/best-effort), durability
(volatile/transient-local), history (keep-last N / keep-all); the presets
match `rmw_qos_profile_default/sensor_data/parameters/parameter_events/services`.
deadline, lifespan and liveliness are not implemented.

## Directory layout

```
src/rcllite/          headers and implementations side by side; includes start at rcllite/
                      (cdr, node, publisher, subscription, service, client,
                       parameter, executor, qos, names, msg_traits,
                       exception, logging; dds/ is the CycloneDDS abstraction layer)
msg/                  Bazel generation targets for official ROS 2 interfaces (rcllite_msg_library)
bazel/rules/msg.bzl   .msg/.srv → C++ build-time generation rule (analogous to rules_proto)
build_defs.bzl        common compile flags (Windows MSVC / gcc/clang)
bazel/cyclonedds/     Bazel build of CycloneDDS (new_git_repository sources + generated headers + ddsc lib)
tools/msg_codegen.py  .msg/.srv → C++ generator (hermetic python build)
tools/debug_raw.cpp   raw DDS topic packet-capture debug tool (hexdump of CDR payloads)
tests/mock/           link-time mock ddsc (network isolation for unit tests)
examples/ tests/      examples and tests (gtest unit=mock; end-to-end=manual tag)
```

## Known limitations

- Services/parameters interoperate only with `rmw_cyclonedds_cpp` nodes (see
  the table above; a cross-rmw limitation of ROS 2 itself).
- The `ros2 node info` endpoint list is empty (the graph announcement does
  not report entity GIDs).
- No actions, lifecycle, timers, user-exposed guards, or intra-process
  communication.
- Messages carry no DDS key; advanced DDS features such as content filtering
  are not exposed.
- Bound to CycloneDDS 0.10.x (the sertype interface changed in 0.11+;
  adaptation is left for later).
- `wstring` types are unsupported (codegen skips them with a warning).

## Docker interop test environment (reproduction)

```bash
docker network create rosnet
docker run -d --name ros2test --network rosnet ros:humble sleep infinity
docker exec ros2test bash -c "apt-get update -qq && apt-get install -y -qq \
  ros-humble-rmw-cyclonedds-cpp ros-humble-demo-nodes-cpp"
# rcllite side (Linux build):
docker run -d --name rclbuild --network rosnet ros:humble sleep infinity
# ... copy the sources in, bazelisk build //..., then run examples/ on both sides
# On the ROS 2 side remember: export RMW_IMPLEMENTATION=rmw_cyclonedds_cpp
```

Note: under Docker Desktop (Windows/WSL2) default NAT, **UDP between the
Windows host and containers does not work** (only container → host is
reachable one-way; vpnkit does not forward unpublished ports), so a native
Windows process and in-container ROS 2 cannot interoperate over DDS
directly; for cross-machine scenarios use a real network or run both sides
inside containers/WSL. All bidirectional interoperability in this repo's
matrix was performed on a docker bridge network.
