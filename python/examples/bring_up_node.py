# Copyright 2026 rcllite contributors
# Licensed under the Apache License, Version 2.0
"""Bring up an rcllite node from Python.

Build the FFI library first: bazelisk build //src/ffi:rcllite_ffi
Run from the repository root: python python/examples/bring_up_node.py [seconds]

The node announces itself on ros_discovery_info, so on a ROS 2 host with
rmw_cyclonedds peers `ros2 node list` shows /py_bringup after a few seconds.
"""

import sys
import threading
import time

import pyrcllite


def main() -> None:
    """Bring a node up, spin it for the requested seconds, tear it down."""
    seconds = float(sys.argv[1]) if len(sys.argv) > 1 else 10.0

    with pyrcllite.Node("py_bringup") as node:
        print(
            f"node up: {node.fully_qualified_name} (domain visible to `ros2 node list`)"
        )
        stop = threading.Event()

        def spin_until_stopped() -> None:
            while not stop.is_set():
                node.spin_once(100)

        spinner = threading.Thread(target=spin_until_stopped)
        spinner.start()
        try:
            time.sleep(seconds)
        except KeyboardInterrupt:
            pass
        finally:
            stop.set()
            spinner.join()

    print("node down")


if __name__ == "__main__":
    main()
