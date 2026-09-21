// Copyright 2026 rcllite contributors
// Licensed under the Apache License, Version 2.0
//
// Compile-time node factory registry: lets users ship behavior-carrying node
// classes (MyNode : rcl::Node) that a GraphExecutor topology can
// instantiate by name via its "class" field, without the executor knowing
// the type at compile time.  Built on vila's GlobalFactoryRegistry.
//
// User side:
//
//   class MyTalker : public rcl::Node {
//    public:
//     MyTalker(const std::string& name, const std::string& ns)
//         : rcl::Node(name, ns) {
//       // behavior: create typed publishers/subscriptions, declare params...
//     }
//   };
//   RCLLITE_REGISTER_NODE(my::MyTalker);
//
// Topology side:
//
//   {"nodes": [{"name": "talker_1", "class": "my::MyTalker",
//               "parameters": {...}, "publishers": [...]}]}
//
// The config-driven parameters/endpoints are assembled on top of whatever
// the class constructor set up, so class behavior and topology structure
// compose freely.
//
// IMPORTANT (linkage): registration happens from static initialization, so
// the library holding RCLLITE_REGISTER_NODE must not be pruned by the
// linker.  Give the user cc_library `alwayslink = 1` (rcllite_cc_library
// forwards it) and add it to the deps of the binary that hosts the
// GraphExecutor (//src/ffi:rcllite_ffi for the Python bindings).
//
// The registry is compile-time only by design: classes must be linked into
// the hosting binary; there is no dlopen/plugin loading (nodes not linked
// in run as separate processes and interop over the wire).
#ifndef RCLLITE__NODE_REGISTRY_HPP_
#define RCLLITE__NODE_REGISTRY_HPP_

#include <functional>
#include <memory>
#include <string>

#include "rcllite/node.hpp"
#include "vila/widget/registration.h"

namespace rcl {

/// Factory signature: builds a Node (possibly a user subclass) from the
/// topology's name/namespace pair.
using NodeFactory = std::function<std::shared_ptr<Node>(const std::string& name,
                                                        const std::string& ns)>;

/// Process-wide name -> NodeFactory registry.  Names are C++-qualified
/// ("my::MyTalker"); a leading "::" is stripped, matching vila's rules.
using NodeRegistry =
    vila::GlobalFactoryRegistry<std::shared_ptr<Node>, const std::string&,
                                const std::string&>;

}  // namespace rcl

/// Register a Node subclass under its qualified name.  Place at namespace
/// scope in a translation unit of an `alwayslink = 1` library.
#define RCLLITE_REGISTER_NODE(Cls)                                                  \
  static auto REGISTRY_STATIC_VAR(rcllite_node_registration_, __LINE__) =           \
      ::rcl::NodeRegistry::Register(#Cls, [](const ::std::string& rcllite_reg_name, \
                                             const ::std::string& rcllite_reg_ns) { \
        return ::std::make_shared<Cls>(rcllite_reg_name, rcllite_reg_ns);           \
      })

#endif  // RCLLITE__NODE_REGISTRY_HPP_
