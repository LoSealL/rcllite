# Copyright 2026 rcllite contributors
# Licensed under the Apache License, Version 2.0
"""Runtime graph sniffing: observe live nodes and save the directed graph.

A :class:`GraphMonitor` runs a C++ observer (built-in DDS topics +
``ros_discovery_info`` + ``rt/rosout``) that watches the domain without
announcing itself.  :meth:`GraphMonitor.snapshot` returns the current graph
as plain data; :meth:`GraphMonitor.save` persists it, either as the raw JSON
document or — with networkx installed — as GraphML/GEXF/DOT for direct use
as a ``networkx.DiGraph``.

Standalone use (from the repository root, after
``bazelisk build //src/ffi:rcllite_ffi``):

    python -m pyrcllite --wait 5 --out graph.json
    python -m pyrcllite --wait 5 --out graph.graphml --format graphml
"""

from __future__ import annotations

import json
from pathlib import Path
from typing import TYPE_CHECKING, Any

from ._ffi import ffi

if TYPE_CHECKING:
    import networkx as nx


def _networkx() -> Any:
    """Import networkx lazily: it is an optional extra, not a hard dep."""
    import networkx  # pylint: disable=import-outside-toplevel

    return networkx


class GraphMonitor:
    """Observe the live node graph on the DDS domain.

    The monitor itself is invisible to the observed system; the snapshot's
    ``edges`` are directed data-flow links (publisher -> subscription node,
    service client -> service server), with plumbing topics excluded.
    """

    def __init__(self) -> None:
        self._ffi = ffi()

    def start(self) -> None:
        """Start sniffing (idempotent)."""
        self._ffi.graph_start()

    def stop(self) -> None:
        """Stop sniffing (idempotent)."""
        self._ffi.graph_stop()

    def __enter__(self) -> GraphMonitor:
        self.start()
        return self

    def __exit__(self, *exc: object) -> None:
        self.stop()

    def snapshot(self) -> dict[str, Any]:
        """Return the current graph as the raw JSON document, parsed."""
        return json.loads(self._ffi.graph_snapshot())

    def digraph(self) -> nx.DiGraph:
        """Return the snapshot as a directed graph.

        Nodes carry a ``source`` attribute (``discovery`` / ``user_data`` /
        ``anonymous``); edges carry ``topic`` and ``kind``
        (``topic`` / ``service``).  Requires the ``graph`` extra
        (``pip install pyrcllite[graph]``).
        """
        graph = _networkx().DiGraph()
        snap = self.snapshot()
        for node in snap["nodes"]:
            graph.add_node(node["name"], source=node["source"])
        for edge in snap["edges"]:
            graph.add_edge(
                edge["source"], edge["target"], topic=edge["topic"], kind=edge["kind"]
            )
        return graph

    def save(self, path: str | Path, fmt: str = "json") -> Path:
        """Save the graph data to ``path`` in format ``fmt``.

        ``json`` (default) writes the raw snapshot document — nodes, topics,
        services, edges, endpoints and the rosout log ring; ``graphml`` /
        ``gexf`` / ``dot`` write the directed graph via networkx.
        """
        path = Path(path)
        if fmt == "json":
            path.write_text(json.dumps(self.snapshot(), indent=2), encoding="utf-8")
            return path
        networkx = _networkx()
        graph = self.digraph()
        if fmt == "graphml":
            networkx.write_graphml(graph, path)
        elif fmt == "gexf":
            networkx.write_gexf(graph, path)
        elif fmt == "dot":
            networkx.nx_agraph.write_dot(graph, path)  # needs pygraphviz
        else:
            raise ValueError(f"unknown format: {fmt}")
        return path
