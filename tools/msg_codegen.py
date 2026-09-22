#!/usr/bin/env python3
# Copyright 2026 rcllite contributors
# Licensed under the Apache License, Version 2.0
"""rcllite message/service code generator.

Converts ROS 2 .msg and .srv definition files into self-contained C++17
headers with:
  * plain structs mirroring the ROS 2 message layout
  * rcl::MessageType<T> specializations carrying the DDS type name
    ("pkg::msg::dds_::Name_" / "pkg::srv::dds_::Name_Request_") and CDR
    serialize/deserialize code byte-compatible with ROS 2 (XCDR1/CDR-LE)

Wire rules (verified against rmw_cyclonedds captures):
  * sequence length prefixes are uint32 and always 4-aligned
  * elements keep their own alignment after the length

Usage:
  msg_codegen.py --package rcl_interfaces --output types --files A.msg B.srv
"""

import argparse
import os
import re
import sys
from dataclasses import dataclass, field

# ros primitive -> (write/read method suffix, wire alignment)
PRIM = {
    "bool": ("bool", 1),
    "byte": ("byte", 1),
    "char": ("char", 1),
    "float32": ("float32", 4),
    "float64": ("float64", 8),
    "int8": ("int8", 1),
    "int16": ("int16", 2),
    "int32": ("int32", 4),
    "int64": ("int64", 8),
    "uint8": ("uint8", 1),
    "uint16": ("uint16", 2),
    "uint32": ("uint32", 4),
    "uint64": ("uint64", 8),
}

CPP_PRIM = {
    "bool": "bool",
    "byte": "uint8_t",
    "char": "uint8_t",
    "float32": "float",
    "float64": "double",
    "int8": "int8_t",
    "int16": "int16_t",
    "int32": "int32_t",
    "int64": "int64_t",
    "uint8": "uint8_t",
    "uint16": "uint16_t",
    "uint32": "uint32_t",
    "uint64": "uint64_t",
}


def split_pkg_type(token: str) -> tuple[str, str]:
    pkg, _, name = token.partition("/")
    if not pkg or not name:
        raise ValueError(f"nested type must be pkg/Name, got: {token}")
    return pkg, name


def snake_to_camel(s: str) -> str:
    # preserve already-camelCase identifiers (capitalize() would lowercase them)
    return "".join(p[:1].upper() + p[1:] for p in s.split("_"))


def camel_to_snake(s: str) -> str:
    return re.sub(r"(?<=[a-z0-9])(?=[A-Z])", "_", s).lower()


@dataclass
class Member:
    ros_type: str  # primitive | "string" | "pkg/Name" (nested)
    name: str = ""
    is_seq: bool = False  # T[] or T[<=N]
    fixed_len: int | None = None  # T[N]
    default: str = ""


@dataclass
class MsgDef:
    package: str
    name: str  # CamelCase
    members: list[Member] = field(default_factory=list)


@dataclass
class SrvDef:
    package: str
    name: str
    request: MsgDef
    response: MsgDef


def parse_msg(text: str, package: str, name: str) -> MsgDef:
    msg = MsgDef(package=package, name=name)
    for raw in text.splitlines():
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        # constants ("uint8 PARAMETER_BOOL=1") carry no data; skipped
        if re.match(r"^[A-Za-z_][A-Za-z0-9_]*\s+[A-Z_][A-Z0-9_]*\s*=", line):
            continue
        m = re.match(
            r"^([A-Za-z_][A-Za-z0-9_/]*(?:<=\d+)?)(\[[^\]]*\])?\s+"
            r"([a-z][A-Za-z0-9_]*)(?:\s+(.*))?$",
            line,
        )
        if not m:
            raise ValueError(f"{package}/{name}: cannot parse line: {line!r}")
        type_tok, bracket, member_name, rest = (
            m.group(1),
            m.group(2),
            m.group(3),
            m.group(4),
        )
        member = Member(ros_type=type_tok, name=member_name)
        if type_tok == "wstring":
            raise ValueError(f"{package}/{name}: wstring is not supported")
        if type_tok.startswith("string<="):
            type_tok = "string"  # bounded strings serialize like strings
            member.ros_type = "string"
        if bracket is not None:
            inner = bracket[1:-1]
            if inner == "":
                member.is_seq = True
            elif inner.startswith("<="):
                member.is_seq = True  # bounded sequence == sequence on the wire
            else:
                member.fixed_len = int(inner)
        if rest:
            member.default = rest.strip()
        # same-package nested types may omit the package prefix (e.g.
        # ByteMultiArray.msg refers to "MultiArrayLayout layout")
        if (
            member.ros_type not in PRIM
            and member.ros_type != "string"
            and "/" not in member.ros_type
        ):
            member.ros_type = f"{package}/{member.ros_type}"
        msg.members.append(member)
    return msg


