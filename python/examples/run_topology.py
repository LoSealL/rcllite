# Copyright 2026 rcllite contributors
# Licensed under the Apache License, Version 2.0
"""Run a whole node topology inside this process.

Build the FFI library first: bazelisk build //src/ffi:rcllite_ffi
Run from the repository root:

    python python/examples/run_topology.py python/examples/topology.yaml 5

While it runs, `python -m pyrcllite` (in another process) shows the graph;
the real C++ listener also receives /chatter (wire-compatible CDR).
"""

import argparse
import json
import sys
import threading
import time

import pyrcllite


def main() -> int:
    """Launch the topology, spin it, print per-endpoint counters, shut down."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("topology", help="topology file (.yaml/.json)")
    parser.add_argument("seconds", nargs="?", type=float, default=10.0)
    args = parser.parse_args()

    with pyrcllite.GraphExecutor(args.topology) as executor:
        stop = threading.Event()

        def spin_until_stopped() -> None:
            while not stop.is_set():
                executor.spin_once(100)

        spinner = threading.Thread(target=spin_until_stopped)
        spinner.start()
        try:
            time.sleep(args.seconds)
        except KeyboardInterrupt:
            pass
        finally:
            stop.set()
            spinner.join()

        print(json.dumps(executor.status(), indent=2))
    print("topology down")
    return 0


if __name__ == "__main__":
    sys.exit(main())
