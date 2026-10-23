# Copyright 2026 rcllite contributors
# Licensed under the Apache License, Version 2.0
"""Node pub/sub API wiring tests against a fake FFI (no DLL / DDS needed)."""

import gc

# Test names state the scenario; fixtures arrive as pytest-injected arguments.
# pylint: disable=missing-function-docstring,redefined-outer-name,unused-argument
from typing import Any, cast

import pytest

import pyrcllite
from pyrcllite import messages
from pyrcllite import node as node_mod
from pyrcllite.pubsub import make_trampoline


class FakeFFI:
    """Records FFI calls; hands out fixed handles."""

    def __init__(self):
        self.calls = []
        self.sub_callback = None  # captured by sub_create

    def node_create(self, name, namespace_):
        self.calls.append(("node_create", name, namespace_))
        return 1

    def node_name(self, _h):
        return "n"

    def node_namespace(self, _h):
        return "/"

    def node_fully_qualified_name(self, _h):
        return "/n"

    def node_spin_once(self, _h, _t):
        return False

    def node_cancel(self, _h):
        self.calls.append(("node_cancel",))

    def node_destroy(self, _h):
        self.calls.append(("node_destroy",))

    def init_dds(self, port, peers, domain, interface):
        self.calls.append(("init_dds", port, peers, domain, interface))

    def pub_create(self, node, topic, type_name, depth, reliable, transient_local):
        self.calls.append((
            "pub_create",
            node,
            topic,
            type_name,
            depth,
            reliable,
            transient_local,
        ))
        return 11

    def pub_publish(self, handle, payload):
        self.calls.append(("pub_publish", handle, bytes(payload)))

    def pub_destroy(self, handle):
        self.calls.append(("pub_destroy", handle))

    def sub_create(
        self, node, topic, type_name, depth, reliable, transient_local, callback
    ):
        self.calls.append((
            "sub_create",
            node,
            topic,
            type_name,
            depth,
            reliable,
            transient_local,
        ))
        self.sub_callback = callback
        return 12

    def sub_destroy(self, handle):
        self.calls.append(("sub_destroy", handle))


@pytest.fixture
def fake_ffi(monkeypatch):
    fake = FakeFFI()
    monkeypatch.setattr(node_mod, "ffi", lambda: fake)
    return fake


def test_create_publisher_passes_type_and_qos(fake_ffi):
    node = pyrcllite.Node("n")
    pub = node.create_publisher(
        pyrcllite.String, "/map", pyrcllite.QoS(depth=2, transient_local=True)
    )
    assert fake_ffi.calls == [
        ("node_create", "n", "/"),
        ("pub_create", 1, "/map", "std_msgs/msg/String", 2, True, True),
    ]
    assert pub.topic_name == "/map"


def test_create_subscription_defaults(fake_ffi):
    node = pyrcllite.Node("n")
    sub = node.create_subscription(pyrcllite.Empty, "/stream/done", lambda _m: None)
    assert fake_ffi.calls[-1][:6] == (
        "sub_create",
        1,
        "/stream/done",
        "std_msgs/msg/Empty",
        10,
        True,
    )
    sub.destroy()


def test_wrong_message_type_fails_loudly(fake_ffi):
    node = pyrcllite.Node("n")
    for bad in (str, "std_msgs/msg/String", messages.Header, None):
        bad_type = cast(Any, bad)
        with pytest.raises(TypeError):
            node.create_publisher(bad_type, "/t")
        with pytest.raises(TypeError):
            node.create_subscription(bad_type, "/t", lambda _m: None)


def test_publisher_publishes_serialized(fake_ffi):
    node = pyrcllite.Node("n")
    pub = node.create_publisher(pyrcllite.String, "/t")
    pub.publish(pyrcllite.String(data="hi"))
    assert fake_ffi.calls[-1] == (
        "pub_publish",
        11,
        b"\x00\x01\x00\x00" + (3).to_bytes(4, "little") + b"hi\x00",
    )
    with pytest.raises(TypeError):
        pub.publish(pyrcllite.Empty())  # wrong message type for this publisher


def test_trampoline_decodes_and_drops_malformed(fake_ffi):
    node = pyrcllite.Node("n")
    got = []
    sub = node.create_subscription(pyrcllite.String, "/t", got.append)
    fake_ffi.sub_callback(pyrcllite.String(data="ok").serialize())
    assert got == [pyrcllite.String(data="ok")]
    fake_ffi.sub_callback(b"\x00\x01\x00\x00\x99")  # malformed: dropped, no raise
    assert got == [pyrcllite.String(data="ok")]
    sub.destroy()


def test_destroy_is_idempotent(fake_ffi):
    node = pyrcllite.Node("n")
    pub = node.create_publisher(pyrcllite.String, "/t")
    sub = node.create_subscription(pyrcllite.String, "/t", lambda _m: None)
    pub.destroy()
    pub.destroy()
    sub.destroy()
    sub.destroy()
    assert fake_ffi.calls.count(("pub_destroy", 11)) == 1
    assert fake_ffi.calls.count(("sub_destroy", 12)) == 1
    node.destroy()


def test_init_dds_argument_mapping(fake_ffi):
    pyrcllite.init_dds()
    assert fake_ffi.calls == [("init_dds", 0, [], -1, "127.0.0.1")]
    pyrcllite.init_dds(tcp_port=7606, peers=["127.0.0.1:7601"], domain_id=42)
    assert fake_ffi.calls[-1] == (
        "init_dds",
        7606,
        ["127.0.0.1:7601"],
        42,
        "127.0.0.1",
    )
    pyrcllite.init_dds(tcp_port=7606, interface="192.168.3.101")
    assert fake_ffi.calls[-1] == ("init_dds", 7606, [], -1, "192.168.3.101")


def test_node_owns_endpoints(fake_ffi):
    # rclpy semantics: dropping the return value of create_* must not tear
    # the DDS endpoint down (a trace of exactly this bug showed the reader
    # deleted 44us after creation, silently never matching any writer).
    node = pyrcllite.Node("n")
    node.create_publisher(pyrcllite.String, "/t")
    node.create_subscription(pyrcllite.String, "/t", lambda _m: None)
    gc.collect()
    destroys = [c for c in fake_ffi.calls if c[0] in ("pub_destroy", "sub_destroy")]
    assert destroys == []
    # the ownership itself is under test, so reach into the node
    assert len(node._endpoints) == 2  # pylint: disable=protected-access
    node.destroy()
    # After node.destroy the endpoints are already gone C++-side; their
    # handles are stale and the fake records no extra destroys.
    assert not [c for c in fake_ffi.calls if c[0] in ("pub_destroy", "sub_destroy")]


def test_trampoline_requires_callable(fake_ffi):
    node = pyrcllite.Node("n")
    with pytest.raises(TypeError):
        node.create_subscription(pyrcllite.String, "/t", cast(Any, "not-callable"))


def test_make_trampoline_is_the_bound_adapter():
    # The trampoline handed to the FFI is exactly this adapter (typed in one
    # place instead of per-call sites).
    calls = []
    tramp = make_trampoline(pyrcllite.Empty, calls.append)
    tramp(pyrcllite.Empty().serialize())
    assert calls == [pyrcllite.Empty()]
