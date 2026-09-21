---
name: msg-codegen
description: 为 rcllite 新增或编译 ROS 2 消息/服务类型（.msg/.srv → C++ 头）。当需要引入新的接口包、为已有包添加 .msg/.srv、排查 rcllite_msg_library 生成失败、或解释 rcllite_types include 路径时使用。
---

# 消息类型编译（.msg/.srv → C++ 头）

## 机制

rcllite **不提交任何 .msg 副本或生成头**。流程（类比 rules_proto）：

```
MODULE.bazel (new_git_repository 拉上游接口仓库, pin 在 humble 分支)
  → bazel/<repo>.BUILD (exports_files 导出 .msg/.srv 原文)
  → msg/BUILD.bazel 的 rcllite_msg_library 规则 (bazel/rules/msg.bzl)
  → genrule 调 tools/msg_codegen.py (hermetic python_3_12 工具链)
  → 构建时生成 C++17 头, cc_library 以 "rcllite_types/<pkg>/xxx.hpp" 导出
```

生成器输出与 ROS 2 逐字节一致的 CDR 序列化（XCDR1/CDR-LE，含嵌套结构体、
定长/变长数组、有界序列、默认值）。`wstring` 不支持（跳过并告警）。

已内置的官方接口包（见 `msg/BUILD.bazel`）：
`builtin_interfaces`、`rcl_interfaces`（参数所需全部 msg/srv）、
`rosgraph_msgs`、`rmw_dds_common`（图通告）。来源仓库均为
`@rcl_interfaces` / `@rmw_dds_common`（pin 见 MODULE.bazel）。

## 直接使用已有类型

```cpp
#include "rcllite_types/rcl_interfaces/parameter.hpp"   // CamelCase → snake_case
// 命名空间与 ROS 2 一致: rcl_interfaces::msg::Parameter
// srv: rcl_interfaces::srv::GetParameters (Request / Response 内嵌)
```

BUILD 依赖：`//msg:rcl_interfaces`（或对应包名）。

## 新增一个其它接口包（三步）

以 `my_pkg` 为例，源在 `https://github.com/org/my_interfaces.git`：

**1. MODULE.bazel** —— 用 `iface_git`（`new_git_repository` 的 use_repo_rule，
照抄已有的 `rcl_interfaces` 块）拉取并 pin commit：

```python
iface_git(
    name="my_interfaces",
    build_file="//bazel:my_interfaces.BUILD",
    commit="<上游 commit>",
    remote="https://github.com/org/my_interfaces.git",
)
```

**2. bazel/my_interfaces.BUILD** —— 导出接口原文：

```python
package(default_visibility=["//visibility:public"])

exports_files(
    glob([
        "my_pkg/msg/*.msg",
        "my_pkg/srv/*.srv",
    ]),
)
```

**3. msg/BUILD.bazel** —— 声明生成目标（必须写在顶层 `msg` 目录，规则里
`--output "$(BINDIR)/msg"` 依赖这一点）：

```python
load("//bazel/rules:msg.bzl", "rcllite_msg_library")

rcllite_msg_library(
    name="my_pkg",
    package="my_pkg",
    files=["@my_interfaces//:my_pkg/msg/MyType.msg"],
    deps=[":builtin_interfaces"],  # 嵌套类型所依赖的其它 rcllite_msg_library
)
```

## 验证

```bash
bazelisk build //msg:my_pkg
# 头文件出现在 bazel-bin/msg/rcllite_types/my_pkg/my_type.hpp，可直接检查生成结果
```

若要快速试一个自有类型而不建外部仓库：`files` 也可以指向仓库内任意
filegroup/标签下的 `.msg` 文件（规则只消费文件内容）。

## 线协议注意

- 类型名映射为 `pkg::msg::dds_::Name_`（srv 为 `..._Request_/_Response_`），
  与 ROS 2 一致，勿改。
- 序列长度前缀恒 4 字节对齐、元素各自对齐——改动 `tools/msg_codegen.py`
  的序列化逻辑前先用 `tools/debug_raw.cpp` 抓包对比 ROS 2 侧字节。
- 上游 pin 分支决定互通的 ROS 2 发行版（当前 humble；`rmw_dds_common` 的
  `Gid` 在 iron/jazzy/rolling 为 `char[16]`，换 pin 时注意）。
