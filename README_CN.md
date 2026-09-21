# rcllite

[English](README.md) | 中文

轻量化、跨平台（Windows / Linux）的 ROS 2 兼容客户端库。参照
[ros2/rcl](https://github.com/ros2/rcl)、[ros2/rmw](https://github.com/ros2/rmw)
与 [ros2/rclcpp](https://github.com/ros2/rclcpp) 的分层架构移植，只保留通信核心，
在 DDS 线协议层面与标准 ROS 2 节点互通。

```
+-----------------------------------------------------------+
|  用户代码 (rclcpp 风格 API, namespace rcl::)            |
+-----------------------------------------------------------+
|  rcllite 核心层   Node / Publisher / Subscription /          |
|                  Service / Client / Parameter / Executor    |
+-----------------------------------------------------------+
|  DDS 抽象层       participant / writer / reader / QoS 映射    |
|                  + 不透明 CDR sertype/serdata (raw 读写)     |
+-----------------------------------------------------------+
|  Eclipse CycloneDDS 0.10.x  (纯 C, 可替换)                  |
+-----------------------------------------------------------+
|  vila (基础依赖: vila::Logger 日志 / vila::Status 错误码)     |
+-----------------------------------------------------------+
```

## 与标准 ROS 2 节点的互通性

以下矩阵已在 **ros:humble（Docker，RMW_IMPLEMENTATION=rmw_cyclonedds_cpp）与 rcllite Linux 构建
之间逐项实测通过**：

| 功能 | 互通条件 | 实测结果 |
|---|---|---|
| Topic 发布/订阅 | 任意 rmw | ✅ rcllite talker → `ros2 topic echo`；`ros2 topic pub` → rcllite listener |
| Service 请求/响应 | `rmw_cyclonedds_cpp` | ✅ `ros2 service call /add_two_ints` → rcllite server（21+21=42）；rcllite client → ROS 2 demo server（41+1=42） |
| Parameter | `rmw_cyclonedds_cpp` | ✅ `ros2 param list/get/set` 直接操作 rcllite 节点，远程 set 后本地生效 |
| `ros2 node list` | `rmw_cyclonedds_cpp` | ✅ 节点可见（实现 `ros_discovery_info` / `ParticipantEntitiesInfo` 图通告协议） |
| `ros2 topic list` / `service list` | 任意 rmw | ✅ 端点天然可见 |

说明：
- Topic 走标准 CDR（XCDR1/CDR-LE），topic 名 `rt/<fqn>`、类型名 `pkg::msg::dds_::Name_`，
  对所有 rmw 实现一致。
- Service 逐字节镜像 rmw_cyclonedds 的请求头（8B client id + 8B sequence）。ROS 2 各 rmw 的
  服务头本就互不兼容（官方已知限制），因此选择与 CycloneDDS rmw 互通。
- 节点可见性实现 `rmw_dds_common` 的 `ros_discovery_info` 协议（transient local）。注意
  `Gid` 字段大小随 ROS 2 版本不同：**humble 为 `char[24]`，iron/jazzy/rolling 为 `char[16]`**；
  接口定义 pin 在上游 humble 分支（MODULE.bazel），如需对接 jazzy/rolling，
  把 `rmw_dds_common` 的 pin 换到对应分支即可（Gid 为 `char[16]`）。
- 序列化遵循实测到的 rmw 行为：**序列长度前缀恒按 4 字节对齐**（uint32 自对齐），
  元素各自保持自身对齐。
- `ros2 node info` 的端点列表暂为空（未上报每实体的 GID）。

## 构建（Bazel）

使用 [bazelisk](https://github.com/bazelbuild/bazelisk) + Bazel 8.8.0（`.bazelversion` 固定版本），
Bzlmod 管理依赖：vila、googletest 经 BCR / git_override 引入；CycloneDDS 0.10.4 上游无 Bazel
支持，经 `new_git_repository`（`use_repo_rule`）拉取源码，由 `bazel/cyclonedds/` 下的
BUILD 编译（含 cmake 生成的 4 个公共头 `generated/dds/`）。

```bash
bazelisk build //...          # 全部目标（库 + 测试 + 示例 + FFI 共享库）
bazelisk test //tests/...     # 单元测试（mock ddsc，无网络，默认包含全部非 manual 测试）
bazelisk run //examples:rcllite_talker
bazelisk run //examples/graph:abc_graph   # A→B→C 流水线（registry + 拓扑执行器）
```

测试分两层：

- **单元测试**（默认运行）：`client/executor/service/qos/names/cdr/graph_executor`
  七组测试通过 `tests/mock/mock_ddsc` 在链接级替换 CycloneDDS C API —— 无
  socket、无发现、完全确定性，可覆盖异步回调、超时路径、错误注入（写失败、
  畸形样本）、executor 动态挂载/摘除、QoS 翻译、拓扑配置校验/注册表实例化等
  难以在真实网络上稳定复现的场景。
- **端到端测试**（`manual` 标签，需真实网络，手动运行）：
  `bazelisk test //tests:comms_test //tests:qos_e2e_test`
  覆盖真实 RTPS 回环下的全栈通信（pub/sub、服务、参数）与 QoS 行为
  （best-effort 传输、transient-local 迟到订阅者重放）。

Windows 下 Bazel 自动探测 MSVC；Linux/macOS 使用 gcc/clang。各编译器的公共参数
（MSVC `/W4 /utf-8 /bigobj ...`，gcc/clang `-Wall -Wextra -Wpedantic`）集中在
`build_defs.bzl`。代码风格：`clang-format`，基于 Google、行宽 88（`.clang-format`）。

## Python 绑定（pyrcllite）

基于 TVM FFI（vila 同款机制）的薄封装，覆盖三块能力——节点初始化/执行、
运行图嗅探、拓扑执行：

```bash
pip install apache-tvm-ffi            # FFI 运行时（Bazel 构建也依赖它）
pip install -e ".[graph,launch]"      # 可选：networkx 出图 / PyYAML 拓扑
bazelisk build //src/ffi:rcllite_ffi  # 绑定共享库（bazel-bin/src/ffi/）
```

```python
import pyrcllite

node = pyrcllite.Node("talker")  # 上线即对 ros2 node list 可见
node.spin_once(100)  # 或 node.spin() + node.cancel()

with pyrcllite.GraphMonitor() as m:  # 嗅探运行图
    m.save("graph.json")
with pyrcllite.GraphExecutor("topology.yaml") as e:  # 单进程拉起整个拓扑
    e.spin_once(100)
```

- **GraphMonitor** / `python -m pyrcllite`：被动观察域上全部 rcllite / ROS 2
  节点（DDS 内建 topic + `ros_discovery_info` + `rt/rosout` 三路融合），输出
  有向数据流图（JSON / networkx GraphML）与节点日志。
- **GraphExecutor**：读取完整拓扑配置（node.yaml 大合集）在本进程内拉起全部
  节点（参数 / pub / sub / QoS / 发布节奏）；`class` 字段可实例化经
  `RCLLITE_REGISTER_NODE` 注册的行为节点类（构造行为 + 配置装配叠加）。

详见 [python/README.md](python/README.md);C++ 侧胶水在
`src/ffi/rcllite_ffi.cpp`，观察器在 `src/rcllite/graph.hpp`，拓扑执行器在
`src/rcllite/graph_executor.hpp`，节点注册表在
`src/rcllite/node_registry.hpp`。

## 使用

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

  node->create_subscription<rcl_interfaces::msg::Parameter>(
    "echo", [](const rcl_interfaces::msg::Parameter & m) { /* ... */ });

  rcl::spin(node);   // 或自行组合 Executor
}
```

服务与参数：

```cpp
auto server = node->create_service<rcl_interfaces::srv::GetParameters>(
  "get_parameters",
  [](const auto & req) {
    rcl_interfaces::srv::GetParameters::Response resp;
    /* 填充 resp.values ... */
    return resp;
  });

auto client = node->create_client<rcl_interfaces::srv::GetParameters>("get_parameters");
client->wait_for_service();
auto resp = client->call(req);            // 同步（需另一线程 spin）
client->async_send_request(req, on_resp); // 异步（executor 线程回调）

node->declare_parameter("speed", int64_t{42});
node->set_parameter("speed", rcl::param::to_value(int64_t{7}));
long speed = node->get_parameter<int64_t>("speed");
```

`examples/` 内含 talker / listener / GetParameters 服务端与客户端 / 参数演示。

## 消息类型

- **官方接口在构建时生成**：`builtin_interfaces`、`rcl_interfaces`（参数所需全部 msg/srv）、
  `rosgraph_msgs`、`rmw_dds_common`（图通告）。`.msg`/`.srv` 原文由 Bazel 经
  `new_git_repository` 直接从上游拉取（pin 在 humble 分支），`bazel/rules/msg.bzl` 的
  `rcllite_msg_library` 规则（类比 rules_proto）调用 `tools/msg_codegen.py` 现场生成
  C++ 头 —— **仓库不提交任何 .msg 副本或生成头**，Python 解释器用 rules_python 的
  hermetic 工具链，不依赖宿主机。
- 为其它接口包生成类型（与对应 ROS 2 发行版互通）：

```python
# msg/BUILD.bazel
load("//bazel/rules:msg.bzl", "rcllite_msg_library")

rcllite_msg_library(
    name="my_pkg",
    package="my_pkg",
    files=["@my_interfaces//:my_pkg/msg/MyType.msg"],
    deps=[":builtin_interfaces"],  # 嵌套类型依赖
)
```

生成的头文件提供与 ROS 2 逐字节一致的 CDR 序列化（含嵌套结构体、定长/变长数组、
有界序列、默认值）。`wstring` 暂不支持。

## 线程模型

- `Publisher::publish` / `Client::async_send_request` 可在任意线程调用。
- 回调（订阅、服务、参数服务）统一在 `Executor::spin` 的线程内派发，
  单 executor 内回调天然串行，无需加锁。
- 同步 `Client::call` 需要 executor 在另一线程 spin。

## QoS 子集

`rcl::QoS` 支持 reliability（reliable/best-effort）、durability
（volatile/transient-local）、history（keep-last N / keep-all），预设值与
`rmw_qos_profile_default/sensor_data/parameters/parameter_events/services` 一致。
deadline、lifespan、liveliness 未实现。

## 目录结构

```
src/rcllite/          头文件与实现同目录；include 路径从 rcllite/ 起
                      （cdr, node, publisher, subscription, service, client,
                       parameter, executor, qos, names, msg_traits,
                       exception, logging；dds/ 为 CycloneDDS 抽象层）
msg/                  官方 ROS 2 接口的 Bazel 生成目标（rcllite_msg_library）
bazel/rules/msg.bzl   .msg/.srv → C++ 构建时生成规则（类比 rules_proto）
build_defs.bzl        公共编译参数（Windows MSVC / gcc/clang）
bazel/cyclonedds/     CycloneDDS 的 Bazel 构建（new_git_repository 源码 + 生成头 + ddsc 库）
tools/msg_codegen.py  .msg/.srv → C++ 生成器（hermetic python 构建）
tools/debug_raw.cpp   裸 DDS topic 抓包调试工具（hexdump CDR 载荷）
tests/mock/           链接级 mock ddsc（单元测试隔离网络用）
examples/ tests/      示例与测试（gtest 单元=mock；端到端=manual 标签）
```

## 已知限制

- 服务/参数仅与 `rmw_cyclonedds_cpp` 节点互通（见上表，ROS 2 本身的跨 rmw 限制）。
- `ros2 node info` 端点列表为空（图通告未上报实体 GID）。
- 无 action、lifecycle、timers、guards 对用户暴露、进程内(intra-process)通信。
- 消息不携带 DDS key；内容过滤等 DDS 高级特性未暴露。
- 绑定 CycloneDDS 0.10.x（0.11+ 的 sertype 接口有变化，适配留作后续）。
- `wstring` 类型不支持（codegen 跳过并告警）。

## Docker 互通测试环境（复现）

```bash
docker network create rosnet
docker run -d --name ros2test --network rosnet ros:humble sleep infinity
docker exec ros2test bash -c "apt-get update -qq && apt-get install -y -qq \
  ros-humble-rmw-cyclonedds-cpp ros-humble-demo-nodes-cpp"
# rcllite 侧（Linux 构建）：
docker run -d --name rclbuild --network rosnet ros:humble sleep infinity
# ... 拷入源码后 bazelisk build //...，然后两侧各跑 examples/ 即可
# ROS 2 侧记得：export RMW_IMPLEMENTATION=rmw_cyclonedds_cpp
```

注意：Docker Desktop (Windows/WSL2) 默认 NAT 下 **Windows 宿主 ↔ 容器的 UDP 不通**
（仅容器 → 宿主单向可达，vpnkit 不转发未发布端口），因此 Windows 原生进程与容器内
ROS 2 无法直接 DDS 互通；跨机场景请用真实网络或两侧均在容器/WSL 内。本仓库的双向
互通矩阵均在 docker bridge 网络内完成。