def parse_srv(text: str, package: str, name: str) -> SrvDef:
    req_s, sep, resp_s = text.partition("---")
    if not sep:
        raise ValueError(f"service {name} lacks '---' separator")
    return SrvDef(
        package=package,
        name=name,
        request=parse_msg(req_s, package, name + "_Request"),
        response=parse_msg(resp_s, package, name + "_Reply"),
    )


# ---------------------------------------------------------------------------
# C++ emission


def cpp_type(member: Member) -> str:
    t = member.ros_type
    if t in PRIM:
        base = CPP_PRIM[t]
    elif t == "string":
        base = "std::string"
    else:
        pkg, nm = split_pkg_type(t)
        base = f"{pkg}::msg::{nm}"
    if member.is_seq:
        return f"std::vector<{base}>"
    if member.fixed_len is not None:
        return f"std::array<{base}, {member.fixed_len}>"
    return base


def default_init(member: Member) -> str:
    t = member.ros_type
    if member.is_seq or member.fixed_len is not None:
        return ""
    d = member.default
    if t == "bool" and d:
        return "true" if d == "true" else "false"
    if t == "string" and d:
        if d.startswith('"') and d.endswith('"'):
            return d
        return f'"{d}"'
    if t in PRIM and d:
        suffix = "u" if CPP_PRIM[t].startswith("u") else ""
        dot = ""
        return f"static_cast<{CPP_PRIM[t]}>({d}{suffix}{dot})"
    if t == "bool":
        return "false"
    if t in ("float32", "float64"):
        return "0.0"
    if t in PRIM:
        return f"static_cast<{CPP_PRIM[t]}>(0)"
    return ""


def emit_serialize(member: Member) -> str:
    t = member.ros_type
    f_ = f"msg.{member.name}"
    if member.is_seq:
        lines = [
            "w.align_to(4);",
            f"w.write_uint32(static_cast<uint32_t>({f_}.size()));",
        ]
        if t in PRIM:
            lines.append(f"for (const auto & e : {f_}) {{ w.write_{PRIM[t][0]}(e); }}")
        elif t == "string":
            lines.append(f"for (const auto & e : {f_}) {{ w.write_string(e); }}")
        else:
            pkg, nm = split_pkg_type(t)
            lines.append(
                f"for (const auto & e : {f_}) {{ {pkg}::msg::{nm}::serialize(e, w); }}"
            )
        return "\n    ".join(lines)
    if member.fixed_len is not None:
        if t in PRIM:
            align = PRIM[t][1]
            elem = CPP_PRIM[t]
            return (
                f"w.align_to({align});\n    "
                f"w.write_bytes({f_}.data(), {f_}.size() * sizeof({elem}));"
            )
        if t == "string":
            return f"for (const auto & e : {f_}) {{ w.write_string(e); }}"
        pkg, nm = split_pkg_type(t)
        return f"for (const auto & e : {f_}) {{ {pkg}::msg::{nm}::serialize(e, w); }}"
    # scalar
    if t in PRIM:
        return f"w.write_{PRIM[t][0]}({f_});"
    if t == "string":
        return f"w.write_string({f_});"
    pkg, nm = split_pkg_type(t)
    return f"{pkg}::msg::{nm}::serialize({f_}, w);"


