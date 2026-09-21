# Copyright 2026 rcllite contributors
# Licensed under the Apache License, Version 2.0
"""pyrcllite: bring up rcllite nodes from Python.

Thin TVM-FFI bindings over the C++ rcllite library, covering node
initialization and execution: create a :class:`Node`, then drive it with
:meth:`Node.spin` / :meth:`Node.spin_once` until :meth:`Node.cancel`.
The middleware context initializes implicitly when the first node is
created (ROS_DOMAIN_ID / ROS_LOCALHOST_ONLY are read then).

Requires the FFI shared library from ``bazelisk build //src/ffi:rcllite_ffi``
(see :mod:`pyrcllite._ffi` for how it is located) and ``pip install
apache-tvm-ffi``.

Example:
    >>> import pyrcllite
    >>> node = pyrcllite.Node("talker")
    >>> node.fully_qualified_name
    '/talker'
    >>> node.spin_once(100)  # one wait-and-dispatch round, 100 ms budget
    False
"""

from .executor import GraphExecutor
from .graph import GraphMonitor
from .node import Node

__version__ = "0.1.0"

__all__ = [
    "GraphExecutor",
    "GraphMonitor",
    "Node",
    "__version__",
]
