# Copyright 2026 rcllite contributors
# Licensed under the Apache License, Version 2.0
"""Typed pub/sub demo: a python talker and listener exchanging std_msgs/String.

Build the FFI library first: bazelisk build //src/ffi:rcllite_ffi
Run from the repository root: python python/examples/pub_sub.py [seconds]

The default run is self-contained: one process hosts both nodes, streams
Strings through DDS loopback, then replays the transient-local latch demo
(publish first, subscribe later — the late joiner still gets the sample).

Interop: point it at an existing String topic to cross language lines, e.g.
with stellaporta's native nodes

    python python/examples/pub_sub.py --sub-only --topic /stlp_talk
    python python/examples/pub_sub.py --pub-only --topic /stlp_talk

or with any ROS 2 / rcllite C++ talker-listener pair on the same domain.
For an all-TCP topology add e.g. --port 7606 --peer 127.0.0.1:7601.
"""

import argparse
import threading
import time

import pyrcllite


def parse_args() -> argparse.Namespace:
    """Collect the demo knobs."""
    doc = __doc__ or "pyrcllite typed pub/sub demo"
    parser = argparse.ArgumentParser(description=doc.splitlines()[0])
    parser.add_argument(
        "--duration",
        type=float,
        default=6.0,
        help="seconds to stream before the latch demo",
    )
    parser.add_argument("--rate", type=float, default=2.0, help="publish rate [Hz]")
    parser.add_argument("--topic", default="/py_pubsub", help="ROS topic")
    parser.add_argument("--pub-only", action="store_true", help="only publish")
    parser.add_argument("--sub-only", action="store_true", help="only subscribe")
    parser.add_argument("--port", type=int, help="own TCP port (init_dds topology)")
    parser.add_argument(
        "--peer",
        action="append",
        default=[],
        help="peer host:port to dial (repeatable)",
    )
    parser.add_argument("--domain", type=int, help="ROS domain id override")
    return parser.parse_args()


def main() -> None:
    """Stream Strings between two in-process nodes, then demo the latch."""
    args = parse_args()
    if args.port:
        pyrcllite.init_dds(args.port, args.peer, args.domain)

    listener = None if args.pub_only else pyrcllite.Node("py_listener")
    heard: list[str] = []
    late: list[str] = []  # filled by the latch demo below

    def on_string(msg: pyrcllite.String) -> None:
        heard.append(msg.data)
        print(f"heard: [{msg.data}]")

    def on_latched(msg: pyrcllite.String) -> None:
        late.append(msg.data)

    subscriptions = []
    spin_stop = threading.Event()
    spinner: threading.Thread | None = None
    if listener:
        subscriptions.append(
            listener.create_subscription(pyrcllite.String, args.topic, on_string)
        )

        def spin() -> None:
            while not spin_stop.is_set():
                assert listener is not None
                listener.spin_once(100)

        spinner = threading.Thread(target=spin, daemon=True)
        spinner.start()

    talker = pyrcllite.Node("py_talker")
    pub = talker.create_publisher(pyrcllite.String, args.topic)
    count = 0
    deadline = time.monotonic() + args.duration
    try:
        while time.monotonic() < deadline:
            msg = pyrcllite.String(data=f"hello from pyrcllite #{count}")
            pub.publish(msg)
            print(f"published: [{msg.data}]")
            count += 1
            time.sleep(1.0 / args.rate)
    except KeyboardInterrupt:
        pass

    if not args.pub_only and not args.sub_only and listener:
        # Transient-local latch: publish after the volatile phase, then let a
        # late-joining subscription receive it (map-path hot reload pattern).
        latched = pyrcllite.String(data=f"latched at #{count}")
        latch_pub = talker.create_publisher(
            pyrcllite.String, args.topic + "_latched", pyrcllite.QoS.latched()
        )
        latch_pub.publish(latched)
        subscriptions.append(
            listener.create_subscription(
                pyrcllite.String,
                args.topic + "_latched",
                on_latched,
                pyrcllite.QoS.latched(),
            )
        )
        time.sleep(1.0)
        print(
            f"latch demo: late joiner received {late}"
            + (" (OK)" if late == [latched.data] else " (FAILED)")
        )

    talker.destroy()
    spin_stop.set()
    if spinner is not None:
        spinner.join()
    if listener:
        listener.destroy()
    print(f"done: published {count}, heard {len(heard)}")


if __name__ == "__main__":
    main()
