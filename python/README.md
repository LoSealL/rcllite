# pyrcllite

Python bindings for [rcllite](../README.md), built on
[TVM FFI](https://github.com/apache/tvm-ffi) — the same mechanism vila uses.
Two areas are covered: **node initialization and execution** (creating a
node brings a ROS 2-visible participant onto the DDS graph, and the
executor calls keep it alive and processing), and **runtime graph
sniffing** (observing all live nodes and saving the directed data-flow
graph).

## Setup

```bash
pip install apache-tvm-ffi          # FFI runtime (headers + libtvm_ffi)
bazelisk build //src/ffi:rcllite_ffi  # the bindings shared library
```

The loader searches `$RCLLITE_FFI_LIB`, then `bazel-bin/src/ffi/` inside the
repository, then the working directory (see `pyrcllite/_ffi.py`).  To install
the pure-Python package for development:

```bash
pip install -e .
```

## Usage

```python
import pyrcllite

node = pyrcllite.Node("talker")  # visible to `ros2 node list`
print(node.fully_qualified_name)  # /talker

node.spin_once(100)  # one wait-and-dispatch round

# Or block until cancelled from another thread:
import threading

spinner = threading.Thread(target=node.spin)
spinner.start()
...
node.cancel()  # Executor.cancel() semantics
spinner.join()

node.destroy()  # or use `with pyrcllite.Node(...) as n:`
```

## Graph sniffing

`GraphMonitor` passively observes the whole domain — CycloneDDS built-in
topics for the endpoint ground truth, `ros_discovery_info` for ROS 2 node
names, `rt/rosout` for logs — without announcing itself.  It sees both
rcllite and standard ROS 2 nodes.

```python
from pyrcllite import GraphMonitor

with GraphMonitor() as monitor:
    import time

    time.sleep(3)  # let discovery settle
    snap = monitor.snapshot()  # dict: nodes/topics/services/edges/endpoints/logs
    monitor.save("graph.json")  # raw data
    monitor.save("graph.graphml", fmt="graphml")  # networkx DiGraph
```

Standalone capture (independent of any node code):

```bash
python -m pyrcllite --wait 5 --out graph.json
python -m pyrcllite -o graph.graphml -f graphml --logs 20
```

`edges` are directed data-flow links — `publisher node -> subscription
node` for topics (`kind: topic`), `client -> server` for services
(`kind: service`); plumbing topics (`ros_discovery_info`, `rt/rosout`,
`rt/parameter_events`) stay in the raw data but not in the edges.  The
`graphml`/`gexf`/`dot` formats need the `graph` extra
(`pip install -e ".[graph]"`).  `logs` carries the latest `rt/rosout`
records (ROS 2 peers; rcllite nodes do not publish rosout).

## Topology execution (GraphExecutor)

`GraphExecutor` runs a **complete topology** — every node's concrete
configuration, the "node.yaml 大合集" — inside one process.  The C++ side
(`src/rcllite/graph_executor.hpp`) parses the topology, creates every node,
parameter, publisher and subscription, and drives them all on one shared
executor: equivalent to each node executing its own config separately, just
co-located (communication still crosses DDS loopback — no intra-process path).

```python
from pyrcllite import GraphExecutor

with GraphExecutor("topology.yaml") as executor:
    executor.spin_once(100)
    print(executor.status())  # per-endpoint published/received counters
```

Accepts a `.yaml`/`.json` file path, YAML/JSON text, or a dict.  YAML needs
the `launch` extra (`pip install -e ".[launch]"`).  The schema is documented
in `pyrcllite/executor.py`; a runnable demo lives in
`python/examples/topology.yaml`:

```bash
python python/examples/run_topology.py python/examples/topology.yaml 5
```

Publishers write raw CDR (`payload_hex`, encapsulation header included) — a
typed ROS 2 / rcllite peer deserializes them normally (verified against the
real C++ listener).  Subscriptions count samples without deserializing — the
topology carries structure, not behavior.  Services are not in the schema
yet.

### Behavior-carrying node classes (`class` field)

Structure-only nodes are skeletons; to run real behavior inside the same
process, register a `Node` subclass **in the hosting binary** with the
compile-time registry (vila's `GlobalFactoryRegistry` under the hood):

```cpp
class MyTalker : public rcl::Node {
 public:
  MyTalker(const std::string& name, const std::string& ns)
      : rcl::Node(name, ns) { /* typed pubs/subs, callbacks... */ }
};
RCLLITE_REGISTER_NODE(my::MyTalker);   // rcllite/node_registry.hpp
```

and reference it in the topology (`class: my::MyTalker`); the constructor's
behavior runs, then config-driven parameters/endpoints are assembled on top.
Registration is static-initialization based, so the library holding it must
be built with `alwayslink = 1` and added to the deps of the hosting binary
(e.g. `//src/ffi:rcllite_ffi`).  Unknown class names are an error (never a
silent skeleton).  `pyrcllite.registered_node_classes()` lists the names
available in the loaded binary.  Nodes not linked in keep running as
separate processes and interop over the wire — the registry is compile-time
only, there is no dlopen/plugin loading by design.

## Layout

- `pyrcllite/_ffi.py` — locates and loads the FFI shared library.
- `pyrcllite/node.py` — the `Node` wrapper (create / spin / spin_once /
  cancel / destroy).
- `pyrcllite/executor.py` — the `GraphExecutor` (topology file → all nodes
  running in-process; `status()` counters).
- `pyrcllite/graph.py` — the `GraphMonitor` wrapper (snapshot / digraph /
  save).
- `pyrcllite/__main__.py` — the standalone capture CLI (`python -m pyrcllite`).
- `../src/rcllite/graph.hpp` — the C++ observer.
- `../src/rcllite/graph_executor.hpp` — the C++ topology executor.
- `../src/ffi/rcllite_ffi.cpp` — the C++ side (`TVM_FFI_DLL_EXPORT_TYPED_FUNC`).
- `examples/` — runnable demos.

## Lint / type check

Configs live in the root `pyproject.toml`: ruff (preview mode, line length
88), pyright and pylint.

```bash
ruff check python && ruff format --check python
pyright
pylint python
```
