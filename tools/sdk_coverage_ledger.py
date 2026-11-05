#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""SDK coverage ledger: how every recognized SDK entry point of a game runs.

Inputs are the per-game files the runtime writes under
.gekkoaot/cache/sdk/<GAME>/ (the v300 service map and the native intercept
manifest) and, optionally, a perf profile of a real gameplay run:

  perf record -e cpu_core/cycles/u -j any_call,u -p <runner> -o hp.data -- sleep 15
  perf report -i hp.data --no-children --sort symbol --stdio > hp.report
  perf script -i hp.data -F brstacksym > hp.brstack     (call counts, optional)

  sdk_coverage_ledger.py <game dir> [--report hp.report] [--brstack hp.brstack]
                         [--tests tests/] -o ledger.json [--markdown ledger.md]

Native statuses require matching execution evidence and a hashed validation
artifact. A compiler manifest alone remains UNVERIFIED. Unreplaced entries
remain AOT_GUEST, regardless of the host architecture of their compiled code.
Absent runtime measurements are unknown, never inferred to be zero.
"""
import argparse
import hashlib
import json
import os
import re
import struct
import sys
from collections import Counter, defaultdict
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from sdk_service_corpus import FAMILIES  # noqa: E402

# Declared typed command producers; this does not assert FIFO removal or validation.
GX_LOWERED = {"GXSetArray", "GXLoadPosMtxImm", "GXLoadTexMtxImm", "GXLoadNrmMtxImm",
              "GXLoadNrmMtxImm3x3", "GXSetAlphaCompare", "GXSetColorUpdate",
              "GXSetAlphaUpdate", "GXSetZMode", "GXSetBlendMode"}
STATUSES = ("DIRECT_HOST", "LLVM_INTRINSIC", "NATIVEGX_IR", "NATIVE_IO", "NATIVE_AUDIO",
            "AOT_GUEST", "COMPATIBILITY_REQUIRED", "UNIMPLEMENTED", "UNVERIFIED")


def service_catalog():
    source = Path(__file__).resolve().parents[1] / 'runtime/sdk/native_services.def'
    result = {}
    indices = Counter()
    for domain, kind, name, abi in re.findall(r'SDK_(SIMPLE|OS)\((\w+), "([^"]+)", "([^"]+)"\)', source.read_text()):
        token = (0xfffc0000 if domain == 'SIMPLE' else 0xfffd0000) | indices[domain]
        indices[domain] += 1
        result[token] = dict(name=name, binding=f'gekkoaot_sdk_{domain.lower()}_{kind}',
                             host_abi='uint32_t(CPUState*)', abi_class=abi,
                             validation='availability-only')
    return result


def binary_references(path, entries):
    """Static direct BLs and initialized-data address words, not indirect proof."""
    image = Path(path).read_bytes()
    if len(image) < 0x100:
        raise ValueError('truncated DOL')
    word = lambda at: struct.unpack_from('>I', image, at)[0]
    calls = {e['address']: [] for e in entries}
    references = {e['address']: [] for e in entries}
    for section in range(18):
        offset, base, size = (word(at + 4 * section) for at in (0, 0x48, 0x90))
        if not size:
            continue
        if offset < 0x100 or offset + size > len(image) or base + size > 0x100000000:
            raise ValueError('invalid DOL section bounds')
        for pos in range(0, size - 3, 4):
            value, pc = word(offset + pos), base + pos
            if section < 7 and value >> 26 == 18 and value & 1:
                displacement = value & 0x03fffffc
                if displacement & 0x02000000:
                    displacement -= 0x04000000
                target = (displacement if value & 2 else pc + displacement) & 0xffffffff
                if target in calls:
                    calls[target].append(hex(pc))
            elif section >= 7 and value in references:
                references[value].append(hex(pc))
    return calls, references


def source_identity(path):
    with open(path, 'rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def verified_observations(path, map_path, manifest, binary=None):
    """Runtime/test evidence is bound to exact input files, never to names.

    Missing measurements remain unknown. A static manifest is a declaration
    of a replacement, not evidence that the compiled game executed it.
    """
    if not path:
        return {}
    with open(path) as stream:
        evidence = json.load(stream)
    if (evidence.get('schema') != 1 or
            evidence.get('service_map_sha256') != source_identity(map_path) or
            evidence.get('binary_sha256') != (source_identity(binary) if binary else None) or
            evidence.get('manifest_sha256') != (source_identity(manifest) if manifest else None)):
        raise ValueError('SDK execution evidence does not match the selected map and manifest')
    result = {}
    for entry in evidence.get('entries', []):
        address = int(entry['address'], 0)
        if address in result:
            raise ValueError(f'duplicate SDK execution evidence: {address:#x}')
        if entry.get('status') not in STATUSES:
            raise ValueError('unknown SDK lowering status')
        for metric in ('native_calls', 'guest_calls', 'exclusive_cpu_ns', 'guest_instructions',
                       'compatibility_transitions', 'fifo_bytes_avoided', 'mmio_operations_avoided'):
            if entry.get(metric) is not None and (type(entry[metric]) is not int or entry[metric] < 0):
                raise ValueError(f'invalid SDK measurement: {metric}')
        if entry['status'] in STATUSES[:5] and (entry.get('validation') != 'passed' or
                not entry.get('implementation') or not entry.get('lowering_method') or
                not entry.get('validation_artifact') or not entry.get('validation_artifact_sha256')):
            raise ValueError('native status requires implementation, lowering and passing validation evidence')
        if entry.get('validation') == 'passed':
            artifact = Path(path).resolve().parent / entry.get('validation_artifact', '')
            if not artifact.is_file() or source_identity(artifact) != entry.get('validation_artifact_sha256'):
                raise ValueError('SDK validation artifact is missing or has changed')
        result[address] = entry
    return result


def classify_entries(entries, direct, device_support, observations, references):
    catalog = service_catalog()
    by_name = defaultdict(list)
    for entry in entries:
        by_name[entry['name']].append(entry)
    for ordinal, entry in enumerate(entries):
        name, address = entry['name'], entry['address']
        aliases = by_name[name]
        entry.update(entry_id=ordinal, family=family_of(name),
                     aliases=[hex(a['address']) for a in aliases if a is not entry],
                     confidence='service-map-recognition-only', abi=None, handler=None,
                     implementation=None, lowering_method=None, validation='unverified',
                     runtime_native_calls=None, runtime_guest_calls=None, exclusive_cpu_ns=None,
                     guest_instructions=None, compatibility_transitions=None,
                     fifo_bytes_avoided=None, mmio_operations_avoided=None,
                     guest_call_sites=None, sdk_revision=None, remaining_guest_execution='unmeasured',
                     test_referenced=name in references or name.lstrip('_') in references, tested=False)
        declared = direct.get(address)
        if declared:
            # Preserve aliases explicitly. Matching an address alone can hide a
            # stale manifest for another overlay or a conflicting recognized name.
            compatible = declared['name'] == name or {declared['name'], name} == {'memcpy', '__memcpy'}
            entry.update(path='UNVERIFIED', handler=declared['token'], abi=declared['abi'],
                         declared_native_path='NATIVEGX_IR' if name in GX_LOWERED else 'DIRECT_HOST',
                         declaration_matches=compatible)
            entry['native_service'] = catalog.get(int(declared['token'], 0))
            entry['compiler_binding'] = declared.get('binding')
        elif len({a['size'] for a in aliases}) > 1:
            entry.update(path='UNVERIFIED', confidence='ambiguous-alias')
        else:
            entry['path'] = 'AOT_GUEST'
        observed = observations.get(address)
        if observed:
            if observed.get('name') != name or (declared and not entry['declaration_matches']):
                raise ValueError(f'SDK evidence identity mismatch at {address:#x}')
            if observed['status'] in STATUSES[:5] and not declared:
                raise ValueError(f'native evidence has no matching compiler manifest at {address:#x}')
            entry.update(path=observed['status'], implementation=observed.get('implementation'),
                         lowering_method=observed.get('lowering_method'), validation=observed.get('validation'),
                         tested=observed.get('validation') == 'passed',
                         runtime_native_calls=observed.get('native_calls'), runtime_guest_calls=observed.get('guest_calls'))
            for metric in ('exclusive_cpu_ns', 'guest_instructions', 'compatibility_transitions',
                           'fifo_bytes_avoided', 'mmio_operations_avoided', 'guest_call_sites',
                           'sdk_revision', 'remaining_guest_execution'):
                if metric in observed:
                    entry[metric] = observed[metric]
        entry['device_support'] = address in device_support
    return by_name
# Host runtime symbols (not compiled guest code) by subsystem, for the
# exclusive-time split of a profile.
HOST_CATEGORIES = [
    ("gx-fifo-decode", re.compile(r"fifo|command_processor|handle_(cp|xf|bp)|draw_prim|LowerNative|Consume|NativeCommand|AppendNative", re.I)),
    ("gx-renderer", re.compile(r"aurora|gfx::|wgpu|dawn|push_draw|build_uniform|shader|pipeline|texture", re.I)),
    ("native-os", re.compile(r"NativeOS|native_os|Scheduler|OSThread|Reschedule", re.I)),
    ("mmio-hw", re.compile(r"mmio|Mmio|MMIO|native_hw|NativeHW|wgpipe|WGPIPE|Exi|Dvd|Vi[A-Z]|Si[A-Z]|Pi[A-Z]|Mi[A-Z]", re.I)),
    ("audio-dsp", re.compile(r"dsp|DSP|audio|Audio|AX|mixer|Mixer|adpcm|ADPCM", re.I)),
    ("runtime-boundary", re.compile(r"boundary|Boundary|dispatch|Dispatch|interrupt|Interrupt|Decrementer|budget", re.I)),
]


def family_of(name):
    for fam, rx in FAMILIES:
        if rx.search(name):
            return fam
    return "MSL" if not name.startswith("__") else "SDK-INTERNAL"


def read_map(path):
    entries = []
    for line in Path(path).read_text().splitlines():
        if line.startswith("#") or not line.strip():
            continue
        addr, size, name = line.split()[:3]
        entries.append({"address": int(addr, 16), "size": int(size), "name": name})
    return entries


def read_manifest(path):
    direct, device_support = {}, set()
    for line in Path(path).read_text().splitlines():
        parts = line.split()
        if line.startswith("# device-support") and len(parts) >= 4:
            device_support.add(int(parts[2], 16))
        elif not line.startswith("#") and len(parts) >= 3:
            direct[int(parts[0], 16)] = {"name": parts[1], "token": parts[2],
                                         "abi": parts[3] if len(parts) > 3 else "leaf",
                                         "binding": parts[4] if len(parts) > 4 else None}
    return direct, device_support


def read_report(path):
    """perf report --no-children --sort symbol: percent per symbol."""
    guest, host = Counter(), Counter()
    rx = re.compile(r"^\s*([\d.]+)%\s+\[\.\]\s+(\S+)")
    for line in Path(path).read_text(errors="replace").splitlines():
        m = rx.match(line)
        if not m:
            continue
        pct, sym = float(m.group(1)), m.group(2)
        g = re.match(r"func_([0-9A-Fa-f]{8})", sym)
        if g:
            guest[int(g.group(1), 16)] += pct
        else:
            host[sym] += pct
    return guest, host


def read_brstack(path):
    """perf script -F brstacksym: count branch records landing on a function entry."""
    calls = Counter()
    rx = re.compile(r"/func_([0-9A-Fa-f]{8})\+0x0/")
    for line in Path(path).read_text(errors="replace").splitlines():
        for m in rx.finditer(line):
            calls[int(m.group(1), 16)] += 1
    return calls


def tested_names(tests_dir):
    names = set()
    if not tests_dir or not os.path.isdir(tests_dir):
        return names
    word = re.compile(r"\b([A-Za-z_][A-Za-z0-9_]{3,})\b")
    for root, _, files in os.walk(tests_dir):
        for f in files:
            if f.endswith((".cpp", ".c", ".h", ".py")):
                names.update(word.findall(Path(root, f).read_text(errors="replace")))
    return names


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("game_dir")
    ap.add_argument("--manifest", help="native intercept manifest (default: newest in game_dir)")
    ap.add_argument("--report")
    ap.add_argument("--brstack")
    ap.add_argument("--tests", default=os.path.join(os.path.dirname(__file__), "..", "tests"))
    ap.add_argument("--execution-evidence", help="JSON measurements and validation tied to exact map/manifest hashes")
    ap.add_argument("--binary", help="matching DOL for identity, static BL call sites and data address references")
    ap.add_argument("-o", "--output", required=True)
    ap.add_argument("--markdown")
    args = ap.parse_args()

    map_path = os.path.join(args.game_dir, "sdk-service-map-v300.txt")
    entries = read_map(map_path)
    manifest = args.manifest
    if not manifest:
        cands = [os.path.join(args.game_dir, f) for f in os.listdir(args.game_dir)
                 if f.startswith("native-intercepts-")]
        manifest = max(cands, key=os.path.getmtime) if cands else None
    direct, device_support = read_manifest(manifest) if manifest else ({}, set())
    guest_pct, host_pct = read_report(args.report) if args.report else (Counter(), Counter())
    calls = read_brstack(args.brstack) if args.brstack else Counter()
    tested = tested_names(args.tests)

    observations = verified_observations(args.execution_evidence, map_path, manifest, args.binary)
    if set(observations) - {e['address'] for e in entries}:
        raise ValueError('SDK evidence contains entries absent from the selected service map')
    by_name = classify_entries(entries, direct, device_support, observations, tested)
    if args.binary:
        call_sites, data_refs = binary_references(args.binary, entries)
        for entry in entries:
            entry['guest_call_sites'] = call_sites[entry['address']]
            entry['data_address_references'] = data_refs[entry['address']]
            entry['reference_scan'] = 'static-direct-bl-and-data-words; indirect-targets-unproven'
    for e in entries:
        addr = e["address"]
        e["cpu_pct"] = round(guest_pct.get(addr, 0.0), 4) if args.report else None
        e["calls_sampled"] = calls.get(addr, 0) if args.brstack else None
    for e in entries:
        e["address"] = hex(e["address"])

    totals = Counter(e["path"] for e in entries)
    cpu_by_path = Counter()
    for e in entries:
        cpu_by_path[e["path"]] += e["cpu_pct"] or 0
    sdk_addrs = {int(e["address"], 16) for e in entries}
    game_cpu = sum(p for a, p in guest_pct.items() if a not in sdk_addrs)
    host_by_cat = Counter()
    for sym, pct in host_pct.items():
        for cat, rx in HOST_CATEGORIES:
            if rx.search(sym):
                host_by_cat[cat] += pct
                break
        else:
            host_by_cat["other-host"] += pct
    ranked = sorted((e for e in entries if e["path"] not in STATUSES[:5]),
                    key=lambda e: (-(e["cpu_pct"] or 0), -(e["calls_sampled"] or 0), e["name"]))
    ledger = {
        "schema": 2, "game_dir": os.path.abspath(args.game_dir), "manifest": manifest,
        "service_map_sha256": source_identity(map_path),
        "binary": os.path.abspath(args.binary) if args.binary else None,
        "binary_sha256": source_identity(args.binary) if args.binary else None,
        "manifest_sha256": source_identity(manifest) if manifest else None,
        "execution_evidence": args.execution_evidence,
        "entries_total": len(entries), "distinct_names": len(by_name),
        "duplicate_names": sorted(n for n, v in by_name.items() if len(v) > 1),
        "paths": dict(totals), "cpu_pct_by_path": {k: round(v, 3) for k, v in cpu_by_path.items()} if args.report else None,
        "cpu_pct_game_code": round(game_cpu, 3) if args.report else None,
        "cpu_pct_host_by_category": {k: round(v, 3) for k, v in host_by_cat.items()} if args.report else None,
        "entries": entries,
    }
    with open(args.output, "w") as f:
        json.dump(ledger, f, indent=1)

    if args.markdown:
        fams = defaultdict(Counter)
        for e in entries:
            fams[e["family"]][e["path"]] += 1
        paths = STATUSES
        out = [f"# SDK coverage ledger: {os.path.basename(os.path.normpath(args.game_dir))}", "",
               f"{len(entries)} entries, {len(by_name)} names; duplicates: "
               + (", ".join(ledger["duplicate_names"]) or "none"), "",
               "Manifest declarations and source test references do not prove native execution or passing validation.",
               "Runtime metrics are null in JSON when no matching measurements were supplied.", "",
               "| Path | Entries | Exclusive CPU % |", "| --- | ---: | ---: |"]
        out += [f"| {p} | {totals.get(p, 0)} | " +
                (f"{cpu_by_path.get(p, 0):.2f}" if args.report else "unmeasured") + " |" for p in paths]
        out += ["", (f"Game code outside the SDK: {game_cpu:.2f}% CPU. Host runtime: "
                + ", ".join(f"{k} {v:.2f}%" for k, v in host_by_cat.most_common())) if args.report
                else "CPU costs have not been measured.", "",
                "| Family | " + " | ".join(paths) + " |", "| --- |" + " ---: |" * len(paths)]
        for fam in sorted(fams):
            out.append(f"| {fam} | " + " | ".join(str(fams[fam].get(p, 0)) for p in paths) + " |")
        out += ["", "## Remaining work by measured cost", "",
                "| Function | Address | Path | CPU % | Sampled calls | Tested |", "| --- | --- | --- | ---: | ---: | --- |"]
        for e in ranked[:40]:
            cost=f"{e['cpu_pct']:.3f}" if e['cpu_pct'] is not None else 'unmeasured'
            count=str(e['calls_sampled']) if e['calls_sampled'] is not None else 'unmeasured'
            out.append(f"| {e['name']} | {e['address']} | {e['path']} | {cost} | {count} | {'yes' if e['tested'] else 'unverified'} |")
        out += ["", "## Complete registry", "",
                "| Function | Address | Family | Status | Compiler binding | Validation | Native calls |",
                "| --- | --- | --- | --- | --- | --- | ---: |"]
        for entry in entries:
            native_calls = entry['runtime_native_calls']
            out.append(f"| {entry['name']} | {entry['address']} | {entry['family']} | {entry['path']} | "
                       f"{entry.get('compiler_binding') or 'unverified'} | {entry['validation']} | "
                       f"{native_calls if native_calls is not None else 'unmeasured'} |")
        with open(args.markdown, "w") as f:
            f.write("\n".join(out) + "\n")
    print(json.dumps({"entries": len(entries), "paths": dict(totals),
                      "cpu_pct_by_path": ledger["cpu_pct_by_path"]}))


if __name__ == "__main__":
    main()
