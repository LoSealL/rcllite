# Copyright 2026 rcllite contributors
# Licensed under the Apache License, Version 2.0
"""Publisher / Subscription wrappers over the type-agnostic FFI endpoints."""

from __future__ import annotations

import atexit
import sys
import weakref
from collections.abc import Callable

import tvm_ffi

from .messages import Message

# Endpoints are torn down via atexit while the interpreter is still alive:
# the C++ side holds the callback as a tvm_ffi Function wrapping the Python
# trampoline, and destroying it during interpreter finalization would call
# back into a dying runtime (observed as a hang or segfault at exit).
_TRACKED: weakref.WeakSet[object] = weakref.WeakSet()


@atexit.register
def _destroy_tracked() -> None:
    for endpoint in list(_TRACKED):
        try:
            endpoint.destroy()  # type: ignore[attr-defined]
        except Exception:  # pylint: disable=broad-exception-caught
            pass  # best effort during shutdown


def _destroy_from_del(ffi: tvm_ffi.Module, method: str, handle: int) -> None:
    if sys.is_finalizing():
        return  # the process is dying; crossing FFI now is unsafe (leak it)
    getattr(ffi, method)(handle)


class Publisher:
    """A typed publisher: serializes ``Message`` instances to CDR and publishes.

    ``publish`` may be called from any thread (the C++ writer is thread-safe);
    samples written with no subscriber matched are invisible to volatile
    late joiners — use a ``transient_local`` QoS to latch instead.
    """

    def __init__(
        self, ffi: tvm_ffi.Module, handle: int, msg_type: type[Message], topic: str
    ) -> None:
        self._ffi = ffi
        self._handle = handle
        self._msg_type = msg_type
        self._topic = topic
        _TRACKED.add(self)

    @property
    def topic_name(self) -> str:
        """The ROS topic this publisher writes to."""
        return self._topic

    def publish(self, msg: Message) -> None:
        """Serialize ``msg`` and write it (raises on a failed DDS write)."""
        if not isinstance(msg, self._msg_type):
            raise TypeError(
                f"publisher on {self._topic} expects {self._msg_type.__name__}, "
                f"got {type(msg).__name__}"
            )
        self._ffi.pub_publish(self._handle, msg.serialize())

    def destroy(self) -> None:
        """Tear the endpoint down; idempotent."""
        if getattr(self, "_handle", 0):
            self._ffi.pub_destroy(self._handle)
            self._handle = 0
        _TRACKED.discard(self)

    def __enter__(self) -> Publisher:
        return self

    def __exit__(self, *exc: object) -> None:
        self.destroy()

    def __del__(self) -> None:
        try:
            _destroy_from_del(self._ffi, "pub_destroy", getattr(self, "_handle", 0))
        except Exception:  # pylint: disable=broad-exception-caught
            pass  # interpreter teardown, nothing to do

    def __repr__(self) -> str:
        handle = getattr(self, "_handle", 0)
        topic = getattr(self, "_topic", "?")
        msg = getattr(self, "_msg_type", Message)
        return f"Publisher(handle={handle}, topic={topic!r}, type={msg.__name__})"


class Subscription:
    """A typed subscription feeding decoded messages to a Python callback.

    The callback runs on the thread calling :meth:`pyrcllite.Node.spin` /
    ``spin_once`` of the owning node; samples that fail to decode are dropped
    with a warning (mirroring the C++ subscriptions).
    """

    def __init__(
        self, ffi: tvm_ffi.Module, handle: int, msg_type: type[Message], topic: str
    ) -> None:
        self._ffi = ffi
        self._handle = handle
        self._msg_type = msg_type
        self._topic = topic
        _TRACKED.add(self)

    @property
    def topic_name(self) -> str:
        """The ROS topic this subscription listens on."""
        return self._topic

    def destroy(self) -> None:
        """Stop dispatching and tear the endpoint down; idempotent."""
        if getattr(self, "_handle", 0):
            self._ffi.sub_destroy(self._handle)
            self._handle = 0
        _TRACKED.discard(self)

    def __enter__(self) -> Subscription:
        return self

    def __exit__(self, *exc: object) -> None:
        self.destroy()

    def __del__(self) -> None:
        try:
            _destroy_from_del(self._ffi, "sub_destroy", getattr(self, "_handle", 0))
        except Exception:  # pylint: disable=broad-exception-caught
            pass  # interpreter teardown, nothing to do

    def __repr__(self) -> str:
        handle = getattr(self, "_handle", 0)
        topic = getattr(self, "_topic", "?")
        msg = getattr(self, "_msg_type", Message)
        return f"Subscription(handle={handle}, topic={topic!r}, type={msg.__name__})"


def make_trampoline[T: Message](
    msg_type: type[T], callback: Callable[[T], None]
) -> Callable[[bytes], None]:
    """The raw-CDR-to-message adapter handed to the FFI subscription."""

    def deliver(raw: bytes) -> None:
        try:
            msg = msg_type.from_cdr(raw)
        except ValueError:
            # Malformed payloads must not kill the spin loop; drop them like
            # the C++ subscriptions drop malformed samples.
            print(f"pyrcllite: dropping malformed {msg_type.__name__} sample")
            return
        callback(msg)

    return deliver
