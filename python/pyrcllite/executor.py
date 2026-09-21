# Copyright 2026 rcllite contributors
# Licensed under the Apache License, Version 2.0
"""Config-driven topology execution: launch every node of a graph in one process.

A :class:`GraphExecutor` takes a complete topology — the "node.yaml 大合
集", i.e. every node's concrete configuration — instantiates all of it
C++-side (nodes, parameters, raw publishers/subscriptions, publish rates)
and runs the whole graph on one shared executor.  Semantically this is
equivalent to each node executing its own configuration separately; the
difference is only that everything lives in one process (communication
still goes through DDS loopback — rcllite has no intra-process path).

The C++ side consumes JSON; this layer accepts

* a ``dict`` (the topology as Python data),
* JSON text,
* YAML text (needs PyYAML, ``pip install pyrcllite[launch]``),
* a path to a ``.yaml``/``.json`` file.

Schema (see ``examples/topology.yaml``)::

    nodes:
      - name: talker            # required
        namespace: /            # optional
        class: my::MyTalker     # optional: a Node subclass registered in the
                                # hosting binary with RCLLITE_REGISTER_NODE;
                                # its constructor's behavior runs, then the
                                # config below is assembled on top
        parameters: {rate: 10}  # optional: bool/int/float/str
        publishers:             # optional
          - topic: /chatter     # relative names expand against the namespace
            type: rcl_interfaces/msg/Parameter     # or the full DDS type name
            payload_hex: "00010000..."             # raw CDR incl. header
            rate_hz: 1.0                           # absent/0: publish once
            qos: {reliability: reliable, durability: volatile, depth: 10}
        subscriptions:          # optional; samples counted, not deserialized
          - {topic: /chatter, type: rcl_interfaces/msg/Parameter}
"""

from __future__ import annotations

import json
from pathlib import Path
from typing import Any

from ._ffi import ffi


def registered_node_classes() -> list[str]:
    """Names available for the topology ``class`` field (registered in the
    FFI binary via ``RCLLITE_REGISTER_NODE``)."""
    return list(json.loads(ffi().graph_exec_registered_nodes()))


def _topology_to_json(topology: str | Path | dict[str, Any]) -> str:
    """Normalize a topology (dict / JSON text / YAML text / file) to JSON text."""
    if isinstance(topology, dict):
        return json.dumps(topology)
    path = Path(topology)
    text = path.read_text(encoding="utf-8") if path.is_file() else str(topology)
    try:
        return json.dumps(json.loads(text))
    except json.JSONDecodeError:
        try:
            import yaml  # pylint: disable=import-outside-toplevel
            # (optional "launch" extra, deliberately lazy)
        except ImportError as exc:  # pragma: no cover - depends on extras
            raise ImportError(
                "YAML topologies need PyYAML: pip install pyrcllite[launch]"
            ) from exc
        loaded = yaml.safe_load(text)
        if not isinstance(loaded, dict):
            raise ValueError("topology must be a mapping with a 'nodes' list") from None
        return json.dumps(loaded)


class GraphExecutor:
    """Run an entire node-graph topology inside this process."""

    def __init__(self, topology: str | Path | dict[str, Any]) -> None:
        """Build every node/endpoint described by ``topology`` immediately."""
        self._ffi = ffi()
        self._handle = self._ffi.graph_exec_create(_topology_to_json(topology))

    def spin(self) -> None:
        """Spin all nodes until :meth:`cancel` (from any thread)."""
        self._ffi.graph_exec_spin(self._handle)

    def spin_once(self, timeout_ms: float | None = None) -> bool:
        """One wait-and-dispatch round across all nodes."""
        timeout = -1 if timeout_ms is None else int(timeout_ms)
        return bool(self._ffi.graph_exec_spin_once(self._handle, timeout))

    def cancel(self) -> None:
        """Stop a running :meth:`spin` from any thread."""
        self._ffi.graph_exec_cancel(self._handle)

    def status(self) -> dict[str, Any]:
        """Per-node counters (published / received) as parsed JSON."""
        return json.loads(self._ffi.graph_exec_status(self._handle))

    def destroy(self) -> None:
        """Stop publishing and tear every node down; idempotent."""
        if getattr(self, "_handle", 0):
            self._ffi.graph_exec_destroy(self._handle)
            self._handle = 0

    def __enter__(self) -> GraphExecutor:
        return self

    def __exit__(self, *exc: object) -> None:
        self.destroy()

    def __del__(self) -> None:
        try:
            self.destroy()
        except Exception:  # pylint: disable=broad-exception-caught
            pass  # interpreter teardown, nothing to do
