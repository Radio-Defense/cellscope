#!/usr/bin/env python3
"""Read or update CellScope's semantic version in all tracked locations."""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
CMAKE = ROOT / "CMakeLists.txt"
README = ROOT / "README.md"
WORKFLOW = ROOT / ".github" / "workflows" / "cmake-multi-platform.yml"
SEMVER = re.compile(r"^\d+\.\d+\.\d+$")
CMAKE_VERSION = re.compile(r"^project\(CellScope VERSION \d+\.\d+\.\d+ LANGUAGES C\)$", re.MULTILINE)
README_VERSION = re.compile(r"(\*\*Current version:\*\* `)[^`]+(`)")
WORKFLOW_VERSION = re.compile(r"v\d+\.\d+\.\d+")


def fail(message: str) -> "NoReturn":
    raise SystemExit(f"version: {message}")


def read_version() -> str:
    match = re.search(r"^project\(CellScope VERSION (\d+\.\d+\.\d+) LANGUAGES C\)$", CMAKE.read_text(), re.MULTILINE)
    if not match:
        fail("cannot read the project version from CMakeLists.txt")
    return match.group(1)


def write_version(version: str) -> None:
    if not SEMVER.fullmatch(version):
        fail(f"invalid semantic version: {version}")

    cmake = CMAKE.read_text()
    cmake, count = CMAKE_VERSION.subn(
        f"project(CellScope VERSION {version} LANGUAGES C)", cmake, count=1
    )
    if count != 1:
        fail("cannot update the project version in CMakeLists.txt")
    CMAKE.write_text(cmake)

    readme = README.read_text()
    readme, count = README_VERSION.subn(rf"\g<1>{version}\g<2>", readme, count=1)
    if count != 1:
        fail("README is missing the '**Current version:** `x.y.z`' marker")
    README.write_text(readme)

    workflow = WORKFLOW.read_text()
    if not WORKFLOW_VERSION.search(workflow):
        fail("workflow is missing its fallback version")
    WORKFLOW.write_text(WORKFLOW_VERSION.sub(f"v{version}", workflow))


def bump(part: str) -> str:
    if part not in {"major", "minor", "patch"}:
        fail("bump must be major, minor, or patch")
    major, minor, patch = (int(piece) for piece in read_version().split("."))
    if part == "major":
        major, minor, patch = major + 1, 0, 0
    elif part == "minor":
        minor, patch = minor + 1, 0
    else:
        patch += 1
    version = f"{major}.{minor}.{patch}"
    write_version(version)
    return version


def main(argv: list[str]) -> int:
    if len(argv) == 2 and argv[1] == "read":
        print(read_version())
        return 0
    if len(argv) == 3 and argv[1] == "bump":
        print(bump(argv[2]))
        return 0
    if len(argv) == 3 and argv[1] == "set":
        write_version(argv[2])
        print(argv[2])
        return 0
    print("Usage: scripts/version.py read|bump <major|minor|patch>|set <x.y.z>", file=sys.stderr)
    return 2


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
