# Copyright 2026 rcllite contributors
# Licensed under the Apache License, Version 2.0
"""pyrcllite: bring up rcllite nodes from Python.

Thin TVM-FFI bindings over the C++ rcllite library, covering node
initialization and execution plus typed pub/sub: create a :class:`Node`,
exchange the shipped message types (:mod:`pyrcllite.messages`) with
:func:`Node.create_publisher` / :func:`Node.create_subscription`, then drive
it with :meth:`Node.spin` / :meth:`Node.spin_once` until :meth:`Node.cancel`.
The middleware context initializes implicitly when the first node is created
(ROS_DOMAIN_ID / ROS_LOCALHOST_ONLY are read then; :func:`init_dds` overrides
beforehand).  On the wire the endpoints interoperate with ROS 2 / rcllite C++
nodes — payloads are standard CDR and type names match ``ros2 topic``.

Requires the FFI shared library from ``bazelisk build //src/ffi:rcllite_ffi``
(see :mod:`pyrcllite._ffi` for how it is located) and ``pip install
apache-tvm-ffi``.

Example:
    >>> import pyrcllite
    >>> node = pyrcllite.Node("talker")
    >>> pub = node.create_publisher(pyrcllite.String, "/chatter")
    >>> pub.publish(pyrcllite.String(data="hello"))
    >>> node.spin_once(100)  # one wait-and-dispatch round, 100 ms budget
    False
"""

from .executor import GraphExecutor
from .graph import GraphMonitor
from .messages import (
    CameraInfo,
    Empty,
    Image,
    PoseStamped,
    String,
    TransformStamped,
)
from .node import Node, init_dds
from .pubsub import Publisher, Subscription
from .qos import QoS

__version__ = "0.1.0"

__all__ = [
    "CameraInfo",
    "Empty",
    "GraphExecutor",
    "GraphMonitor",
    "Image",
    "Node",
    "PoseStamped",
    "Publisher",
    "QoS",
    "String",
    "Subscription",
    "TransformStamped",
    "__version__",
    "init_dds",
]
