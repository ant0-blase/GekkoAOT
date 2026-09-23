#!/usr/bin/env python3
"""Cross-game PowerPC optimization database for GekkoAOT.

The database deliberately stores normalized code fingerprints and aggregate
optimization evidence, never raw profile counters or host machine code.  PGO
from one title is therefore used only as a prior for structurally similar PPC
code in another title; game-specific PGO always remains authoritative.  v0.3.6
can also learn from sampled guest-PC dispatch profiles emitted by RecompCore.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
import re
import struct
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable

SCHEMA = 1
DEFAULT_SIGNATURE_CHUNK = 512
DEFAULT_COMPILE_CHUNK = 128
MAX_LEARNED = 128
FUNC_RE = re.compile(r"(?:^|:)func_([0-9A-Fa-f]{8})_budget(?:$|[._].*)")


@dataclass(frozen=True)
class Section:
    address: int
    data: bytes

    @property
    def end(self) -> int:
        return self.address + len(self.data)


@dataclass(frozen=True)
class Region:
    start: int
    end: int
    words: tuple[int, ...]

    @property
    def instructions(self) -> int:
        return len(self.words)


def u32be(data: bytes, offset: int) -> int:
    return struct.unpack_from(">I", data, offset)[0]


def parse_dol(path: Path) -> tuple[list[Section], int]:
    blob = path.read_bytes()
    if len(blob) < 0xE4:
        raise ValueError(f"{path} is too small to be a DOL")
    offsets = [u32be(blob, 0x00 + 4 * i) for i in range(7)]
    addrs = [u32be(blob, 0x48 + 4 * i) for i in range(7)]
    sizes = [u32be(blob, 0x90 + 4 * i) for i in range(7)]
    sections: list[Section] = []
    for off, addr, size in zip(offsets, addrs, sizes):
        if not off or not addr or not size:
            continue
        if off + size > len(blob):
            raise ValueError(f"text section at 0x{addr:08X} exceeds DOL size")
        size &= ~3
        if size:
            sections.append(Section(addr, blob[off : off + size]))
    if not sections:
        raise ValueError("DOL has no executable text sections")
    entry = u32be(blob, 0xE0)
    return sections, entry


def code_address(sections: Iterable[Section], address: int) -> bool:
    return any(s.address <= address < s.end and ((address - s.address) & 3) == 0 for s in sections)


def sign_extend(value: int, bits: int) -> int:
    sign = 1 << (bits - 1)
    return (value ^ sign) - sign


def direct_branch_target(word: int, address: int) -> tuple[int | None, bool, bool]:
    op = word >> 26
    if op == 18:  # b / bl
        disp = sign_extend(word & 0x03FFFFFC, 26)
        aa = bool((word >> 1) & 1)
        lk = bool(word & 1)
        target = disp & 0xFFFFFFFF if aa else (address + disp) & 0xFFFFFFFF
        return target, lk, aa
    if op == 16:  # bc / bcl
        disp = sign_extend(word & 0x0000FFFC, 16)
        aa = bool((word >> 1) & 1)
        lk = bool(word & 1)
        target = disp & 0xFFFFFFFF if aa else (address + disp) & 0xFFFFFFFF
        return target, lk, aa
    return None, False, False


def is_blr(word: int) -> bool:
    # bclr BO=20,BI=0,BH=0,LK=0 -> canonical blr.  DolRecomp uses the same
    # return boundary for partitioning normal compiled functions.
    return word == 0x4E800020


def collect_partition_points(sections: list[Section], entry: int) -> list[int]:
    points: set[int] = set()
    if code_address(sections, entry):
        points.add(entry)
    for section in sections:
        for off in range(0, len(section.data), 4):
            address = section.address + off
            word = u32be(section.data, off)
            target, lk, _ = direct_branch_target(word, address)
            if lk and target is not None and code_address(sections, target):
                points.add(target)
            if is_blr(word) and code_address(sections, address + 4):
                points.add(address + 4)
    return sorted(points)


def build_regions(sections: list[Section], entry: int, chunk_instructions: int) -> list[Region]:
    points = collect_partition_points(sections, entry)
    regions: list[Region] = []
    for section in sections:
        start = section.address
        while start < section.end:
            cap = min(section.end, start + chunk_instructions * 4)
            end = cap
            for point in points:
                if start < point < end:
                    end = point
                    break
            if end <= start:
                end = min(section.end, start + 4)
            offset = start - section.address
            raw = section.data[offset : offset + (end - start)]
            words = tuple(struct.unpack(f">{len(raw) // 4}I", raw))
            regions.append(Region(start, end, words))
            start = end
    return regions


def immediate_class(value: int) -> str:
    signed = sign_extend(value & 0xFFFF, 16)
    if signed == 0:
        return "z"
    if -16 <= signed <= 16:
        return "s"
    if -256 <= signed <= 256:
        return "m"
    return "l"


def opcode_token(word: int, address: int) -> str:
    op = word >> 26
    if op in (18, 16):
        target, lk, aa = direct_branch_target(word, address)
        direction = "x"
        if target is not None:
            direction = "b" if target < address else "f"
        if op == 16:
            bo = (word >> 21) & 0x1F
            # Retain BO class because loop/condition structure matters, but
            # intentionally drop BI/register identities.
            return f"bc:{bo:02x}:{int(lk)}:{int(aa)}:{direction}"
        return f"b:{int(lk)}:{int(aa)}:{direction}"
    if op in (4, 19, 31, 59, 63):
        xo = (word >> 1) & 0x3FF
        return f"x:{op:02d}:{xo:03x}"
    if 32 <= op <= 55:
        ra_zero = 1 if ((word >> 16) & 0x1F) == 0 else 0
        return f"mem:{op:02d}:{ra_zero}"
    if op in (7, 8, 10, 11, 12, 13, 14, 15, 24, 25, 26, 27, 28, 29):
        return f"imm:{op:02d}"
    return f"op:{op:02d}"


def features(region: Region) -> dict[str, int]:
    result = {
        "instructions": region.instructions,
        "branches": 0,
        "calls": 0,
        "returns": 0,
        "back_edges": 0,
        "loads": 0,
        "stores": 0,
        "fp_ps": 0,
    }
    for i, word in enumerate(region.words):
        address = region.start + i * 4
        op = word >> 26
        if op in (16, 18):
            result["branches"] += 1
            target, lk, _ = direct_branch_target(word, address)
            if lk:
                result["calls"] += 1
            if target is not None and target < address:
                result["back_edges"] += 1
        if is_blr(word):
            result["returns"] += 1
        if 32 <= op <= 47:
            # PPC scalar load/store primary opcodes are interleaved; odd/even
            # is not exact for every instruction, so use the canonical ranges.
            if op in (32, 33, 34, 35, 40, 41, 42, 43, 46):
                result["loads"] += 1
            else:
                result["stores"] += 1
        elif 48 <= op <= 55:
            if op in (48, 49, 50, 51):
                result["loads"] += 1
            else:
                result["stores"] += 1
        if op in (4, 59, 63):
            result["fp_ps"] += 1
    return result


def normalized_tokens(region: Region) -> list[str]:
    return [opcode_token(word, region.start + i * 4) for i, word in enumerate(region.words)]


def fingerprint(region: Region) -> dict[str, object]:
    tokens = normalized_tokens(region)
    exact = hashlib.sha256("\n".join(tokens).encode()).hexdigest()
    weighted: dict[str, int] = {}
    for token in tokens:
        weighted["u:" + token] = weighted.get("u:" + token, 0) + 1
    for n in (2, 3):
        for i in range(0, max(0, len(tokens) - n + 1)):
            gram = "|".join(tokens[i : i + n])
            weighted[f"g{n}:{gram}"] = weighted.get(f"g{n}:{gram}", 0) + 2
    accum = [0] * 64
    for key, weight in weighted.items():
        value = int.from_bytes(hashlib.blake2b(key.encode(), digest_size=8).digest(), "big")
        for bit in range(64):
            accum[bit] += weight if (value >> bit) & 1 else -weight
    simhash = 0
    for bit, score in enumerate(accum):
        if score >= 0:
            simhash |= 1 << bit
    return {
        "fingerprint": exact,
        "simhash": f"{simhash:016x}",
        "features": features(region),
        "span_instructions": region.instructions,
    }


def empty_db() -> dict[str, object]:
    return {
        "schema": SCHEMA,
        "kind": "gekkoaot-crossgame-powerpc",
        "signature_chunk_instructions": DEFAULT_SIGNATURE_CHUNK,
        "entries": [],
    }


def load_db(path: Path | None) -> dict[str, object]:
    if path is None or not path.exists():
        return empty_db()
    data = json.loads(path.read_text(encoding="utf-8"))
    if data.get("schema") != SCHEMA:
        raise ValueError(f"unsupported CrossGameDB schema in {path}")
    if not isinstance(data.get("entries"), list):
        raise ValueError(f"invalid CrossGameDB entries in {path}")
    return data


def save_db(path: Path, db: dict[str, object]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temp = path.with_suffix(path.suffix + f".tmp.{os.getpid()}")
    temp.write_text(json.dumps(db, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    temp.replace(path)


def merge_entries(databases: Iterable[dict[str, object]]) -> list[dict[str, object]]:
    merged: dict[str, dict[str, object]] = {}
    for db in databases:
        for raw in db.get("entries", []):
            if not isinstance(raw, dict) or not raw.get("fingerprint"):
                continue
            fp = str(raw["fingerprint"])
            dst = merged.get(fp)
            if dst is None:
                dst = json.loads(json.dumps(raw))
                dst["games"] = sorted(set(dst.get("games", [])))
                merged[fp] = dst
                continue
            # Databases are cumulative snapshots.  Use maxima rather than
            # addition so exporting the same local snapshot twice is idempotent
            # and cannot artificially inflate confidence.
            dst["samples"] = max(int(dst.get("samples", 0)), int(raw.get("samples", 0)))
            dst["hot_weight"] = max(float(dst.get("hot_weight", 0.0)), float(raw.get("hot_weight", 0.0)))
            dst["games"] = sorted(set(dst.get("games", [])) | set(raw.get("games", [])))
            dst["confidence"] = confidence_for(dst)
            if policy_rank(str(raw.get("policy", "balanced"))) > policy_rank(str(dst.get("policy", "balanced"))):
                dst["policy"] = raw.get("policy", "balanced")
    return sorted(merged.values(), key=lambda x: str(x.get("fingerprint", "")))


def policy_rank(value: str) -> int:
    return {"off": 0, "balanced": 1, "aggressive": 2}.get(value, 0)


def confidence_for(entry: dict[str, object]) -> float:
    samples = max(0, int(entry.get("samples", 0)))
    games = len(set(entry.get("games", [])))
    # Exact normalized code seen hot in multiple games rapidly becomes a strong
    # prior, but never reaches 1.0: game-specific PGO must stay authoritative.
    return round(min(0.98, 0.55 + 0.07 * min(samples, 4) + 0.08 * min(games, 3)), 4)


def feature_similarity(a: dict[str, int], b: dict[str, int]) -> float:
    keys = ("branches", "calls", "returns", "back_edges", "loads", "stores", "fp_ps")
    na = max(1, int(a.get("instructions", 1)))
    nb = max(1, int(b.get("instructions", 1)))
    diff = 0.0
    for key in keys:
        diff += abs(float(a.get(key, 0)) / na - float(b.get(key, 0)) / nb)
    return max(0.0, 1.0 - diff / len(keys) * 3.0)


def entry_match(sig: dict[str, object], entry: dict[str, object]) -> tuple[float, str] | None:
    if sig["fingerprint"] == entry.get("fingerprint"):
        return float(entry.get("confidence", 0.75)), "exact"
    try:
        left = int(str(sig["simhash"]), 16)
        right = int(str(entry.get("simhash", "0")), 16)
    except ValueError:
        return None
    distance = (left ^ right).bit_count()
    if distance > 6:
        return None
    size_a = int(sig.get("span_instructions", 0))
    size_b = int(entry.get("span_instructions", 0))
    if not size_a or not size_b:
        return None
    ratio = min(size_a, size_b) / max(size_a, size_b)
    if ratio < 0.78:
        return None
    structural = feature_similarity(sig.get("features", {}), entry.get("features", {}))
    sim = 0.70 * (1.0 - distance / 64.0) + 0.20 * ratio + 0.10 * structural
    if sim < 0.91:
        return None
    confidence = min(float(entry.get("confidence", 0.65)), sim) * 0.92
    if confidence < 0.68:
        return None
    return confidence, "fuzzy"


def write_env(path: Path, values: dict[str, str | int | float]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    text = "".join(f"{key}={value}\n" for key, value in values.items())
    path.write_text(text, encoding="utf-8")


def command_match(args: argparse.Namespace) -> int:
    sections, entry = parse_dol(args.dol)
    repo = load_db(args.db)
    local = load_db(args.local) if args.local else empty_db()
    entries = merge_entries((repo, local))
    if args.exclude_game:
        # CrossGameDB must be genuinely cross-game.  v1 records are cumulative,
        # so a record touched by the current title cannot be deconvolved safely;
        # exclude it completely instead of feeding a title's own PGO back to it.
        entries = [
            entry_obj
            for entry_obj in entries
            if args.exclude_game not in {str(game) for game in entry_obj.get("games", [])}
        ]
    sig_chunk = int(repo.get("signature_chunk_instructions", DEFAULT_SIGNATURE_CHUNK))
    signature_regions = build_regions(sections, entry, sig_chunk)
    matched: list[tuple[Region, float, str, dict[str, object]]] = []
    for region in signature_regions:
        if region.instructions < 8:
            continue
        sig = fingerprint(region)
        best: tuple[float, str, dict[str, object]] | None = None
        for db_entry in entries:
            candidate = entry_match(sig, db_entry)
            if candidate is None:
                continue
            conf, kind = candidate
            if best is None or conf > best[0]:
                best = (conf, kind, db_entry)
        if best is not None:
            matched.append((region, best[0], best[1], best[2]))

    exact = sum(1 for _, _, kind, _ in matched if kind == "exact")
    fuzzy = len(matched) - exact
    if matched:
        weighted = sum(conf * region.instructions for region, conf, _, _ in matched)
        matched_insns = sum(region.instructions for region, _, _, _ in matched)
        confidence = weighted / max(1, matched_insns)
        covered = matched_insns / max(1, sum(r.instructions for r in signature_regions))
    else:
        confidence = 0.0
        covered = 0.0

    aggressive_votes = sum(1 for _, conf, _, e in matched if conf >= 0.82 and e.get("policy") == "aggressive")
    if exact >= 2 and (confidence >= 0.82 or covered >= 0.08):
        policy = "aggressive" if aggressive_votes or exact >= 4 else "balanced"
    elif exact >= 1 or fuzzy >= 2:
        policy = "balanced"
    else:
        policy = "off"

    compile_chunk = args.compile_chunk
    if policy == "balanced" and not args.chunk_locked:
        compile_chunk = max(compile_chunk, 256)
    elif policy == "aggressive" and not args.chunk_locked:
        compile_chunk = max(compile_chunk, 512)

    compile_regions = build_regions(sections, entry, compile_chunk)
    hot_intervals = [(r.start, r.end, conf) for r, conf, _, _ in matched]
    ordered: list[tuple[float, int]] = []
    for region in compile_regions:
        best_conf = 0.0
        for start, end, conf in hot_intervals:
            if region.start < end and region.end > start:
                best_conf = max(best_conf, conf)
        if best_conf:
            ordered.append((best_conf, region.start))
    ordered.sort(key=lambda item: (-item[0], item[1]))

    args.symbol_order.parent.mkdir(parents=True, exist_ok=True)
    args.symbol_order.write_text(
        "".join(f"func_{address:08X}_budget\n" for _, address in ordered), encoding="utf-8"
    )
    result_material = (
        f"policy={policy}|chunk={compile_chunk}|exact={exact}|fuzzy={fuzzy}|"
        f"confidence={confidence:.4f}|exclude={args.exclude_game}|symbols="
        + ",".join(f"{a:08X}" for _, a in ordered)
    )
    match_fingerprint = hashlib.sha256(result_material.encode()).hexdigest()[:24]
    write_env(
        args.env,
        {
            "GEKKOAOT_CROSSGAME_POLICY": policy,
            "GEKKOAOT_CROSSGAME_MATCHES": len(matched),
            "GEKKOAOT_CROSSGAME_EXACT": exact,
            "GEKKOAOT_CROSSGAME_FUZZY": fuzzy,
            "GEKKOAOT_CROSSGAME_CONFIDENCE": f"{confidence:.4f}",
            "GEKKOAOT_CROSSGAME_COVERAGE": f"{covered:.4f}",
            "GEKKOAOT_CROSSGAME_CHUNK": compile_chunk,
            "GEKKOAOT_CROSSGAME_FINGERPRINT": match_fingerprint,
        },
    )
    source_games = sorted(
        {
            str(game)
            for _, _, _, entry_obj in matched
            for game in entry_obj.get("games", [])
            if str(game)
        }
    )
    print(
        f"CrossGameDB: policy={policy} matches={len(matched)} exact={exact} fuzzy={fuzzy} "
        f"confidence={confidence:.1%} coverage={covered:.1%} chunk={compile_chunk} "
        f"source-games={len(source_games)}"
    )
    return 0


def parse_dispatch_profile(path: Path | None) -> dict[int, int]:
    """Read `pc,samples` emitted by StaticRecompCore during one PGO session."""
    result: dict[int, int] = {}
    if path is None or not path.exists():
        return result
    for lineno, raw in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        line = raw.strip()
        if not line or line.startswith("#") or line.lower() == "pc,samples":
            continue
        try:
            pc_text, samples_text = (part.strip() for part in line.split(",", 1))
            pc = int(pc_text, 16)
            samples = int(samples_text, 10)
        except (ValueError, TypeError):
            print(f"CrossGameDB: ignoring malformed dispatch-profile line {lineno}: {raw!r}", file=sys.stderr)
            continue
        if samples > 0:
            result[pc] = result.get(pc, 0) + samples
    return result


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


def find_region(regions: list[Region], address: int) -> Region | None:
    for region in regions:
        if region.start == address:
            return region
    for region in regions:
        if region.start <= address < region.end:
            return region
    return None


def command_learn(args: argparse.Namespace) -> int:
    sections, entry = parse_dol(args.dol)

    # Prefer the chassis-level guest-PC profile.  Unlike LLVM instrumentation it
    # still sees hot PPC regions when DolRecomp reuses already-compiled AOT
    # objects from the incremental cache.  llvm-profdata remains a compatibility
    # fallback for older runtimes.
    counts = parse_dispatch_profile(args.dispatch_profile)
    source_kind = "dispatch" if counts else "llvm"
    if counts:
        print(
            f"CrossGameDB: runtime PPC dispatch profile sites={len(counts)} "
            f"samples={sum(counts.values())}"
        )
    else:
        proc = subprocess.run(
            [str(args.llvm_profdata), "show", "--all-functions", "--counts", str(args.profile)],
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        if proc.returncode != 0:
            print(proc.stderr, file=sys.stderr, end="")
            raise SystemExit(proc.returncode)
        counts = parse_profdata(proc.stdout)
        counts = {addr: score for addr, score in counts.items() if score > 0}
        if counts:
            print(f"CrossGameDB: LLVM profile fallback counters={len(counts)}")

    if not counts:
        print(
            "CrossGameDB: no executed guest-PPC hotspots in dispatch profile or LLVM profile",
            file=sys.stderr,
        )
        return 1

    db = load_db(args.local)
    signature_chunk = int(db.get("signature_chunk_instructions", args.chunk))
    db["signature_chunk_instructions"] = signature_chunk
    regions = build_regions(sections, entry, signature_chunk)

    # Many hot dispatch PCs can fall in the same normalized 512-instruction
    # signature region.  Fold them first so one session contributes at most one
    # confidence sample per region and short loops do not inflate confidence.
    region_scores: dict[int, int] = {}
    regions_by_start = {region.start: region for region in regions}
    for address, score in counts.items():
        if score <= 0:
            continue
        region = find_region(regions, address)
        if region is None or region.instructions < 8:
            continue
        region_scores[region.start] = region_scores.get(region.start, 0) + score

    if not region_scores:
        print("CrossGameDB: hotspot PCs did not map to executable DOL regions", file=sys.stderr)
        return 1

    ranked = sorted(region_scores.items(), key=lambda item: item[1], reverse=True)
    total = sum(score for _, score in ranked)
    selected: list[tuple[int, int]] = []
    cumulative = 0
    floor = max(1, ranked[0][1] // 1000)
    for address, score in ranked:
        if len(selected) >= MAX_LEARNED:
            break
        if score < floor and selected:
            break
        selected.append((address, score))
        cumulative += score
        if len(selected) >= 16 and cumulative >= total * 0.90:
            break

    # Collapse equal normalized fingerprints inside the same session as well.
    # Repeated copies of the same code pattern are useful hotness evidence, but
    # they are still one independent training session for confidence purposes.
    selected_patterns: dict[str, dict[str, object]] = {}
    for address, score in selected:
        sig = fingerprint(regions_by_start[address])
        fp = str(sig["fingerprint"])
        item = selected_patterns.get(fp)
        if item is None:
            selected_patterns[fp] = {"signature": sig, "score": score}
        else:
            item["score"] = int(item["score"]) + score

    by_fp = {str(e.get("fingerprint")): e for e in db.get("entries", []) if isinstance(e, dict)}
    learned = 0
    max_score = max(int(item["score"]) for item in selected_patterns.values())
    selected_total = max(1, sum(int(item["score"]) for item in selected_patterns.values()))
    for fp, item in selected_patterns.items():
        sig = item["signature"]
        score = int(item["score"])
        share = score / max_score
        session_share = score / selected_total
        policy = "aggressive" if share >= 0.10 else "balanced"
        entry_obj = by_fp.get(fp)
        if entry_obj is None:
            entry_obj = {
                **sig,
                "samples": 0,
                "hot_weight": 0.0,
                "games": [],
                "policy": policy,
                "confidence": 0.55,
            }
            db.setdefault("entries", []).append(entry_obj)
            by_fp[fp] = entry_obj
        # `samples` means independent PGO sessions in v0.3.6, not individual
        # sampled PCs/duplicate regions.  hot_weight is normalized to the
        # current session so a long menu idle cannot dwarf a short gameplay run.
        entry_obj["samples"] = int(entry_obj.get("samples", 0)) + 1
        entry_obj["hot_weight"] = round(
            float(entry_obj.get("hot_weight", 0.0)) + math.log2(1.0 + 4096.0 * session_share),
            4,
        )
        games = set(entry_obj.get("games", []))
        if args.disc_id:
            games.add(args.disc_id)
        entry_obj["games"] = sorted(games)
        if policy_rank(policy) > policy_rank(str(entry_obj.get("policy", "balanced"))):
            entry_obj["policy"] = policy
        entry_obj["confidence"] = confidence_for(entry_obj)
        learned += 1

    db["entries"] = sorted(db.get("entries", []), key=lambda x: str(x.get("fingerprint", "")))
    save_db(args.local, db)
    print(
        f"CrossGameDB: learned {learned} hot PPC pattern(s) from {args.disc_id or args.dol.name} "
        f"source={source_kind} selected-samples={sum(score for _, score in selected)}; "
        f"local entries={len(db['entries'])}"
    )
    return 0


def command_merge(args: argparse.Namespace) -> int:
    base = load_db(args.base)
    local = load_db(args.local)
    out = empty_db()
    out["signature_chunk_instructions"] = int(base.get("signature_chunk_instructions", DEFAULT_SIGNATURE_CHUNK))
    out["entries"] = merge_entries((base, local))
    save_db(args.output, out)
    print(f"CrossGameDB: wrote {len(out['entries'])} merged entries to {args.output}")
    return 0


def command_stats(args: argparse.Namespace) -> int:
    db = load_db(args.db)
    entries = db.get("entries", [])
    exact_games = set()
    aggressive = 0
    for entry in entries:
        exact_games.update(entry.get("games", []))
        aggressive += entry.get("policy") == "aggressive"
    print(f"schema={db.get('schema')} entries={len(entries)} games={len(exact_games)} aggressive={aggressive}")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    sub = ap.add_subparsers(dest="command", required=True)

    match = sub.add_parser("match", help="match a DOL against the shipped/local CrossGameDB")
    match.add_argument("--dol", type=Path, required=True)
    match.add_argument("--db", type=Path, required=True)
    match.add_argument("--local", type=Path)
    match.add_argument("--env", type=Path, required=True)
    match.add_argument("--symbol-order", type=Path, required=True)
    match.add_argument("--compile-chunk", type=int, default=DEFAULT_COMPILE_CHUNK)
    match.add_argument("--chunk-locked", action="store_true")
    match.add_argument(
        "--exclude-game",
        default="",
        help="exclude evidence containing this disc id (prevents self-PGO feedback)",
    )
    match.set_defaults(func=command_match)

    learn = sub.add_parser("learn", help="learn normalized hot PPC patterns from a completed PGO run")
    learn.add_argument("--dol", type=Path, required=True)
    learn.add_argument("--profile", type=Path, required=True)
    learn.add_argument("--llvm-profdata", type=Path, required=True)
    learn.add_argument(
        "--dispatch-profile",
        type=Path,
        help="sampled guest-PC CSV written by StaticRecompCore during this PGO session",
    )
    learn.add_argument("--local", type=Path, required=True)
    learn.add_argument("--disc-id", default="")
    learn.add_argument("--chunk", type=int, default=DEFAULT_SIGNATURE_CHUNK)
    learn.set_defaults(func=command_learn)

    merge = sub.add_parser("merge", help="merge local learned patterns into a repository database")
    merge.add_argument("--base", type=Path, required=True)
    merge.add_argument("--local", type=Path, required=True)
    merge.add_argument("--output", type=Path, required=True)
    merge.set_defaults(func=command_merge)

    stats = sub.add_parser("stats", help="show CrossGameDB statistics")
    stats.add_argument("--db", type=Path, required=True)
    stats.set_defaults(func=command_stats)

    args = ap.parse_args()
    return int(args.func(args))


if __name__ == "__main__":
    raise SystemExit(main())
