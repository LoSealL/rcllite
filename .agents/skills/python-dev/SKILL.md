---
name: python-dev
description: rcllite 的 Python 侧开发：pyrcllite 绑定的构建、FFI 库加载机制、Node/GraphMonitor/GraphExecutor API、节点类注册表、新增 FFI 函数的流程、lint 三件套与 pytest。当需要开发/调试 Python 绑定、扩展 FFI 接口、排查 rcllite_ffi 库加载失败时使用。
---

# Python 侧开发（pyrcllite）

## 架构

```
python/pyrcllite/          纯 Python 包（定位 3.12）
  _ffi.py                  FFI 库定位与加载（cache 单例）
  node.py                  Node 包装（句柄 + 专属 Executor）
  graph.py                 GraphMonitor（快照 / digraph / save）
  executor.py              GraphExecutor（拓扑 → 单进程拉起）+ registered_node_classes()
  __main__.py              独立抓图 CLI：python -m pyrcllite
src/ffi/rcllite_ffi.cpp    C++ 胶水（TVM FFI，TVM_FFI_DLL_EXPORT_TYPED_FUNC）
                           //src/ffi:rcllite_ffi 编译为共享库（linkshared）
apache-tvm-ffi (pip)       FFI 运行时（复用 vila 的机制；Bazel 侧经
                           vila 的 tvm_ffi_extension 从 pip 包发现头与 libtvm_ffi）
```

C++ 对象一律用 int64 handle 寻址（进程级 registry，handle 不复用，0 恒无效）；
Python 侧只做薄包装（类型注解 + docstring）。FFI 导出分三组：
`node_*`（生命周期 + spin/cancel）、`graph_*`
（嗅探 start/stop/snapshot）、`graph_exec_*`（拓扑 create/spin/spin_once/
cancel/status/destroy/registered_nodes）。

## 构建与安装

```bash
pip install apache-tvm-ffi             # FFI 运行时（Bazel 构建也依赖它，先装）
bazelisk build //src/ffi:rcllite_ffi   # 产物在 bazel-bin/src/ffi/
pip install -e ".[test,graph,launch]"  # graph=networkx（GraphML/GEXF/DOT），
                                       # launch=PyYAML（YAML 拓扑）
```

## 共享库查找顺序（`pyrcllite/_ffi.py`）

1. `$RCLLITE_FFI_LIB` —— 显式指定共享库路径（CI/部署用）
2. `<workspace>/bazel-bin/src/ffi/` —— 仓库内开发默认命中
3. 当前工作目录

Windows 下加载前会把库所在目录注册为 DLL 搜索目录，以便解析同级运行时
DLL（libtvm_ffi）。加载失败的报错会列出全部尝试路径——排查时先确认
`bazelisk build //src/ffi:rcllite_ffi` 是否成功、再看路径列表。

## 三块能力

### 1. 节点初始化与执行（`node.py`）

```python
import pyrcllite

node = pyrcllite.Node("talker")  # 创建即对 ros2 node list 可见
node.name
node.namespace
node.fully_qualified_name
node.spin_once(100)  # 等待最多 100ms 处理一批事件；timeout_ms=None 无限等
node.spin()
node.cancel()  # 阻塞 spin + 任意线程 cancel
node.destroy()  # 幂等；或用 with 语句
```

后台 spin 模式见 `python/examples/bring_up_node.py`（threading + spin_once 循环）。

### 2. 运行图嗅探（`graph.py`，C++ 侧 `src/rcllite/graph.hpp`）

```python
with pyrcllite.GraphMonitor() as m:
    time.sleep(3)  # 等发现收敛
    snap = m.snapshot()  # dict: nodes/topics/services/edges/endpoints/logs
    m.save("graph.json")  # 原始数据
    m.save("graph.graphml", fmt="graphml")  # networkx DiGraph（需 graph extra）
```

