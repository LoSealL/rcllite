# Copyright 2026 rcllite contributors
# Licensed under the Apache License, Version 2.0
"""Codec tests: golden wire bytes, round-trips and malformed-input handling.

The goldens pin the byte layout C++ rcllite / ROS 2 put on the wire (XCDR1
LE, alignment relative to the payload start after the 4-byte header).
"""

# Test names state the scenario.
# pylint: disable=missing-function-docstring

import struct

import pytest

from pyrcllite.messages import (
    WIRE_TYPES,
    CameraInfo,
    Empty,
    Header,
    Image,
    Point,
    Pose,
    PoseStamped,
    Quaternion,
    RegionOfInterest,
    String,
    Time,
    Transform,
    TransformStamped,
    Vector3,
)

_U32 = struct.Struct("<I").pack
_F64 = struct.Struct("<d").pack


def test_empty_golden():
    assert Empty().serialize() == b"\x00\x01\x00\x00"


def test_string_golden():
    # uint32 length (incl. NUL) + "Hi" + NUL
    assert String(data="Hi").serialize() == (b"\x00\x01\x00\x00" + _U32(3) + b"Hi\x00")


def test_pose_stamped_golden():
    # Time(sec=1, ns=2) + frame_id "world" pushes the payload offset to 18,
    # so the first float64 pads to 24 — the alignment case that matters.
    msg = PoseStamped(
        header=Header(stamp=Time(sec=1, nanosec=2), frame_id="world"),
        pose=Pose(position=Point(x=1.5), orientation=Quaternion(w=1.0)),
    )
    assert msg.serialize() == (
        b"\x00\x01\x00\x00"
        + struct.pack("<i", 1)  # sec
        + _U32(2)  # nanosec
        + _U32(6)  # "world\0"
        + b"world\x00"
        + b"\x00" * 6  # pad 18 -> 24 for the first float64
        + struct.pack("<7d", 1.5, 0, 0, 0, 0, 0, 1.0)  # position, quaternion
    )


def test_image_golden():
    msg = Image(
        header=Header(stamp=Time(sec=0, nanosec=0), frame_id=""),
        height=1,
        width=1,
        encoding="rgb8",
        is_bigendian=0,
        step=3,
        data=b"\x01\x02\x03",
    )
    assert msg.serialize() == (
        b"\x00\x01\x00\x00"
        + _U32(0)  # sec
        + _U32(0)  # nanosec
        + _U32(1)  # frame_id ""
        + b"\x00"
        + b"\x00" * 3  # pad 13 -> 16 for height
        + _U32(1)  # height
        + _U32(1)  # width
        + _U32(5)  # "rgb8\0"
        + b"rgb8\x00"
        + b"\x00"  # is_bigendian
        + b"\x00" * 2  # pad 34 -> 36 for step
        + _U32(3)  # step
        + _U32(3)  # data length
        + b"\x01\x02\x03"
    )


def test_camera_info_golden():
    msg = CameraInfo(height=320, width=240, distortion_model="", d=[1.5])
    zeros9 = struct.pack("<9d", *([0.0] * 9))
    zeros12 = struct.pack("<12d", *([0.0] * 12))
    assert msg.serialize() == (
        b"\x00\x01\x00\x00"
        + _U32(0)  # sec
        + _U32(0)  # nanosec
        + _U32(1)  # frame_id ""
        + b"\x00"
        + b"\x00" * 3  # pad 13 -> 16 for height
        + _U32(320)  # height
        + _U32(240)  # width
        + _U32(1)  # distortion_model ""
        + b"\x00"
        + b"\x00" * 3  # pad 29 -> 32 for the D length prefix
        + _U32(1)  # D length
        + b"\x00" * 4  # pad 36 -> 40 for the double element
        + _F64(1.5)
        + zeros9  # K (fixed, no prefix)
        + zeros9  # R
        + zeros12  # P
        + _U32(0)  # binning_x
        + _U32(0)  # binning_y
        + _U32(0) * 4  # roi x/y/height/width
        + b"\x00"  # roi do_rectify
    )


def test_transform_stamped_golden():
    msg = TransformStamped(
        header=Header(frame_id="map"),
        child_frame_id="cam",
        transform=Transform(translation=Vector3(z=2.0)),
    )
    # header(frame_id "map") ends at offset 16; "cam\0" ends at 24, so the
    # float64s need no pad at all — the aligned-16 fast path.
    assert msg.serialize() == (
        b"\x00\x01\x00\x00"
        + _U32(0)  # sec
        + _U32(0)  # nanosec
        + _U32(4)  # frame_id "map\0"
        + b"map\x00"
        + _U32(4)  # child_frame_id "cam\0"
        + b"cam\x00"
        + struct.pack("<7d", 0, 0, 2.0, 0, 0, 0, 1.0)  # translation, rotation
    )


def test_roundtrips():
    pose = PoseStamped(
        header=Header(stamp=Time(sec=3, nanosec=7), frame_id="world"),
        pose=Pose(position=Point(x=1.0, y=2.0, z=3.0)),
    )
    image = Image(
        header=Header(frame_id="cam"),
        height=2,
        width=3,
        encoding="rgb8",
        step=9,
        data=bytes(range(18)),
    )
    camera = CameraInfo(
        height=1080,
        width=1920,
        distortion_model="rational_polynomial",
        d=[-0.1, 0.2],
        roi=RegionOfInterest(x_offset=1, do_rectify=True),
    )
    transform = TransformStamped(
        child_frame_id="base",
        transform=Transform(rotation=Quaternion(w=0.9), translation=Vector3(z=0.5)),
    )
    for msg in (Empty(), String(data="hello"), pose, image, camera, transform):
        assert type(msg).from_cdr(msg.serialize()) == msg, type(msg).__name__


def test_wire_type_names():
    assert set(WIRE_TYPES) == {
        "std_msgs/msg/Empty",
        "std_msgs/msg/String",
        "geometry_msgs/msg/PoseStamped",
        "geometry_msgs/msg/TransformStamped",
        "sensor_msgs/msg/Image",
        "sensor_msgs/msg/CameraInfo",
    }


def test_malformed_payloads_raise():
    good = String(data="ok").serialize()
    with pytest.raises(ValueError):
        String.from_cdr(b"\x00\x02\x00\x00")  # CDR-BE not supported
    with pytest.raises(ValueError):
        String.from_cdr(good[:6])  # truncated after the length prefix
    with pytest.raises(ValueError):
        String.from_cdr(b"")  # not even a header
