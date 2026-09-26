#!/usr/bin/env python3
"""Build a compact GekkoAOT hot-function manifest from LLVM IR-PGO data.

The manifest intentionally contains only guest function start addresses.  The
DolRecomp adaptive pass consumes it at compile time; no game-specific address is
hard-coded in the compiler or committed to the repository.
"""
from __future__ import annotations

import argparse
import re
import subprocess
from pathlib import Path

FUNC_RE = re.compile(r"(?:^|:)func_([0-9A-Fa-f]{8})_budget(?:$|[._].*)")


def parse_profdata(text: str) -> dict[int, int]:
    result: dict[int, int] = {}
    current: str | None = None
    function_count = 0
    block_max = 0

    def flush() -> None:
        nonlocal current, function_count, block_max
        if current:
            match = FUNC_RE.search(current)
            if match:
                address = int(match.group(1), 16)
                result[address] = max(result.get(address, 0), function_count, block_max)
        current = None
        function_count = 0
        block_max = 0

    for line in text.splitlines():
        header = re.match(r"^  ([^ ].*):$", line)
        if header:
            flush()
            current = header.group(1).strip()
            continue
        if current is None:
            continue
        count = re.match(r"^\s+Function count:\s+(\d+)\s*$", line)
        if count:
            function_count = int(count.group(1))
            continue
        blocks = re.match(r"^\s+Block counts:\s*\[(.*)\]\s*$", line)
        if blocks:
            values = [int(x) for x in re.findall(r"\d+", blocks.group(1))]
            block_max = max(values, default=0)
    flush()
    return result


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--llvm-profdata", type=Path, required=True)
    parser.add_argument("--profile", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--max-functions", type=int, default=32)
    parser.add_argument("--relative-divisor", type=int, default=64)
    args = parser.parse_args()

    proc = subprocess.run(
        [str(args.llvm_profdata), "show", "--all-functions", "--counts", str(args.profile)],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if proc.returncode != 0:
        if proc.stderr:
            print(proc.stderr, end="")
        return proc.returncode

    counts = {address: score for address, score in parse_profdata(proc.stdout).items() if score > 0}
    ranked = sorted(counts.items(), key=lambda item: (-item[1], item[0]))
    if not ranked:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text("# GekkoAOT adaptive PGO hot functions\n", encoding="utf-8")
        print("GEKKOAOT_ADAPTIVE_PGO_V21=0 reason=no-profiled-functions")
        return 0

    maximum = ranked[0][1]
    divisor = max(1, args.relative_divisor)
    threshold = max(1, maximum // divisor)
    selected = [(address, score) for address, score in ranked if score >= threshold]
    selected = selected[: max(1, args.max_functions)]

    lines = [
        "# GekkoAOT adaptive PGO hot functions v1",
        f"# max-score={maximum} threshold={threshold} relative-divisor={divisor}",
        "# address  # score relative-to-hottest",
    ]
    for address, score in selected:
        ratio = score / maximum if maximum else 0.0
        lines.append(f"{address:08X}  # score={score} relative={ratio:.6f}")
    lines.append("")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text("\n".join(lines), encoding="utf-8")

    preview = ",".join(f"{address:08X}:{score}" for address, score in selected[:8])
    print(
        "GEKKOAOT_ADAPTIVE_PGO_V21=1 "
        f"functions={len(selected)} candidates={len(ranked)} max-score={maximum} "
        f"threshold={threshold} preview={preview} manifest=\"{args.output}\""
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