def emit_deserialize(member: Member) -> str:
    t = member.ros_type
    f_ = f"msg.{member.name}"
    if member.is_seq:
        # own scope (several sequences may declare `n`); 4-aligned length;
        # the `n > remaining` guard rejects corrupt/hostile lengths before
        # the resize could allocate gigabytes (every element needs >= 1 byte)
        guard = "if (n > r.remaining()) { return false; }\n    "
        if t in PRIM:
            return (
                f"{{\n    r.align_to(4);\n    "
                f"const uint32_t n = r.read_uint32();\n    {guard}"
                f"{f_}.clear(); {f_}.resize(n);\n    "
                f"for (uint32_t i = 0; i < n; ++i) {{ "
                f"{f_}[i] = r.read_{PRIM[t][0]}(); }}\n  }}"
            )
        if t == "string":
            return (
                f"{{\n    r.align_to(4);\n    "
                f"const uint32_t n = r.read_uint32();\n    {guard}"
                f"{f_}.clear(); {f_}.reserve(n);\n    "
                f"for (uint32_t i = 0; i < n; ++i) {{ "
                f"{f_}.push_back(r.read_string()); }}\n  }}"
            )
        pkg, nm = split_pkg_type(t)
        return (
            f"{{\n    r.align_to(4);\n    "
            f"const uint32_t n = r.read_uint32();\n    {guard}"
            f"{f_}.clear(); {f_}.resize(n);\n    "
            f"for (auto & e : {f_}) {{ if (!{pkg}::msg::{nm}::deserialize(e, r)) "
            f"{{ return false; }} }}\n  }}"
        )
    if member.fixed_len is not None:
        if t in PRIM:
            return (
                f"r.align_to({PRIM[t][1]});\n    "
                f"r.read_bytes({f_}.data(), {f_}.size() * sizeof({CPP_PRIM[t]}));"
            )
        if t == "string":
            return f"for (auto & e : {f_}) {{ e = r.read_string(); }}"
        pkg, nm = split_pkg_type(t)
        return (
            f"for (auto & e : {f_}) {{ if (!{pkg}::msg::{nm}::deserialize(e, r)) "
            f"{{ return false; }} }}"
        )
    if t in PRIM:
        return f"{f_} = r.read_{PRIM[t][0]}();"
    if t == "string":
        return f"{f_} = r.read_string();"
    pkg, nm = split_pkg_type(t)
    return f"if (!{pkg}::msg::{nm}::deserialize({f_}, r)) {{ return false; }}"


def struct_body(msg: MsgDef, indent: str = "  ") -> str:
    lines = []
    for m in msg.members:
        init = default_init(m)
        init_txt = f" {{{init}}}" if init else ""
        lines.append(f"{indent}{cpp_type(m)} {m.name}{init_txt};")
    if not lines:
        lines.append(f"{indent}char _rcllite_empty_{{0}};  // empty message")
    return "\n".join(lines)


def serialize_method(msg: MsgDef, indent: str) -> str:
    body = "\n".join(f"{indent}  {emit_serialize(m)}" for m in msg.members)
    if not body:
        body = f"{indent}  (void)msg;"
    return (
        f"{indent}static void serialize(const M & msg, rcl::CdrWriter & w)\n"
        f"{indent}{{\n{body}\n{indent}}}"
    )


def deserialize_method(msg: MsgDef, indent: str) -> str:
    body = "\n".join(f"{indent}  {emit_deserialize(m)}" for m in msg.members)
    if not body:
        body = f"{indent}  (void)msg;"
    return (
        f"{indent}static bool deserialize(M & msg, rcl::CdrReader & r)\n"
        f"{indent}{{\n{body}\n{indent}  return true;\n{indent}}}"
    )


def includes_for(
    msgs: list[MsgDef], own_package: str, own_name: str | None = None
) -> str:
    deps = set()
    for msg in msgs:
        for m in msg.members:
            t = m.ros_type
            if t in PRIM or t == "string":
                continue
            pkg, nm = split_pkg_type(t)
            if pkg == own_package and nm == own_name:
                continue  # never include self
            deps.add(f'#include "rcllite_types/{pkg}/{camel_to_snake(nm)}.hpp"')
    return "\n".join(sorted(deps))


def emit_msg_header(msg: MsgDef) -> str:
    pkg, name = msg.package, msg.name
    guard = f"RCLLITE_TYPES__{pkg.upper()}__{camel_to_snake(name).upper()}__"
    inc = includes_for([msg], pkg, name)
    return f"""// Generated by rcllite tools/msg_codegen.py -- DO NOT EDIT
// Source: {pkg}/msg/{camel_to_snake(name)}.msg
#ifndef {guard}
#define {guard}

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "rcllite/cdr.hpp"
#include "rcllite/msg_traits.hpp"
{inc}

namespace {pkg}
{{
namespace msg
{{

struct {name}
{{
  using M = {name};

{struct_body(msg)}

{serialize_method(msg, "  ")}

{deserialize_method(msg, "  ")}
}};

}}  // namespace msg
}}  // namespace {pkg}

namespace rcl
{{

template<>
struct MessageType<{pkg}::msg::{name}>
{{
  static const char * dds_name()
  {{
    return "{pkg}::msg::dds_::{name}_";
  }}
}};

}}  // namespace rcl

#endif  // {guard}
"""


