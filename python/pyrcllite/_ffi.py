# Copyright 2026 rcllite contributors
# Licensed under the Apache License, Version 2.0
"""Locate and load the rcllite FFI shared library (//src/ffi:rcllite_ffi).

The library is built with Bazel (``bazelisk build //src/ffi:rcllite_ffi``
in this repo, or ``bazelisk build @rcllite//src/ffi:rcllite_ffi`` in a
consuming workspace) and loaded through :mod:`tvm_ffi`.  It is looked up
in this order:

1. ``$RCLLITE_FFI_LIB`` — explicit path to the shared library.
2. ``<workspace>/bazel-bin/src/ffi/`` — the Bazel build output next to the
   checked-out sources (this package lives at ``<workspace>/python/pyrcllite``).
3. the current working directory.
4. Bazel build trees discovered by walking up from this package, the cwd
   and the install origin (a venv-installed package remembers its source
   checkout): ``bazel-out/<config>/bin/src/ffi/`` (a standalone build of
   this repo) or ``bazel-out/<config>/bin/external/rcllite*/src/ffi/`` (an
   ``@rcllite`` build in an enclosing workspace), any config suffix, newest
   file first — so building is the only setup step, wherever the DLL/so
   landed.  A plain ``bin/`` under any walked root is honored the same way
   (manual or genrule installs).

On Windows the library's own directory is registered as a DLL search
directory first, so sibling runtime DLLs (ddsc, libtvm_ffi) resolve.
"""

from __future__ import annotations

import json
import os
import re
import sys
from functools import cache
from pathlib import Path
from urllib.parse import unquote, urlparse

import tvm_ffi


def _install_origin() -> Path | None:
    """This package's source checkout when installed from a local path.

    uv/pip record the origin in ``dist-info/direct_url.json`` next to the
    installed package; a checkout found there joins the bazel-out search
    (covers venv installs whose sources live in another workspace).
    """
    try:
        for meta in Path(__file__).resolve().parent.parent.glob(
            "pyrcllite-*.dist-info/direct_url.json"
        ):
            url = json.loads(meta.read_text(encoding="utf-8")).get("url", "")
            if not url.startswith("file:"):
                continue
            raw = unquote(urlparse(url).path)
            if re.match(r"^/[A-Za-z]:", raw):  # file:///D:/... on Windows
                raw = raw[1:]
            origin = Path(raw)
            if origin.is_dir():
                return origin
    except (OSError, ValueError):
        pass
    return None


def _bazel_out_candidates(package_root: Path, lib_name: str) -> list[Path]:
    """Built copies under bazel-out (or bin/) of any enclosing workspace.

    Walks up from the package root, the cwd and the install origin — the
    installed package may live in a venv beside the workspace rather than
    in the sources — and globs the ffi output of both repo layouts at each
    level, newest file first.
    """
    roots: list[Path] = []
    for start in (package_root, Path.cwd(), _install_origin()):
        if start is not None:
            roots += [start, *start.parents]
    found: list[Path] = []
    for root in dict.fromkeys(roots):
        for pattern in (
            f"bazel-out/*/bin/external/rcllite*/src/ffi/{lib_name}",
            f"bazel-out/*/bin/src/ffi/{lib_name}",
            f"bin/{lib_name}",
        ):
            found += root.glob(pattern)
    found.sort(key=lambda p: p.stat().st_mtime, reverse=True)
    return list(dict.fromkeys(found))


def _candidates() -> list[Path]:
    lib_name = "rcllite_ffi.dll" if sys.platform == "win32" else "librcllite_ffi.so"
    workspace_root = Path(__file__).resolve().parents[2]
    candidates = []
    env = os.environ.get("RCLLITE_FFI_LIB")
    if env:
        candidates.append(Path(env))
    candidates += [
        workspace_root / "bazel-bin" / "src" / "ffi" / lib_name,
        Path.cwd() / lib_name,
    ]
    candidates += [
        p for p in _bazel_out_candidates(workspace_root, lib_name)
        if p not in candidates
    ]
    return candidates


def _load() -> tvm_ffi.Module:
    errors = []
    for candidate in _candidates():
        if not candidate.is_file():
            errors.append(f"not found: {candidate}")
            continue
        if sys.platform == "win32":
            # Resolve sibling DLLs (ddsc, libtvm_ffi) shipped next to the
            # FFI library by Bazel's runfiles copy.
            os.add_dll_directory(str(candidate.parent))
        try:
            return tvm_ffi.load_module(str(candidate))
        except Exception as exc:  # pylint: disable=broad-exception-caught
            # Any load failure (missing export, bad ABI, DLL deps) just moves
            # the search on to the next candidate, keeping the reason.
            errors.append(f"failed to load {candidate}: {exc}")
    raise ImportError(
        "rcllite FFI library not found; build it with "
        "`bazelisk build //src/ffi:rcllite_ffi` (or `@rcllite//src/ffi:rcllite_ffi` "
        "from a consuming workspace) or point RCLLITE_FFI_LIB at it. "
        "Tried: " + "; ".join(errors)
    )


@cache
def ffi() -> tvm_ffi.Module:
    """Return the loaded FFI module (loading it on first call)."""
    return _load()
