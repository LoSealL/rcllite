---
name: write-node
description: 用 C++ 编写新的 rcllite 节点：publisher/subscription/service/client/parameter/executor 的 API 用法、行为节点类注册（RCLLITE_REGISTER_NODE）、GraphExecutor 拓扑执行、BUILD 目标写法与线程模型。当需要新建可执行节点、给 examples 加示例、或解释 rcllite C++ API 用法时使用。
---

# 编写新的 C++ 节点

## 骨架

所有实体在 `rcllite/rcllite.hpp` 一个伞头里；消息类型 include 路径为
`rcllite_types/<package>/<snake_case_name>.hpp`。

```cpp
#include <cstdio>

#include "rcllite/rcllite.hpp"
#include "rcllite_types/rcl_interfaces/parameter.hpp"

int main() {
  auto node = std::make_shared<rcl::Node>("my_node");  // 上线即对 ros2 node list 可见

  // ... create_publisher / create_subscription / create_service / create_client ...

  rcl::spin(node);        // 阻塞直到进程退出（无显式 shutdown）
  return 0;
}
```

参考实现：`examples/talker.cpp`（pub）、`examples/listener.cpp`（sub）、
`examples/service_server.cpp`（srv）、`examples/service_client.cpp`、
`examples/parameters_demo.cpp`（param + Executor）、
`examples/graph/abc_graph.cpp`（行为节点类 + 拓扑执行器流水线）。

## 各实体的 API

**Publisher / Subscription**（模板参数为生成的消息结构体）：

```cpp
auto pub = node->create_publisher<rcl_interfaces::msg::Parameter>("chatter");
rcl_interfaces::msg::Parameter msg;
msg.name = "hello";
pub->publish(msg);                                    // 可在任意线程调用

// 订阅者可能尚未上线时（volatile QoS 下，无匹配 reader 时写出的样本
// 不会被后来的订阅者收到；transient-local 本就能收到，无需此 API）：
pub->wait_for_subscribers();                          // 阻塞直到首个订阅者匹配（默认 10s 超时）
bool ok = pub->publish(msg, std::chrono::seconds(1)); // 背压：无订阅者则等待；超时返回 false 且不写入，消息留在调用方
pub->get_subscription_count();                        // 当前匹配的订阅者数

node->create_subscription<rcl_interfaces::msg::Parameter>(
    "chatter", [](const rcl_interfaces::msg::Parameter& m) { /* ... */ });
```

**Service / Client**：

```cpp
auto server = node->create_service<rcl_interfaces::srv::GetParameters>(
    "get_parameters", [](const rcl_interfaces::srv::GetParameters::Request& req) {
      rcl_interfaces::srv::GetParameters::Response resp;
      /* 填充 resp ... */
      return resp;                                    // 回调返回 Response
    });

auto client = node->create_client<rcl_interfaces::srv::GetParameters>("get_parameters");
client->wait_for_service();
auto resp = client->call(req);                        // 同步：需另一线程 spin
client->async_send_request(req, on_resp);             // 异步：executor 线程内回调
```

**Parameter**（远程 `ros2 param list/get/set` 直接可用）：

```cpp
node->declare_parameter("speed", int64_t{42});
node->set_parameter("speed", rcl::param::to_value(int64_t{7}));
long speed = node->get_parameter<int64_t>("speed");   // get 按目标类型转换
```

**Executor**（需要自己控制 spin 线程/多节点时，替代 `rcl::spin`）：

```cpp
rcl::Executor executor;
executor.add_node(node);
std::thread spinner([&executor]() { executor.spin(); });
// ...
executor.cancel();     // 可从任意线程调用
spinner.join();
```

**QoS 子集**（`rcl::QoS`）：reliability（reliable/best-effort）、durability
（volatile/transient-local）、history（keep-last N / keep-all）；预设值与
`rmw_qos_profile_default/sensor_data/parameters/parameter_events/services` 一致。
deadline / lifespan / liveliness 未实现。

## 行为节点类与拓扑执行（graph 模式）

需要多个节点协作、或想被 `GraphExecutor`（含 Python 侧 `pyrcllite.GraphExecutor`）
按名拉起时，把节点写成 `Node` 子类并注册到编译期注册表
（`rcllite/node_registry.hpp`，基于 vila `GlobalFactoryRegistry`）：