独立抓图（不依赖任何节点代码）：`python -m pyrcllite --wait 5 -o graph.json
--logs 20`。信息源三路融合：DDS 内建 topic（端点真相，含 rcllite 自身）、
`ros_discovery_info`（节点命名，权威）、`rt/rosout`（日志环形缓冲，仅 ROS 2
对端发布）。edges 为有向数据流（pub→sub、client→server），管道 topic
（ros_discovery_info / rt/rosout / rt/parameter_events）保留在原始数据但不进 edges。

### 3. 拓扑执行（`executor.py`，C++ 侧 `src/rcllite/graph_executor.hpp`）

```python
with pyrcllite.GraphExecutor("topology.yaml") as e:  # 也接受 .json/文本/dict
    e.spin_once(100)
    e.status()  # 每端点 published/received 计数
```

节点 schema：`name`（必需）、`namespace`、`class`（已注册行为类，见下）、
`parameters`（bool/int/float/str）、`publishers`（`topic`/`type`/`payload_hex`
原始 CDR 含封装头/`rate_hz`/`qos`）、`subscriptions`（计数不反序列化）。
类型名接受 `pkg/msg/Name` 或完整 DDS 名；相对 topic 按 namespace 展开。
完整示例：`python/examples/topology.yaml` + `run_topology.py`。

### 4. 行为节点类注册表（`src/rcllite/node_registry.hpp`）

带行为的节点写成 `Node` 子类并注册，拓扑 `class` 字段按名实例化：

```cpp
class MyTalker : public rcl::Node {
 public:
  MyTalker(const std::string& name, const std::string& ns)
      : rcl::Node(name, ns) { /* 类型化 pub/sub、回调 */ }
};
RCLLITE_REGISTER_NODE(my::MyTalker);
```

构造行为先跑，配置装配叠加其上；class 未注册是**报错**（不静默降级）。
Python 查询可用类：`pyrcllite.registered_node_classes()`。端到端参照：
`examples/graph/abc_graph.cpp`（A 计数→B 累加→C 打印）。

## 新增一个 FFI 函数

1. `src/ffi/rcllite_ffi.cpp`：实现函数，用 `TVM_FFI_DLL_EXPORT_TYPED_FUNC`
   导出。约定：引用已有 `lookup(handle)` 辅助；抛错用
   `TVM_FFI_THROW(ValueError)`。typed 函数支持 void/bool/int64/double/string。
2. 若返回/持有 C++ 对象，进 registry（`g_*_mtx` + map），Python 侧只拿
   int64 handle；析构要能安全处理并发 spin（参考 `node_destroy`——先 cancel
   再释放）。阻塞调用（spin 等）无需特殊处理：TVM FFI 默认释放 GIL。
3. `python/pyrcllite/` 加薄包装（类型注解 + docstring，参考 `node.py`）。
4. 重建：`bazelisk build //src/ffi:rcllite_ffi`（`.py` 改动无需重建）。

## Lint 与测试（提交前三件套全绿）

配置全在根 `pyproject.toml`，Python 定位 3.12：

```bash
ruff check python && ruff format --check python   # preview 开启、行宽 88
python -m pyright                                 # include = python/
python -m pylint python                           # 10.00/10
pip install -e ".[test]" && pytest                # testpaths = python/tests
```

注意：有意的宽泛 except / 惰性 import 用内联 `# pylint: disable=...` 注明理由；
`__init__.py` 只放 docstring 与 re-export（ruff preview 的 non-empty-init-module）。

## 验证

```bash
bazelisk build //src/ffi:rcllite_ffi   # C++ 胶水改动后必须重建
python python/examples/bring_up_node.py 10     # 冒烟：ros2 node list 显示 /py_bringup
python python/examples/run_topology.py python/examples/topology.yaml 5
python -m pyrcllite --wait 5 -o graph.json     # 抓图（可对照上方两个进程）
bazelisk run //examples/graph:abc_graph        # C++ 侧行为节点流水线（自带断言）
```

C++ 侧单测在 `tests/graph_executor_test.cpp`（mock ddsc）。跨机/容器互通注意
事项同 C++（见 write-node skill）。
