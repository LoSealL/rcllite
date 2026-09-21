# Copyright 2026 rcllite contributors
# Licensed under the Apache License, Version 2.0
"""Locate and load the rcllite FFI shared library (//src/ffi:rcllite_ffi).

The library is built with Bazel (``bazelisk build //src/ffi:rcllite_ffi``)
and loaded through :mod:`tvm_ffi`, the same mechanism vila uses.  It is
looked up in this order:

1. ``$RCLLITE_FFI_LIB`` — explicit path to the shared library.
2. ``<workspace>/bazel-bin/src/ffi/`` — the Bazel build output next to the
   checked-out sources (this package lives at ``<workspace>/python/pyrcllite``).
3. the current working directory.

On Windows the library's own directory is registered as a DLL search
directory first, so sibling runtime DLLs (ddsc, libtvm_ffi) resolve.
"""

from __future__ import annotations

import os
import sys
from functools import cache
from pathlib import Path

import tvm_ffi


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
        "`bazelisk build //src/ffi:rcllite_ffi` or point RCLLITE_FFI_LIB at it. "
        "Tried: " + "; ".join(errors)
    )


@cache
def ffi() -> tvm_ffi.Module:
    """Return the loaded FFI module (loading it on first call)."""
    return _load()
