# AGENTS.md — rcllite

面向 agent 的仓库总览。接口与操作细节在 `.agents/skills/` 下，按需查阅：

| Skill | 用途 |
|---|---|
| `.agents/skills/msg-codegen/` | 新增/编译 ROS 2 消息类型（.msg/.srv → C++ 头） |
| `.agents/skills/write-node/` | 用 C++ 编写新的 rcllite 节点（pub/sub/service/param/executor/行为类注册/拓扑执行） |
| `.agents/skills/python-dev/` | Python 侧开发（pyrcllite 绑定、FFI、图嗅探/拓扑执行、lint） |

## 简短描述

rcllite 是轻量化、跨平台（Windows / Linux）的 **ROS 2 兼容客户端库**：参照
ros2/rcl + rmw + rclcpp 的分层架构移植，只保留通信核心（Node / Publisher /
Subscription / Service / Client / Parameter / Executor），在 DDS 线协议层面与
标准 ROS 2 节点互通。另含三块周边能力：Python 绑定 pyrcllite（TVM FFI）、
运行图嗅探（GraphMonitor）、拓扑执行器（GraphExecutor + 节点类注册表）。

- 语言与标准：C++17；Python 绑定 pyrcllite（>=3.12）
- 构建：Bazel 8.8.0（bazelisk + Bzlmod，`.bazelversion` 固定），Windows 用 MSVC
- 中间件：Eclipse CycloneDDS 0.10.4（纯 C，经 `bazel/cyclonedds/` 自建）
- 基础依赖：vila（`vila::Logger` / `vila::Status` / `vila::Json`(config) /
  `GlobalFactoryRegistry`(widget:registration)）；Python FFI 复用 vila 的
  `tvm_ffi_extension` 发现 pip 包 apache-tvm-ffi 的头与运行时库
- 分层：用户 API（`rcl::`）→ DDS 抽象层（`src/rcllite/dds/`）→ CycloneDDS → vila

## 与 ROS 2 的关系和兼容性

**关系**：不是 rcl/rclcpp 的 fork，而是独立实现的精简客户端库；API 风格类似
rclcpp（`create_publisher` / `create_service` / `spin`），但类型、命名空间、
BUILD 体系均为本仓库自有。互通目标是**线协议兼容**而非源码兼容。

**互通矩阵**（已在 ros:humble Docker + `RMW_IMPLEMENTATION=rmw_cyclonedds_cpp`
与 rcllite Linux 构建之间逐项实测）：

| 功能 | 互通条件 | 结果 |
|---|---|---|
| Topic 发布/订阅 | 任意 rmw | ✅ 与 `ros2 topic pub/echo` 双向 |
| Service 请求/响应 | `rmw_cyclonedds_cpp` | ✅ 与 `ros2 service call` 双向 |
| Parameter | `rmw_cyclonedds_cpp` | ✅ `ros2 param list/get/set` |
| `ros2 node list` | `rmw_cyclonedds_cpp` | ✅ 实现 `ros_discovery_info` 图通告 |
| `ros2 topic/service list` | 任意 rmw | ✅ 端点天然可见 |

关键线协议约定（改动序列化/图通告代码前必读）：

- Topic 走标准 CDR（XCDR1/CDR-LE）；topic 名 `rt/<fqn>`，类型名
  `pkg::msg::dds_::Name_`，对所有 rmw 一致。
- Service 头逐字节镜像 rmw_cyclonedds（8B client id + 8B sequence）。
  ROS 2 各 rmw 服务头本就互不兼容，因此只与 CycloneDDS rmw 互通。
- 序列长度前缀恒按 4 字节对齐（uint32 自对齐），元素各自保持自身对齐。
- 接口定义 pin 在上游 **humble 分支**（MODULE.bazel）。`ros_discovery_info` 的
  `Gid` 字段随版本变化：humble 为 `char[24]`，iron/jazzy/rolling 为 `char[16]`；
  对接其它发行版需把 `rmw_dds_common` 的 pin 换到对应分支。

已知限制：无 action / lifecycle / timers / intra-process；`ros2 node info` 端点
列表为空；消息不带 DDS key；`wstring` 不支持（codegen 跳过并告警）；绑定
CycloneDDS 0.10.x。

## 周边能力（src/rcllite 核心之外）

- **Python 绑定 pyrcllite**（`python/pyrcllite/` + `src/ffi/rcllite_ffi.cpp`）：
  基于 TVM FFI（vila 同款机制）的薄封装；覆盖节点初始化/执行、运行图嗅探、
  拓扑执行。构建：`bazelisk build //src/ffi:rcllite_ffi`，Python 侧定位逻辑见
  `pyrcllite/_ffi.py`（`$RCLLITE_FFI_LIB` → `bazel-bin/src/ffi/` → cwd）。