```cpp
class NodeB : public rcl::Node {
 public:
  NodeB(const std::string& name, const std::string& ns)
      : rcl::Node(name, ns) {
    sum_pub_ = create_publisher<rcl_interfaces::msg::Parameter>("sum");
    create_subscription<rcl_interfaces::msg::Parameter>(
        "counter", [this](const rcl_interfaces::msg::Parameter& m) {
          /* 行为：回调里可再 publish（executor 线程内，合法） */
        });
  }
 private:
  std::shared_ptr<rcl::Publisher<rcl_interfaces::msg::Parameter>> sum_pub_;
};
RCLLITE_REGISTER_NODE(NodeB);   // 注意带分号；限定名（my::NodeB）亦可
```

拓扑 JSON 用 `class` 字段实例化，配置装配叠加在构造行为之上：

```cpp
rcl::GraphExecutor executor(R"({
  "nodes": [{"name": "node_b", "class": "NodeB"}]
})");
std::thread spinner([&executor]() { executor.spin(); });
// ... executor.cancel(); spinner.join();
```

约定与边界：

- **注册宏所在库必须 `alwayslink = 1`**——注册靠 static-init，普通静态库会被
  链接器裁剪、注册静默丢失；Python FFI 场景要把插件库加进
  `//src/ffi:rcllite_ffi` 的 deps。
- `class` 声明了但未注册是**报错**（绝不静默降级成空壳）；不写 `class` 的
  节点是纯配置骨架（裸端点 + payload_hex + rate_hz，见 `graph_executor.hpp`
  头注释里的完整 schema）。
- 需要周期发布的骨架节点用配置 `rate_hz`；行为类自起线程时须在析构里 join
  （参考 `examples/graph/abc_graph.cpp` 的 NodeA）。注册表是编译期的——没有
  dlopen/运行时插件，未链接的节点以独立进程跑、线上互通。
- 完整参照：`examples/graph/abc_graph.cpp`（A 计数→B 累加→C 打印，main 自带
  流水线断言）。

## 线程模型（写代码必须遵守）

- `Publisher::publish`、`Client::async_send_request`：任意线程。
- 阻塞变体 `publish(msg, timeout)`、`wait_for_subscribers` 会停住调用线程，
  不要在 executor 回调里调用（除非有意卡住 spin 线程）。
- 回调（订阅、服务、参数服务）：统一在 `Executor::spin` 线程内派发；
  **单 executor 内回调串行，无需加锁**。
- 同步 `Client::call` 需要 executor 在另一线程 spin，否则死等。
- `spin` / `spin_once` 不可在持有回调内锁的情况下调用。

## BUILD 目标

可执行文件用 `build_defs.bzl` 的 `rcllite_cc_binary`（自动带平台编译参数），
依赖消息库与核心库：

```python
load("//:build_defs.bzl", "rcllite_cc_binary")

rcllite_cc_binary(
    name="rcllite_my_node",
    srcs=["my_node.cpp"],
    deps=[
        "//msg:rcl_interfaces",  # 用到的消息包
        "//src/rcllite",
    ],
)
```

示例统一加进 `examples/BUILD.bazel` 的 `RCLLITE_EXAMPLES` 列表即可
（名字约定 `rcllite_<example>`）；图/拓扑类示例放独立的 `examples/graph/`
子包（见 `examples/graph/BUILD.bazel`）。

## 验证

```bash
bazelisk build //examples:rcllite_my_node
bazelisk run //examples:rcllite_my_node
```

与 ROS 2 互通联调（需真实网络；Windows 宿主 ↔ Docker NAT 下 UDP 不通，
两侧都要在容器/同一网络内，ROS 2 侧 `export RMW_IMPLEMENTATION=rmw_cyclonedds_cpp`）：

```bash
ros2 topic echo /chatter          # 收 rcllite talker 的消息
ros2 topic pub /chatter ...       # 发给 rcllite listener
ros2 node list                    # 应看到 rcllite 节点名
```

新功能（非示例）还需在 `tests/` 加单元测试：dep 用
`//src/rcllite:rcllite_under_test` + `//tests/mock:mock_ddsc`，
照 `tests/client_test.cpp` 的既有模式写。
