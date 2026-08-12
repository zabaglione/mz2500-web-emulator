#!/usr/bin/env python3
"""Reject obvious EmuZ/CSCP clean-room boundary violations.

This audit intentionally does not read or compare CSCP source contents. It
checks only repository tracking, publication paths, and distributable names.
"""

from __future__ import annotations

import pathlib
import subprocess
import sys


HERE = pathlib.Path(__file__).resolve().parent.parent
DIST = HERE / "web" / "dist"

FORBIDDEN_TRACKED_PREFIXES = (
    "vendor/common_source_code/",
    "web_emulator/vendor/common_source_code/",
)
FORBIDDEN_PUBLIC_NAMES = ("emuz", "common_source_code", "cscp")


def fail(message: str) -> None:
    print(f"CLEAN-ROOM AUDIT FAIL: {message}")
    raise SystemExit(1)


def tracked_paths() -> list[str]:
    root_result = subprocess.run(
        ["git", "-C", str(HERE), "rev-parse", "--show-toplevel"],
        check=True,
        capture_output=True,
        text=True,
    )
    git_root = root_result.stdout.strip()
    result = subprocess.run(
        ["git", "-C", git_root, "ls-files", "-z"],
        check=True,
        capture_output=True,
    )
    return [path.decode("utf-8", "surrogateescape")
            for path in result.stdout.split(b"\0") if path]


def main() -> None:
    tracked = tracked_paths()
    violations = [
        path for path in tracked
        if path.lower().startswith(FORBIDDEN_TRACKED_PREFIXES)
    ]
    if violations:
        fail(f"forbidden tracked path(s): {violations}")

    bundled_vendor = HERE / "vendor" / "common_source_code"
    if bundled_vendor.exists():
        fail("web_emulator/vendor/common_source_code must not exist")

    if DIST.is_dir():
        named = []
        for path in DIST.rglob("*"):
            relative = path.relative_to(DIST).as_posix().lower()
            if any(token in relative for token in FORBIDDEN_PUBLIC_NAMES):
                named.append(relative)
        if named:
            fail(f"forbidden distributable name(s): {named}")

    print("clean-room tracking boundary OK")
    print("clean-room distributable-name boundary OK")
    print("CLEAN-ROOM AUDIT PASSED")


if __name__ == "__main__":
    main()
