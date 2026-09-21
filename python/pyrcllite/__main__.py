# Copyright 2026 rcllite contributors
# Licensed under the Apache License, Version 2.0
"""Standalone graph capture entry point: ``python -m pyrcllite``."""

from __future__ import annotations

import argparse
import datetime
import sys
import time

from .graph import GraphMonitor


def main(argv: list[str] | None = None) -> int:
    """Run a standalone capture: settle, snapshot, save, summarize."""
    parser = argparse.ArgumentParser(
        prog="pyrcllite",
        description="Sniff live rcllite/ROS 2 nodes and save the directed graph.",
    )
    parser.add_argument(
        "-w",
        "--wait",
        type=float,
        default=3.0,
        metavar="SEC",
        help="seconds to let discovery settle before snapshotting (default: 3)",
    )
    parser.add_argument(
        "-o",
        "--out",
        default="rcllite_graph.json",
        metavar="PATH",
        help="output file (default: rcllite_graph.json)",
    )
    parser.add_argument(
        "-f",
        "--format",
        choices=["json", "graphml", "gexf", "dot"],
        default="json",
        help="output format (graphml/gexf/dot need networkx)",
    )
    parser.add_argument(
        "--logs",
        type=int,
        default=0,
        metavar="N",
        help="also print the last N rosout records",
    )
    args = parser.parse_args(argv)

    with GraphMonitor() as monitor:
        time.sleep(args.wait)
        snap = monitor.snapshot()
        out = monitor.save(args.out, fmt=args.format)

    edges = snap["edges"]
    nodes = snap["nodes"]
    print(f"nodes: {len(nodes)}  edges: {len(edges)}  -> {out}")
    for edge in edges:
        print(
            f"  {edge['source']} --{edge['kind']}:{edge['topic']}--> {edge['target']}"
        )
    if not edges:
        print("  (no topic/service edges observed)")
    if args.logs:
        for log in snap["logs"][-args.logs :]:
            stamp = datetime.datetime.fromtimestamp(log["stamp"] / 1e9)
            print(f"  [{stamp:%H:%M:%S}] {log['name']}: {log['msg']}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
