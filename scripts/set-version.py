#!/usr/bin/env python3
"""Rewrite spallocator/version.hpp for a SemVer X.Y.Z (called from shared release-tooling)."""

from __future__ import annotations

import argparse
import pathlib
import re
import sys


HEADER = pathlib.Path(__file__).resolve().parents[1] / "spallocator" / "version.hpp"

MAJOR_RE = re.compile(r"(#define\s+SPALLOCATOR_VERSION_MAJOR\s+)\d+")
MINOR_RE = re.compile(r"(#define\s+SPALLOCATOR_VERSION_MINOR\s+)\d+")
PATCH_RE = re.compile(r"(#define\s+SPALLOCATOR_VERSION_PATCH\s+)\d+")
STRING_RE = re.compile(r'(#define\s+SPALLOCATOR_VERSION_STRING\s+")[^"]+(")')


def parse_semver(version: str) -> tuple[int, int, int]:
    v = version[1:] if version.startswith(("v", "V")) else version
    parts = v.split(".")
    if len(parts) != 3 or not all(p.isdigit() for p in parts):
        raise ValueError(f"expected SemVer X.Y.Z, got {version!r}")
    return int(parts[0]), int(parts[1]), int(parts[2])


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("version", help="SemVer X.Y.Z (optional leading v)")
    args = ap.parse_args()
    major, minor, patch = parse_semver(args.version)
    semver = f"{major}.{minor}.{patch}"

    text = HEADER.read_text(encoding="utf-8")
    text, n1 = MAJOR_RE.subn(rf"\g<1>{major}", text, count=1)
    text, n2 = MINOR_RE.subn(rf"\g<1>{minor}", text, count=1)
    text, n3 = PATCH_RE.subn(rf"\g<1>{patch}", text, count=1)
    text, n4 = STRING_RE.subn(rf"\g<1>{semver}\2", text, count=1)
    if min(n1, n2, n3, n4) != 1:
        print("set-version.py: failed to rewrite all version macros", file=sys.stderr)
        return 1
    HEADER.write_text(text, encoding="utf-8")
    print(f"updated {HEADER} -> {semver}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
