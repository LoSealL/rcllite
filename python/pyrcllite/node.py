# Copyright 2026 rcllite contributors
# Licensed under the Apache License, Version 2.0
"""The rcllite node: lifecycle, execution and typed pub/sub from Python."""

from __future__ import annotations

import atexit
import sys
import weakref
from collections.abc import Callable, Sequence
from types import TracebackType

from ._ffi import ffi
from .messages import WIRE_TYPES, Message
from .pubsub import Publisher, Subscription, make_trampoline
from .qos import QoS

# Nodes are torn down via atexit while the interpreter is still alive: a node
# carrying Python-callback subscriptions must not be destroyed during
# interpreter finalization (the C++ side would call back into a dying runtime).
_TRACKED_NODES: weakref.WeakSet[Node] = weakref.WeakSet()


@atexit.register
def _destroy_tracked_nodes() -> None:
    for node in list(_TRACKED_NODES):
        try:
            node.destroy()
        except Exception:  # pylint: disable=broad-exception-caught
            pass  # best effort during shutdown


def init_dds(
    tcp_port: int | None = None,
    peers: Sequence[str] | None = None,
    domain_id: int | None = None,
) -> None:
    """Fix the process DDS setup; call once, before creating the first node.

    ``tcp_port`` switches the participant to an all-TCP topology listening on
    that port and dialing exactly ``peers`` (``"host:port"`` entries; the
    node's own port is ignored if listed — never list a port that is not
    running).  ``domain_id`` overrides ``ROS_DOMAIN_ID``.  An explicit
    ``CYCLONEDDS_URI`` in the environment still wins over the generated TCP
    config, so deployments can always override.
    """
    ffi().init_dds(
        tcp_port if tcp_port else 0,
        list(peers) if peers else [],
        domain_id if domain_id is not None else -1,
    )


def _require_wire_type(msg_type: type[Message], what: str) -> None:
    if (
        not isinstance(msg_type, type)
        or not issubclass(msg_type, Message)
        or not getattr(msg_type, "__type_name__", "")
    ):
        known = ", ".join(sorted(WIRE_TYPES))
        raise TypeError(
            f"{what}: expected one of the shipped message types ({known}), "
            f"got {msg_type!r} — a wrong type name fails loudly here instead "
            "of silently never matching on the wire"
        )


class Node:
    """A ROS 2 node, visible on the DDS graph once created.

    Wraps ``rcl::Node`` plus a dedicated executor; construction performs
    the ``ros_discovery_info`` announcement, so ``ros2 node list`` (with
    rmw_cyclonedds peers) sees the node immediately.

    Args:
        name: node name (no ``/``).
        namespace_: node namespace; empty or ``/`` for the root namespace.
    """

    def __init__(self, name: str, namespace_: str = "/") -> None:
        self._ffi = ffi()
        self._handle = self._ffi.node_create(name, namespace_)
        # Snapshot the immutable identity so the properties (and repr) stay
        # usable after destroy() dropped the C++ side.
        self._name = self._ffi.node_name(self._handle)
        self._namespace = self._ffi.node_namespace(self._handle)
        self._fq_name = self._ffi.node_fully_qualified_name(self._handle)
        _TRACKED_NODES.add(self)

    @property
    def name(self) -> str:
        """The node's bare name, without namespace."""
        return self._name

    @property
    def namespace(self) -> str:
        """The node's namespace (``/`` for the root namespace)."""
        return self._namespace

    @property
    def fully_qualified_name(self) -> str:
        """The node's fully qualified name, ``<namespace>/<name>``."""
        return self._fq_name

    def spin(self) -> None:
        """Process events until :meth:`cancel` (from any thread)."""
        self._ffi.node_spin(self._handle)

    def spin_once(self, timeout_ms: float | None = None) -> bool:
        """Wait up to ``timeout_ms`` for one batch of events and process it.

        ``None`` waits forever.  Returns whether any events were processed.
        Subscription callbacks run on the calling thread of this method.
        """
        timeout = -1 if timeout_ms is None else int(timeout_ms)
        return bool(self._ffi.node_spin_once(self._handle, timeout))

    # --- pub/sub ---------------------------------------------------------------

    def create_publisher[T: Message](
        self, msg_type: type[T], topic: str, qos: QoS | None = None
    ) -> Publisher:
        """Create a typed publisher on ``topic`` (returns immediately)."""
        _require_wire_type(msg_type, "create_publisher")
        if qos is None:
            qos = QoS()
        handle = self._ffi.pub_create(
            self._handle,
            topic,
            msg_type.__type_name__,
            qos.depth,
            qos.reliable,
            qos.transient_local,
        )
        return Publisher(self._ffi, handle, msg_type, topic)

    def create_subscription[T: Message](
        self,
        msg_type: type[T],
        topic: str,
        callback: Callable[[T], None],
        qos: QoS | None = None,
    ) -> Subscription:
        """Subscribe ``topic``; ``callback(msg)`` runs on the spin thread."""
        _require_wire_type(msg_type, "create_subscription")
        if not callable(callback):
            raise TypeError(f"callback must be callable, got {callback!r}")
        if qos is None:
            qos = QoS()
        handle = self._ffi.sub_create(
            self._handle,
            topic,
            msg_type.__type_name__,
            qos.depth,
            qos.reliable,
            qos.transient_local,
            make_trampoline(msg_type, callback),
        )
        return Subscription(self._ffi, handle, msg_type, topic)

    def cancel(self) -> None:
        """Stop a running :meth:`spin` from any thread."""
        self._ffi.node_cancel(self._handle)

    def destroy(self) -> None:
        """Tear the node down (cancelling any running spin); idempotent."""
        if getattr(self, "_handle", 0):
            self._ffi.node_destroy(self._handle)
            self._handle = 0
        _TRACKED_NODES.discard(self)

    def __enter__(self) -> Node:
        return self

    def __exit__(
        self,
        exc_type: type[BaseException] | None,
        exc: BaseException | None,
        tb: TracebackType | None,
    ) -> None:
        self.destroy()

    def __del__(self) -> None:
        try:
            if sys.is_finalizing():
                return  # the process is dying; crossing FFI now is unsafe
            self.destroy()
        except Exception:  # pylint: disable=broad-exception-caught
            pass  # interpreter teardown, nothing to do

    def __repr__(self) -> str:
        handle = getattr(self, "_handle", 0)
        name = getattr(self, "_name", "?")
        namespace = getattr(self, "_namespace", "?")
        return f"Node(handle={handle}, name={name!r}, namespace={namespace!r})"
