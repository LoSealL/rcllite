# Copyright 2026 rcllite contributors
# Licensed under the Apache License, Version 2.0
"""Typed CDR codecs for the ROS 2 message types shipped with pyrcllite.

The FFI layer is type-agnostic (raw CDR bytes + ROS type name); decoding and
encoding live here.  The wire rules mirror rcllite's C++ serializer
(``src/rcllite/cdr.hpp``) byte for byte: XCDR1 little-endian, alignment
relative to the byte after the 4-byte encapsulation header, strings as
uint32 length (incl. NUL) + bytes + NUL, sequence length prefixes 4-aligned
with elements at their own natural alignment, fixed-size arrays without a
length prefix.

Only the types consumer nodes actually exchange are provided; extend by
adding a dataclass plus ``_write``/``_read`` following the same rules.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass, field
from typing import ClassVar, TypeVar, cast

_T = TypeVar("_T", bound="Message")

# The CDR primitives and the per-message _write/_read pairs are direct
# translations of the wire rules in the module docstring; method names and
# inline comments carry the field they move.  The structs nest by calling
# each other's _write/_read.
# pylint: disable=missing-function-docstring,missing-class-docstring,protected-access
_CDR_LE_HEADER = b"\x00\x01\x00\x00"


class _CdrWriter:
    """Builds a CDR-LE payload (encapsulation header included)."""

    __slots__ = ("buf",)

    def __init__(self) -> None:
        self.buf = bytearray(_CDR_LE_HEADER)

    def payload(self) -> bytes:
        return bytes(self.buf)

    def _pad(self, alignment: int) -> None:
        self.buf += b"\x00" * (-(len(self.buf) - 4) % alignment)

    def boolean(self, value: bool) -> None:
        self.buf += b"\x01" if value else b"\x00"

    def uint8(self, value: int) -> None:
        self.buf += struct.pack("<B", value)

    def int32(self, value: int) -> None:
        self._pad(4)
        self.buf += struct.pack("<i", value)

    def uint32(self, value: int) -> None:
        self._pad(4)
        self.buf += struct.pack("<I", value)

    def float64(self, value: float) -> None:
        self._pad(8)
        self.buf += struct.pack("<d", value)

    def string(self, value: str) -> None:
        raw = value.encode("utf-8")
        self.uint32(len(raw) + 1)
        self.buf += raw + b"\x00"

    def byte_sequence(self, value: bytes) -> None:
        self._pad(4)
        self.uint32(len(value))
        self.buf += value

    def double_sequence(self, values: list[float]) -> None:
        self._pad(4)
        self.uint32(len(values))
        if values:
            self._pad(8)
            self.buf += struct.pack(f"<{len(values)}d", *values)

    def doubles(self, values: list[float]) -> None:
        if values:
            self._pad(8)
            self.buf += struct.pack(f"<{len(values)}d", *values)


class _CdrReader:
    """Parses a CDR-LE payload (encapsulation header included)."""

    __slots__ = ("data", "pos")

    def __init__(self, data: bytes) -> None:
        if len(data) < 4 or data[0] != 0 or data[1] != 1:
            raise ValueError(
                "not a CDR-LE buffer (expected 00 01 encapsulation header)"
            )
        self.data = data
        self.pos = 4

    def _pad(self, alignment: int) -> None:
        self.pos += -(self.pos - 4) % alignment
        if self.pos > len(self.data):
            raise ValueError("CDR alignment runs past end of buffer")

    def _take(self, size: int) -> bytes:
        if self.pos + size > len(self.data):
            raise ValueError("CDR read past end of buffer")
        chunk = self.data[self.pos : self.pos + size]
        self.pos += size
        return chunk

    def boolean(self) -> bool:
        return self._take(1) != b"\x00"

    def uint8(self) -> int:
        return self._take(1)[0]

    def int32(self) -> int:
        self._pad(4)
        return struct.unpack("<i", self._take(4))[0]

    def uint32(self) -> int:
        self._pad(4)
        return struct.unpack("<I", self._take(4))[0]

    def float64(self) -> float:
        self._pad(8)
        return struct.unpack("<d", self._take(8))[0]

    def string(self) -> str:
        length = self.uint32()
        if length == 0:
            raise ValueError("invalid string length 0 in CDR stream")
        return self._take(length)[:-1].decode("utf-8")

    def byte_sequence(self) -> bytes:
        self._pad(4)
        length = self.uint32()
        return self._take(length)

    def double_sequence(self) -> list[float]:
        self._pad(4)
        count = self.uint32()
        self._pad(8)
        return list(struct.unpack(f"<{count}d", self._take(count * 8)))

    def doubles(self, count: int) -> list[float]:
        self._pad(8)
        return list(struct.unpack(f"<{count}d", self._take(count * 8)))


class Message:
    """Base of the topic-capable message types (those with ``__type_name__``)."""

    __type_name__: ClassVar[str] = ""

    def serialize(self) -> bytes:
        writer = _CdrWriter()
        self._write(writer)
        return writer.payload()

    @classmethod
    def from_cdr(cls: type[_T], data: bytes) -> _T:
        reader = _CdrReader(data)
        return cast(_T, cls._read(reader))  # each subclass returns its own

    def _write(self, writer: _CdrWriter) -> None:
        raise NotImplementedError

    @classmethod
    def _read(cls, reader: _CdrReader) -> Message:
        raise NotImplementedError


# --- std_msgs ----------------------------------------------------------------


@dataclass
class Empty(Message):
    """std_msgs/Empty — zero fields, only the encapsulation header on the wire."""

    __type_name__ = "std_msgs/msg/Empty"

    def _write(self, writer: _CdrWriter) -> None:
        pass

    @classmethod
    def _read(cls, reader: _CdrReader) -> Empty:
        return cls()


@dataclass
class String(Message):
    """std_msgs/String — a single ``data`` string."""

    __type_name__ = "std_msgs/msg/String"

    data: str = ""

    def _write(self, writer: _CdrWriter) -> None:
        writer.string(self.data)

    @classmethod
    def _read(cls, reader: _CdrReader) -> String:
        return cls(data=reader.string())


# --- geometry_msgs -----------------------------------------------------------


@dataclass
class Time:
    """builtin_interfaces/Time — int32 sec + uint32 nanosec."""

    sec: int = 0
    nanosec: int = 0

    def _write(self, writer: _CdrWriter) -> None:
        writer.int32(self.sec)
        writer.uint32(self.nanosec)

    @classmethod
    def _read(cls, reader: _CdrReader) -> Time:
        return cls(sec=reader.int32(), nanosec=reader.uint32())


@dataclass
class Header:
    """std_msgs/Header — stamp + frame_id."""

    stamp: Time = field(default_factory=Time)
    frame_id: str = ""

    def _write(self, writer: _CdrWriter) -> None:
        self.stamp._write(writer)
        writer.string(self.frame_id)

    @classmethod
    def _read(cls, reader: _CdrReader) -> Header:
        return cls(stamp=Time._read(reader), frame_id=reader.string())


@dataclass
class Point:
    x: float = 0.0
    y: float = 0.0
    z: float = 0.0

    def _write(self, writer: _CdrWriter) -> None:
        for v in (self.x, self.y, self.z):
            writer.float64(v)

    @classmethod
    def _read(cls, reader: _CdrReader) -> Point:
        return cls(x=reader.float64(), y=reader.float64(), z=reader.float64())


@dataclass
class Vector3:
    x: float = 0.0
    y: float = 0.0
    z: float = 0.0

    def _write(self, writer: _CdrWriter) -> None:
        for v in (self.x, self.y, self.z):
            writer.float64(v)

    @classmethod
    def _read(cls, reader: _CdrReader) -> Vector3:
        return cls(x=reader.float64(), y=reader.float64(), z=reader.float64())


@dataclass
class Quaternion:
    x: float = 0.0
    y: float = 0.0
    z: float = 0.0
    w: float = 1.0

    def _write(self, writer: _CdrWriter) -> None:
        for v in (self.x, self.y, self.z, self.w):
            writer.float64(v)

    @classmethod
    def _read(cls, reader: _CdrReader) -> Quaternion:
        return cls(
            x=reader.float64(),
            y=reader.float64(),
            z=reader.float64(),
            w=reader.float64(),
        )


@dataclass
class Pose:
    position: Point = field(default_factory=Point)
    orientation: Quaternion = field(default_factory=Quaternion)

    def _write(self, writer: _CdrWriter) -> None:
        self.position._write(writer)
        self.orientation._write(writer)

    @classmethod
    def _read(cls, reader: _CdrReader) -> Pose:
        return cls(position=Point._read(reader), orientation=Quaternion._read(reader))


@dataclass
class PoseStamped(Message):
    """geometry_msgs/PoseStamped — header + pose (c2w in the repo convention)."""

    __type_name__ = "geometry_msgs/msg/PoseStamped"

    header: Header = field(default_factory=Header)
    pose: Pose = field(default_factory=Pose)

    def _write(self, writer: _CdrWriter) -> None:
        self.header._write(writer)
        self.pose._write(writer)

    @classmethod
    def _read(cls, reader: _CdrReader) -> PoseStamped:
        return cls(header=Header._read(reader), pose=Pose._read(reader))


@dataclass
class Transform:
    translation: Vector3 = field(default_factory=Vector3)
    rotation: Quaternion = field(default_factory=Quaternion)

    def _write(self, writer: _CdrWriter) -> None:
        self.translation._write(writer)
        self.rotation._write(writer)

    @classmethod
    def _read(cls, reader: _CdrReader) -> Transform:
        return cls(translation=Vector3._read(reader), rotation=Quaternion._read(reader))


@dataclass
class TransformStamped(Message):
    """geometry_msgs/TransformStamped — header + child_frame_id + transform."""

    __type_name__ = "geometry_msgs/msg/TransformStamped"

    header: Header = field(default_factory=Header)
    child_frame_id: str = ""
    transform: Transform = field(default_factory=Transform)

    def _write(self, writer: _CdrWriter) -> None:
        self.header._write(writer)
        writer.string(self.child_frame_id)
        self.transform._write(writer)

    @classmethod
    def _read(cls, reader: _CdrReader) -> TransformStamped:
        return cls(
            header=Header._read(reader),
            child_frame_id=reader.string(),
            transform=Transform._read(reader),
        )


# --- sensor_msgs -------------------------------------------------------------


@dataclass
class Image(Message):
    """sensor_msgs/Image — header + raster (rgb8 streams ride ``data``)."""

    __type_name__ = "sensor_msgs/msg/Image"

    header: Header = field(default_factory=Header)
    height: int = 0
    width: int = 0
    encoding: str = ""
    is_bigendian: int = 0
    step: int = 0
    data: bytes = b""

    def _write(self, writer: _CdrWriter) -> None:
        self.header._write(writer)
        writer.uint32(self.height)
        writer.uint32(self.width)
        writer.string(self.encoding)
        writer.uint8(self.is_bigendian)
        writer.uint32(self.step)
        writer.byte_sequence(self.data)

    @classmethod
    def _read(cls, reader: _CdrReader) -> Image:
        return cls(
            header=Header._read(reader),
            height=reader.uint32(),
            width=reader.uint32(),
            encoding=reader.string(),
            is_bigendian=reader.uint8(),
            step=reader.uint32(),
            data=reader.byte_sequence(),
        )


@dataclass
class RegionOfInterest:
    x_offset: int = 0
    y_offset: int = 0
    height: int = 0
    width: int = 0
    do_rectify: bool = False

    def _write(self, writer: _CdrWriter) -> None:
        writer.uint32(self.x_offset)
        writer.uint32(self.y_offset)
        writer.uint32(self.height)
        writer.uint32(self.width)
        writer.boolean(self.do_rectify)

    @classmethod
    def _read(cls, reader: _CdrReader) -> RegionOfInterest:
        return cls(
            x_offset=reader.uint32(),
            y_offset=reader.uint32(),
            height=reader.uint32(),
            width=reader.uint32(),
            do_rectify=reader.boolean(),
        )


@dataclass
class CameraInfo(Message):
    """sensor_msgs/CameraInfo — intrinsics latch (K/R/P rows concatenated)."""

    __type_name__ = "sensor_msgs/msg/CameraInfo"

    header: Header = field(default_factory=Header)
    height: int = 0
    width: int = 0
    distortion_model: str = ""
    d: list[float] = field(default_factory=list)
    k: list[float] = field(default_factory=lambda: [0.0] * 9)
    r: list[float] = field(default_factory=lambda: [0.0] * 9)
    p: list[float] = field(default_factory=lambda: [0.0] * 12)
    binning_x: int = 0
    binning_y: int = 0
    roi: RegionOfInterest = field(default_factory=RegionOfInterest)

    def _write(self, writer: _CdrWriter) -> None:
        self.header._write(writer)
        writer.uint32(self.height)
        writer.uint32(self.width)
        writer.string(self.distortion_model)
        writer.double_sequence(self.d)
        writer.doubles(self.k)
        writer.doubles(self.r)
        writer.doubles(self.p)
        writer.uint32(self.binning_x)
        writer.uint32(self.binning_y)
        self.roi._write(writer)

    @classmethod
    def _read(cls, reader: _CdrReader) -> CameraInfo:
        return cls(
            header=Header._read(reader),
            height=reader.uint32(),
            width=reader.uint32(),
            distortion_model=reader.string(),
            d=reader.double_sequence(),
            k=reader.doubles(9),
            r=reader.doubles(9),
            p=reader.doubles(12),
            binning_x=reader.uint32(),
            binning_y=reader.uint32(),
            roi=RegionOfInterest._read(reader),
        )


WIRE_TYPES: dict[str, type[Message]] = {
    cls.__type_name__: cls
    for cls in (Empty, String, PoseStamped, TransformStamped, Image, CameraInfo)
}
"""Topic-capable codecs by ROS type name (``create_*`` validates against it)."""
