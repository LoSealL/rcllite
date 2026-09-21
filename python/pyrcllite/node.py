# Copyright 2026 rcllite contributors
# Licensed under the Apache License, Version 2.0
"""The rcllite node: initialization and execution from Python."""

from __future__ import annotations

from types import TracebackType

from ._ffi import ffi


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
        """
        timeout = -1 if timeout_ms is None else int(timeout_ms)
        return bool(self._ffi.node_spin_once(self._handle, timeout))

    def cancel(self) -> None:
        """Stop a running :meth:`spin` from any thread."""
        self._ffi.node_cancel(self._handle)

    def destroy(self) -> None:
        """Tear the node down (cancelling any running spin); idempotent."""
        if getattr(self, "_handle", 0):
            self._ffi.node_destroy(self._handle)
            self._handle = 0

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
            self.destroy()
        except Exception:  # pylint: disable=broad-exception-caught
            pass  # interpreter teardown, nothing to do

    def __repr__(self) -> str:
        handle = getattr(self, "_handle", 0)
        name = getattr(self, "_name", "?")
        namespace = getattr(self, "_namespace", "?")
        return f"Node(handle={handle}, name={name!r}, namespace={namespace!r})"
