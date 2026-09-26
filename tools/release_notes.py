#!/usr/bin/env python3
"""Extract one GekkoAOT release section from CHANGELOG.md."""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

VERSION_RE = re.compile(r"^\d+\.\d+\.\d+(?:[-.][0-9A-Za-z.-]+)?$")
HEADER_RE = re.compile(
    r"^##\s+(?:\[(?P<bracket>v?\d+\.\d+\.\d+(?:[-.][0-9A-Za-z.-]+)?)\]"
    r"(?:\([^)]*\))?|(?P<plain>v?\d+\.\d+\.\d+(?:[-.][0-9A-Za-z.-]+)?))(?P<tail>.*)$"
)


def normalized_version(value: str) -> str:
    value = value.strip()
    if value.startswith("v"):
        value = value[1:]
    if not VERSION_RE.fullmatch(value):
        raise ValueError(f"invalid semantic version: {value!r}")
    return value


def extract_section(text: str, version: str) -> str:
    lines = text.splitlines()
    start = None
    end = len(lines)

    for index, line in enumerate(lines):
        match = HEADER_RE.match(line)
        if not match:
            continue
        candidate = match.group("bracket") or match.group("plain") or ""
        if candidate.startswith("v"):
            candidate = candidate[1:]
        if start is None and candidate == version:
            start = index + 1
            continue
        if start is not None:
            end = index
            break

    if start is None:
        raise LookupError(f"CHANGELOG.md has no release section for {version}")

    body = "\n".join(lines[start:end]).strip()
    if not body:
        raise LookupError(f"CHANGELOG.md release section for {version} is empty")
    return body + "\n"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--version", required=True, help="release version, with or without leading v")
    parser.add_argument("--changelog", default="CHANGELOG.md")
    parser.add_argument("--output", help="write notes to a file instead of stdout")
    parser.add_argument("--check", action="store_true", help="validate only; do not print notes")
    args = parser.parse_args()

    try:
        version = normalized_version(args.version)
        changelog = Path(args.changelog)
        notes = extract_section(changelog.read_text(encoding="utf-8"), version)
    except (OSError, ValueError, LookupError) as exc:
        print(f"release-notes: {exc}", file=sys.stderr)
        return 2

    if args.check:
        return 0
    if args.output:
        Path(args.output).write_text(notes, encoding="utf-8")
    else:
        sys.stdout.write(notes)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