def emit_srv_header(srv: SrvDef) -> str:
    pkg, name = srv.package, srv.name
    guard = f"RCLLITE_TYPES__{pkg.upper()}__{camel_to_snake(name).upper()}__"
    inc = includes_for([srv.request, srv.response], pkg, name)

    def part(part_msg: MsgDef, cpp_name: str) -> str:
        return f"""  struct {cpp_name}
  {{
    using M = {cpp_name};

{struct_body(part_msg, "    ")}

{serialize_method(part_msg, "    ")}

{deserialize_method(part_msg, "    ")}
  }};"""

    return f"""// Generated by rcllite tools/msg_codegen.py -- DO NOT EDIT
// Source: {pkg}/srv/{camel_to_snake(name)}.srv
#ifndef {guard}
#define {guard}

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "rcllite/cdr.hpp"
#include "rcllite/msg_traits.hpp"
{inc}

namespace {pkg}
{{
namespace srv
{{

struct {name}
{{
{part(srv.request, "Request")}

{part(srv.response, "Response")}
}};

}}  // namespace srv
}}  // namespace {pkg}

namespace rcl
{{

template<>
struct MessageType<{pkg}::srv::{name}::Request>
{{
  static const char * dds_name() {{ return "{pkg}::srv::dds_::{name}_Request_"; }}
}};

template<>
struct MessageType<{pkg}::srv::{name}::Response>
{{
  // note: the DDS *type* suffix is _Response_ even though the reply *topic*
  // is named rr/<fqn>Reply — this mirrors the rosidl/rmw conventions
  static const char * dds_name() {{ return "{pkg}::srv::dds_::{name}_Response_"; }}
}};

}}  // namespace rcl

#endif  // {guard}
"""


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description="rcllite .msg/.srv -> C++ generator")
    ap.add_argument("--package", required=True)
    ap.add_argument(
        "--files",
        nargs="+",
        required=True,
        help=".msg/.srv files (classified by extension)",
    )
    ap.add_argument(
        "--output",
        required=False,
        help="output root; headers land in <output>/rcllite_types/<package>/",
    )
    ap.add_argument(
        "--anchor",
        help="execroot path of the FIRST declared output header; when set,"
        " --output is derived by stripping its three path components and the"
        " --output argument is ignored.  Use this from genrules:"
        " $(location <first out>) -- accurate for root and external-repo"
        " packages alike.",
    )
    args = ap.parse_args(argv)

    msgs: list[MsgDef] = []
    srvs: list[SrvDef] = []
    for fp in args.files:
        with open(fp, encoding="utf-8") as f:
            base = os.path.basename(fp)
            if base.endswith(".msg"):
                msgs.append(
                    parse_msg(f.read(), args.package, snake_to_camel(base[:-4]))
                )
            elif base.endswith(".srv"):
                srvs.append(
                    parse_srv(f.read(), args.package, snake_to_camel(base[:-4]))
                )
            else:
                print(f"warning: ignoring unknown file type: {fp}")

    if not args.output and not args.anchor:
        ap.error("one of --output/--anchor is required")
    if args.anchor:
        # a declared output is <pkg-bin-dir>/rcllite_types/<package>/<file>.hpp:
        # strip the three tail components to recover the package bin dir
        args.output = os.path.dirname(
            os.path.dirname(os.path.dirname(args.anchor))
        )
    out_dir = os.path.join(args.output, "rcllite_types", args.package)
    os.makedirs(out_dir, exist_ok=True)

    count = 0
    for msg in msgs:
        path = os.path.join(out_dir, camel_to_snake(msg.name) + ".hpp")
        with open(path, "w", encoding="utf-8", newline="\n") as f:
            f.write(emit_msg_header(msg))
        print("generated " + path)
        count += 1
    for srv in srvs:
        path = os.path.join(out_dir, camel_to_snake(srv.name) + ".hpp")
        with open(path, "w", encoding="utf-8", newline="\n") as f:
            f.write(emit_srv_header(srv))
        print("generated " + path)
        count += 1
    print(f"{args.package}: {count} files generated")
    return 0


if __name__ == "__main__":
    sys.exit(main())
