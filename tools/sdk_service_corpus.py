#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Offline SDK service corpus: find the retail SDK entry points GekkoAOT can host.

The user-supplied debug corpus (symbolized ELFs) is used only to *recognise*
SDK functions: every body is reduced to the runtime's structural fingerprint
(runtime/sdk/structural_signature.h + the v33 corpus hash in
runtime/native/sdk_autoresolver.cpp).  No code bytes, addresses or symbol maps
are written out -- only names, sizes, hashes and instruction-class counts.

  index <debug-dir> -o corpus.json
      Fingerprint SDK functions (GX, GD, DVD, CARD, PAD, SI, VI, AI, AR/ARQ,
      DSP, AX/MIX, THP, OS, EXI, MTX/VEC, cache, ...) from every ELF with a
      symbol table.  Fingerprints shared by different names are dropped.
  scan <corpus.json> <main.dol> [-o report.json]
      Identify those functions in a retail DOL using the runtime's admission
      policy (proven `bl` targets, then contiguous library neighbours) and
      print a per-subsystem hook map.
  header <corpus.json> -o file.h [--families GX,DVD,...]
      Emit a DebugCorpusSignatureV33-compatible table for the runtime.
"""
import argparse
import json
import os
import re
import struct
import sys
from collections import defaultdict

FAMILIES = [
    ("GX", re.compile(r"^(__)?GX[A-Z]")), ("GD", re.compile(r"^(__)?GD[A-Z]")),
    ("DVD", re.compile(r"^(__)?(DVD|dvd)[A-Z_]")), ("CARD", re.compile(r"^(__)?CARD[A-Z]")),
    ("PAD", re.compile(r"^(__)?PAD[A-Z]")), ("SI", re.compile(r"^(__)?SI[A-Z]")),
    ("VI", re.compile(r"^(__)?VI[A-Z]")), ("AI", re.compile(r"^(__)?AI[A-Z]")),
    ("ARQ", re.compile(r"^(__)?ARQ[A-Z]")), ("AR", re.compile(r"^(__)?AR[A-Z]")),
    ("DSP", re.compile(r"^(__)?DSP[A-Z]")), ("AX", re.compile(r"^(__)?(AX|MIX|SP)[A-Z]")),
    ("THP", re.compile(r"^(__)?THP[A-Z]")), ("OS", re.compile(r"^(__)?OS[A-Z]")),
    ("EXI", re.compile(r"^(__)?EXI[A-Z]")), ("DB", re.compile(r"^(__)?DB[A-Z]")),
    ("MTX", re.compile(r"^(PS|C_)?(MTX|VEC|QUAT)[A-Z]|^PSMTX|^PSVEC|^PSQUAT")),
    ("CACHE", re.compile(r"^(DC|IC|LC)[A-Z]")), ("PPC", re.compile(r"^PPC[A-Z]")),
    ("GBA", re.compile(r"^(__)?GBA[A-Z]")), ("MSL", re.compile(
        r"^(mem(cpy|set|move|cmp|chr)|str(len|cpy|ncpy|cmp|ncmp|cat|ncat|chr|rchr|str|tok)"
        r"|sprintf|vsprintf|snprintf|vsnprintf|printf|sqrt|sqrtf|sin|cos|tan|atan2?|"
        r"acos|asin|pow|floor|ceil|fmod|frexp|ldexp|modf|abs|labs|rand|srand|qsort)$")),
]


SDK_STYLE = re.compile(r"^(__)?[A-Za-z][A-Za-z0-9]*$")


def family_of(name):
    # SDK entry points are plain C identifiers; C++ mangled game methods
    # (Foo__Q2...) and PREFIX_snake helpers only share a prefix with them.
    if not SDK_STYLE.match(name):
        return None
    for fam, rx in FAMILIES:
        if rx.search(name):
            return fam
    return None


# --- runtime-identical structural fingerprint ---------------------------------
def skeleton(word):
    op = word >> 26
    if op == 18:
        return word & 0xFC000003
    if op == 16:
        return word & 0xFFFF0003
    if op in (14, 15, 24, 25) or 32 <= op <= 47:
        imm = word & 0xFFFF
        if imm & 0x8000:
            imm -= 0x10000
        ra = (word >> 16) & 31
        if ra in (2, 13) or imm > 0x100 or imm < -0x100:
            return word & 0xFFFF0000
    return word


M64 = (1 << 64) - 1
MIN_SIZE = 16  # four instructions: shorter bodies are mostly generic getters


def shape(code):
    n = len(code) // 4
    words = struct.unpack(">%dI" % n, code[: n * 4])
    sk = [skeleton(w) for w in words]
    calls = branches = returns = 0
    frame = 0
    for i, w in enumerate(words):
        op = w >> 26
        if op == 18 and (w & 1):
            calls += 1
        if op in (16, 18, 19):
            branches += 1
        if w == 0x4E800020:
            returns += 1
        if i < 8 and op == 37 and ((w >> 21) & 31) == 1 and ((w >> 16) & 31) == 1:
            d = w & 0xFFFF
            if d & 0x8000:
                frame = 0x10000 - d
    h1, h2 = 0xCBF29CE484222325, 0x9E3779B97F4A7C15
    for v in sk:
        h1 = ((h1 ^ v) * 0x100000001B3) & M64
        h2 = (h2 ^ ((v + 0x9E3779B9 + ((h2 << 6) & M64) + (h2 >> 2)) & M64)) & M64
        h2 = (h2 * 0xBF58476D1CE4E5B9) & M64
    return {"size": n * 4, "h1": h1, "h2": h2, "prefix": sk[:3], "calls": calls,
            "branches": branches, "returns": returns, "frame": frame}


def sig_key(s):
    return (s["size"], s["h1"], s["h2"])


# --- index ----------------------------------------------------------------------
def iter_elf_functions(path):
    from elftools.elf.elffile import ELFFile
    from elftools.elf.sections import SymbolTableSection
    with open(path, "rb") as fh:
        try:
            elf = ELFFile(fh)
        except Exception:
            return
        if elf["e_machine"] not in ("EM_PPC",):
            return
        sections = [elf.get_section(i) for i in range(elf.num_sections())]
        for sec in sections:
            if not isinstance(sec, SymbolTableSection):
                continue
            for sym in sec.iter_symbols():
                if sym["st_info"]["type"] != "STT_FUNC" or sym["st_size"] < 12:
                    continue
                idx = sym["st_shndx"]
                if not isinstance(idx, int) or idx >= len(sections):
                    continue
                text = sections[idx]
                if not (text["sh_flags"] & 0x4) or text["sh_type"] != "SHT_PROGBITS":
                    continue
                off = sym["st_value"] - text["sh_addr"]
                size = sym["st_size"] & ~3
                if off < 0 or off + size > text["sh_size"] or size > 0x10000:
                    continue
                yield sym.name, text.data()[off: off + size]


def cmd_index(args):
    by_key = {}
    names_by_key = defaultdict(set)
    sources = defaultdict(set)
    elves = []
    for root, _, files in os.walk(args.debug_dir):
        for f in files:
            if f.lower().endswith((".elf", ".plf")):
                elves.append(os.path.join(root, f))
    for path in sorted(elves):
        label = os.path.relpath(path, args.debug_dir)
        count = 0
        for name, code in iter_elf_functions(path):
            s = shape(code)
            k = sig_key(s)
            # Every named body (game code included) takes part in the
            # uniqueness test: a tiny SDK leaf identical to a game method or
            # to a sibling SDK setter must not be admitted under either name.
            names_by_key[k].add(name)
            if not family_of(name) or s["size"] < MIN_SIZE:
                continue
            by_key[k] = s
            sources[k].add(label)
            count += 1
        print(f"{count:6d} SDK bodies  {label}", file=sys.stderr)
    entries, ambiguous = [], 0
    for k, s in by_key.items():
        names = names_by_key[k]
        if len(names) != 1:
            ambiguous += 1
            continue
        name = next(iter(names))
        entries.append(dict(s, name=name, family=family_of(name), sources=len(sources[k])))
    entries.sort(key=lambda e: (e["family"], e["name"], e["size"], e["h1"]))
    with open(args.output, "w") as fh:
        json.dump({"schema": 1, "entries": entries, "ambiguous": ambiguous}, fh)
    fams = defaultdict(lambda: [0, set()])
    for e in entries:
        fams[e["family"]][0] += 1
        fams[e["family"]][1].add(e["name"])
    print(f"signatures={len(entries)} ambiguous_dropped={ambiguous}", file=sys.stderr)
    for fam, (n, names) in sorted(fams.items()):
        print(f"  {fam:6s} variants={n:5d} functions={len(names)}", file=sys.stderr)


# --- scan -----------------------------------------------------------------------
def load_dol(path):
    data = open(path, "rb").read()
    offs = struct.unpack(">18I", data[0:72])
    addrs = struct.unpack(">18I", data[72:144])
    sizes = struct.unpack(">18I", data[144:216])
    text = []
    for i in range(7):
        if sizes[i]:
            text.append((addrs[i], data[offs[i]: offs[i] + sizes[i]]))
    return text


def cmd_scan(args):
    corpus = json.load(open(args.corpus))["entries"]
    by_prefix = defaultdict(list)
    for e in corpus:
        by_prefix[tuple(e["prefix"])].append(e)
    text = load_dol(args.dol)

    def code_at(pc, size):
        for base, blob in text:
            if base <= pc and pc + size <= base + len(blob):
                o = pc - base
                return blob[o: o + size]
        return None

    targets = set()
    for base, blob in text:
        for i in range(0, len(blob) - 3, 4):
            w = struct.unpack_from(">I", blob, i)[0]
            if (w >> 26) == 18 and (w & 1):
                d = w & 0x03FFFFFC
                if d & 0x02000000:
                    d -= 0x04000000
                targets.add((d if w & 2 else base + i + d) & 0xFFFFFFFF)

    def identify(pc):
        head = code_at(pc, 12)
        if head is None:
            return None
        pre = tuple(skeleton(x) for x in struct.unpack(">3I", head))
        names, best = set(), None
        for e in by_prefix.get(pre, ()):
            code = code_at(pc, e["size"])
            if code is None:
                continue
            s = shape(code)
            if (s["h1"], s["h2"], s["calls"], s["branches"], s["returns"], s["frame"]) == (
                    e["h1"], e["h2"], e["calls"], e["branches"], e["returns"], e["frame"]):
                names.add(e["name"])
                if best is None or e["size"] < best["size"]:
                    best = e
        return best if len(names) == 1 else None

    found = {}
    for pc in sorted(targets):
        e = identify(pc)
        if e:
            found[pc] = e
    # Contiguous library neighbours (indirect-only entry points, callbacks).
    frontier = list(found)
    while frontier:
        pc = frontier.pop()
        nxt = pc + found[pc]["size"]
        for pad in (0, 4, 8, 12, 16, 20, 24, 28):
            cand = nxt + pad
            if cand in found:
                break
            e = identify(cand)
            if e:
                found[cand] = e
                frontier.append(cand)
                break
    fams = defaultdict(list)
    for pc, e in sorted(found.items()):
        fams[e["family"]].append((pc, e["name"], e["size"]))
    report = {"dol": os.path.basename(os.path.dirname(os.path.dirname(args.dol))),
              "functions": {f: [{"address": "%08x" % pc, "name": n, "size": s} for pc, n, s in v]
                            for f, v in fams.items()}}
    if args.output:
        json.dump(report, open(args.output, "w"), indent=1)
    total = sum(len(v) for v in fams.values())
    print(f"identified={total} bl_targets={len(targets)}")
    for fam, v in sorted(fams.items(), key=lambda kv: -len(kv[1])):
        names = sorted({n for _, n, _ in v})
        print(f"  {fam:6s} {len(v):4d}  " + " ".join(names[: args.show]) +
              (" ..." if len(names) > args.show else ""))


# --- header ---------------------------------------------------------------------
def cmd_header(args):
    corpus = json.load(open(args.corpus))["entries"]
    fams = set(args.families.split(",")) if args.families else None
    rows = [e for e in corpus if (fams is None or e["family"] in fams)]
    with open(args.output, "w") as out:
        out.write("#pragma once\n// Generated by tools/sdk_service_corpus.py from the user-supplied debug ELF\n"
                  "// corpus. Structural fingerprints only: no code bytes, addresses or maps.\n"
                  "#include \"sdk/debug_corpus_signatures_v33.h\"\n#include <array>\n"
                  "namespace GekkoAOTSdk {\n")
        out.write(f"inline constexpr std::array<DebugCorpusSignatureV33, {len(rows)}> kSdkServiceCorpusV1 = {{\n")
        for e in rows:
            p = ", ".join("0x%08xu" % x for x in e["prefix"])
            out.write(f'  DebugCorpusSignatureV33{{"{e["name"]}", {e["size"]}u, 0x{e["h1"]:016x}ull, '
                      f'0x{e["h2"]:016x}ull, {{{p}}}, {e["calls"]}u, {e["branches"]}u, {e["returns"]}u, '
                      f'{e["frame"]}u}},\n')
        out.write("};\n}  // namespace GekkoAOTSdk\n")
    print(f"wrote {len(rows)} signatures to {args.output}", file=sys.stderr)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("index"); p.add_argument("debug_dir"); p.add_argument("-o", "--output", required=True)
    p = sub.add_parser("scan"); p.add_argument("corpus"); p.add_argument("dol")
    p.add_argument("-o", "--output"); p.add_argument("--show", type=int, default=40)
    p = sub.add_parser("header"); p.add_argument("corpus"); p.add_argument("-o", "--output", required=True)
    p.add_argument("--families")
    args = ap.parse_args()
    {"index": cmd_index, "scan": cmd_scan, "header": cmd_header}[args.cmd](args)


if __name__ == "__main__":
    main()
