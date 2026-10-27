# Copyright 2026 rcllite contributors
# Licensed under the Apache License, Version 2.0
"""The QoS knobs pyrcllite exposes (the subset rcllite's ``rcl::QoS`` carries)."""

from __future__ import annotations

from dataclasses import dataclass


@dataclass(frozen=True)
class QoS:
    """Reliable volatile keep-last by default, mirroring rmw_qos_profile_default.

    ``transient_local`` latches the most recent sample for late joiners —
    rcllite writers latch exactly one sample regardless of ``depth`` (same
    discipline as the C++ nodes); use it for map paths, camera_info and
    stream-done markers.  Set ``reliable=False`` for lossy sensor streams.
    """

    depth: int = 10
    reliable: bool = True
    transient_local: bool = False

    @staticmethod
    def sensor_data(depth: int = 5) -> QoS:
        """rmw_qos_profile_sensor_data (best-effort keep-last)."""
        return QoS(depth=depth, reliable=False)

    @staticmethod
    def latched(depth: int = 1) -> QoS:
        """Transient-local reliable for latched topics."""
        return QoS(depth=depth, reliable=True, transient_local=True)