- **GraphMonitor**（`src/rcllite/graph.hpp`）：被动观察全域节点——DDS 内建
  topic（端点真相）+ `ros_discovery_info`（节点命名）+ `rt/rosout`（日志环形
  缓冲）三路融合，输出 JSON 快照（nodes/topics/services/edges/endpoints/logs）。
  自身端点按 GUID 自排除；Python 独立入口 `python -m pyrcllite`。
- **GraphExecutor**（`src/rcllite/graph_executor.hpp`）：读拓扑 JSON（"node.yaml
  大合集"）在单进程内拉起全部节点——参数声明、裸 pub/sub 端点（opaque CDR，
  `payload_hex` + `rate_hz`）、QoS、相对 topic 展开；全部节点跑在一个共享
  Executor 上。schema 支持 `class` 字段实例化已注册的行为节点类。
- **节点类注册表**（`src/rcllite/node_registry.hpp`）：基于 vila
  `GlobalFactoryRegistry` 的编译期注册；`RCLLITE_REGISTER_NODE(Cls)` 注册
  行为节点类（`Cls : rcl::Node`，构造函数即行为），拓扑 `class` 字段按名
  实例化，配置装配叠加在构造行为之上。**链接约束：注册宏所在库必须
  `alwayslink = 1`**（静态初始化注册，否则被链接器裁剪）；无 dlopen/运行时
  插件（by design，未链接的节点以独立进程跑、线上互通）。
- 示例：`examples/graph/abc_graph.cpp`——A(计数)→B(累加)→C(打印) 三节点流水线，
  registry + GraphExecutor 的端到端参照。

## 开发要求

### 构建与测试（提交前的检查工具）

```bash
bazelisk build //...           # 全部目标必须编译通过
bazelisk test //tests/...      # 单元测试必须通过（mock ddsc，无网络，确定性）
```

- 单元测试通过 `tests/mock/mock_ddsc` 在链接级替换 CycloneDDS C API——新功能
  必须在这里覆盖（异步回调、超时、错误注入等），不要依赖真实网络测试。
  现有七组：client / executor / service / qos / names / cdr / graph_executor。
- 端到端测试带 `manual` 标签（真实 socket），只在环境允许时手动运行：
  `bazelisk test //tests:comms_test //tests:qos_e2e_test`。
- Python 侧：`pip install -e ".[test]"` 后 `pytest`（配置在 pyproject.toml）。
  静态检查三件套（配置均在 pyproject.toml，Python 定位 3.12）：`ruff check`
  + `ruff format --check`（preview 开启、行宽 88）、`pyright`、`pylint`
  （须全绿）；C++ 胶水（`src/ffi/rcllite_ffi.cpp`）改动后必须
  `bazelisk build //src/ffi:rcllite_ffi` 重建，否则 Python 侧仍是旧符号。
- 改序列化/图通告相关代码时，注意上一节的线协议约定，保持逐字节兼容。

### 代码格式

- C++：clang-format，Google 风格基座、行宽 88，配置在 `.clang-format`。
  提交前对改动文件执行 `clang-format -i`。
- 编译警告门槛（集中在 `build_defs.bzl`，由 `rcllite_cc_*` 包装自动应用）：
  MSVC `/W4 /utf-8 /permissive-`，gcc/clang `-Wall -Wextra -Wpedantic`。
  新代码不应引入警告；新增抑制需说明理由。
- 新的 cc 目标必须用 `build_defs.bzl` 的 `rcllite_cc_library/binary/test`，
  不要直接写裸 `cc_*`。

### 仓库约定

- 头文件与实现同目录于 `src/rcllite/`，include 路径从 `rcllite/` 起
  （`strip_include_prefix = "/src"`）；FFI 胶水在 `src/ffi/`（产物
  `//src/ffi:rcllite_ffi`，`linkshared`）。
- 生成的消息头 include 为 `rcllite_types/<package>/<snake_case_name>.hpp`；
  `msg/` 只放 `rcllite_msg_library` 目标声明（上游接口的编译现场，不搬进
  `src/`——源码与生成物分区是刻意约定）。
- **仓库不提交任何 .msg 副本或生成头**——接口原文由 Bazel 从上游拉取、
  构建时经 `tools/msg_codegen.py` 现场生成（hermetic Python 工具链）。
- 测试文件放 `tests/`（单元测试 dep 用 `//src/rcllite:rcllite_under_test`
  链 mock ddsc），示例放 `examples/`（图/拓扑类示例在 `examples/graph/`）。
- 回调统一在 `Executor::spin` 线程内派发，单 executor 内串行、无需加锁；
  `publish` / `async_send_request` 可在任意线程调用；同步 `Client::call`
  需要 executor 在另一线程 spin。写示例与文档时遵守此线程模型。
