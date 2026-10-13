#!/usr/bin/env python3
"""Run every GameCube image with NativeOS and preserve honest progress evidence.

The normal mode compiles through gekkoaotctl before launching. --reuse-cache is
for quick diagnostics of already built modules and labels that provenance.
"""

import argparse
import csv
import hashlib
import json
import os
from pathlib import Path
import re
import signal
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_IMAGES = ROOT / "debug/Extra SDKs + Tools + Libraries + Docs/Demos/NDDEMO v1.08"
DEFAULT_RESULTS = ROOT / "debug/compat-results"
EXTENSIONS = {".rvz", ".iso", ".gcm"}
FIELDS = ["game", "file", "sha256", "game_id", "boot", "intro", "menu", "gameplay",
          "native_os", "vps", "guest_fps", "render_updates", "pc_values", "di_reads",
          "vi_progress", "timebase_progress", "ai_samples", "dsp_control", "cp_distance",
          "current_thread", "pi_cause", "hle_calls", "stall_class", "status", "cause",
          "hotspot", "cpu_seconds", "elapsed_seconds", "module"]


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(4 * 1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def disc_for(image):
    for metadata in (ROOT / ".gekkoaot/cache/discs").glob("*/meta/disc-source.json"):
        try:
            if Path(json.loads(metadata.read_text())["source"]).resolve() == image.resolve():
                return metadata.parent.parent
        except (OSError, KeyError, ValueError):
            continue
    return None


def prepare(image, timeout, log):
    env = os.environ.copy()
    env["GEKKOAOT_ISO"] = str(image)
    env["GEKKOAOT_NATIVE_OS"] = "1"
    env.setdefault("GEKKOAOT_JOBS", "8")
    with log.open("w") as output:
        proc = subprocess.Popen([str(ROOT / "bin/gekkoaotctl"), "compile"],
                                cwd=ROOT, env=env, stdout=output,
                                stderr=subprocess.STDOUT, start_new_session=True)
        try:
            return proc.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            os.killpg(proc.pid, signal.SIGTERM)
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                os.killpg(proc.pid, signal.SIGKILL)
                proc.wait()
            raise TimeoutError(f"AOT compile exceeded {timeout}s; see {log}")


def latest(paths):
    candidates = [path for path in paths if path.is_file()]
    return max(candidates, key=lambda path: path.stat().st_mtime) if candidates else None


def fnv64(path):
    value = 1469598103934665603
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(16384), b""):
            for byte in chunk:
                value = ((value ^ byte) * 1099511628211) & 0xffffffffffffffff
    return f"{value:x}"


def module_and_manifest(game_id, prefer_native_os):
    pairs = []
    for manifest in (ROOT / ".gekkoaot/cache/sdk" / game_id).glob("native-intercepts-*.txt"):
        key = fnv64(manifest)
        modules = (ROOT / ".gekkoaot/cache/modules" / game_id).glob(
            f"*-{key}-*/g{game_id}_recomp.so")
        for module in modules:
            hooks = bool(re.search(r"\bOS(?:Save|Load|Create|Resume|Yield|Sleep|Wakeup|SetCurrent)",
                                   manifest.read_text()))
            pairs.append((hooks, module.stat().st_mtime, module, manifest))
    if not pairs:
        raise FileNotFoundError(f"matching AOT module and SDK manifest missing for {game_id}")
    pairs.sort(key=lambda item: (item[0], item[1]) if prefer_native_os else (item[1],))
    hooks, _, module, manifest = pairs[-1]
    return module, manifest, hooks


def process_cpu_seconds(pid):
    try:
        fields = Path(f"/proc/{pid}/stat").read_text().rsplit(") ", 1)[1].split()
        return (int(fields[11]) + int(fields[12])) / os.sysconf("SC_CLK_TCK")
    except (OSError, IndexError, ValueError):
        return 0.0


def parse_log(log, os_hooks, elapsed, cpu_seconds, module):
    text = log.read_text(errors="replace") if log.exists() else ""
    def last(pattern, default=""):
        hits = re.findall(pattern, text)
        return hits[-1] if hits else default
    pc_values = set(re.findall(r"GEKKOAOT_NATIVE_PC_TRACE_V10[56]=1[^\n]*? pc=([0-9a-f]+)", text))
    traces = re.findall(r"GEKKOAOT_NATIVE_PC_TRACE_V106=1[^\n]*", text)
    def trace_values(field):
        return re.findall(rf"\b{field}=([0-9a-f]+)", "\n".join(traces))
    vi_values = trace_values("vi_halfline")
    tb_values = trace_values("tb")
    ai_values = trace_values("ai_samples")
    dsp_values = trace_values("dsp_ctl")
    cp_values = trace_values("cp_distance")
    current_values = trace_values("current")
    pi_values = trace_values("pi_cause")
    hle_values = trace_values("hle_calls")
    changes = len(re.findall(r"action=present[^\n]*? changed=1", text))
    frames = re.findall(r"GEKKOAOT_PERF_V49=1[^\n]*?vps=([0-9.]+)[^\n]*?guest_fps=([0-9.]+)", text)
    vps, fps = frames[-1] if frames else ("", "")
    status = last(r'GEKKOAOT_NATIVE_RUN_RESULT status="([^"]+)"')
    fault_pc = last(r"GEKKOAOT_NATIVE_RUN_RESULT[^\n]*? pc=(0x[0-9a-f]+)")
    fault_address = last(r"GEKKOAOT_NATIVE_RUN_RESULT[^\n]*? fault=(0x[0-9a-f]+)")
    loaded = "GEKKOAOT_NATIVE_BOOT game_id=" in text and "GEKKOAOT_NATIVE_DOL_LOADED" in text
    di_reads = last(r"GEKKOAOT_NATIVE_RUN_RESULT[^\n]*?di_reads=(\d+)", "0")
    boot = "yes" if loaded and (len(pc_values) > 2 or (fps and float(fps) > 0)) and (int(di_reads) > 0 or changes > 0) else "unverified"
    stall_class = ""
    if not status:
        outcome, cause = "crash-or-timeout", "no native run result"
    elif status != "host requested exit":
        outcome, cause = "fail", f"{status} at {fault_pc}, fault {fault_address}"
    elif changes == 0 and (not fps or float(fps) == 0):
        dsp_irq = bool(pi_values and (int(pi_values[-1], 16) & 0x40))
        cp_backlog = bool(cp_values and int(cp_values[-1], 16))
        no_current = bool(current_values and int(current_values[-1], 16) == 0)
        stall_class = ("DSP/audio interrupt" if dsp_irq else "GX FIFO" if cp_backlog
                       else "NativeOS scheduler suspected" if no_current else "unknown")
        outcome, cause = "stalled", f"{stall_class}; no changing guest frame"
    elif len(pc_values) <= 2 and int(di_reads) == 0:
        outcome, cause = "suspected-hang", "small PC set with no disc progress"
    else:
        outcome, cause = "progress-unverified", "menu/gameplay requires visual or input verification"
    if outcome == "fail" and status == "guest halt":
        stall_class = "guest halt / exception"
    elif outcome == "fail" and "MMIO" in status:
        stall_class = "MMIO"
    perf_file = log.with_name("perf.txt")
    perf_text = perf_file.read_text(errors="replace") if perf_file.exists() else ""
    hotspot = next((line.strip() for line in perf_text.splitlines()
                    if re.match(r"^\s*[0-9.]+%", line)), "")
    if not hotspot and "Access to performance monitoring" in perf_text:
        hotspot = "perf denied by kernel"
    return {"boot": boot, "intro": "unverified", "menu": "unverified",
            "gameplay": "unverified", "native_os": "yes" if os_hooks else "no-hooks",
            "vps": vps, "guest_fps": fps, "render_updates": changes,
            "pc_values": len(pc_values), "di_reads": di_reads,
            "vi_progress": len(set(vi_values)) > 1,
            "timebase_progress": len(set(tb_values)) > 1,
            "ai_samples": ai_values[-1] if ai_values else "",
            "dsp_control": dsp_values[-1] if dsp_values else "",
            "cp_distance": cp_values[-1] if cp_values else "",
            "current_thread": current_values[-1] if current_values else "",
            "pi_cause": pi_values[-1] if pi_values else "",
            "hle_calls": hle_values[-1] if hle_values else "",
            "stall_class": stall_class, "status": outcome,
            "cause": cause, "hotspot": hotspot, "cpu_seconds": round(cpu_seconds, 2),
            "elapsed_seconds": round(elapsed, 2), "module": str(module)}


def run_image(image, digest, disc, result_dir, seconds, with_perf, reuse_cache):
    game_id = (disc / "meta/disc-id.txt").read_text().strip()
    module, manifest, os_hooks = module_and_manifest(game_id, reuse_cache)
    bridge = ROOT / ".gekkoaot/build/NativeGX-Aurora/libgekkoaot-native-gx.so"
    if not bridge.exists():
        raise FileNotFoundError(f"module/manifest/renderer missing for {game_id}")
    target = result_dir / game_id
    prior_command = target / "command.json"
    if prior_command.exists():
        try:
            prior = json.loads(prior_command.read_text())["command"]
            if str(image) not in prior:
                target = result_dir / f"{game_id}-{digest[:8]}"
        except (OSError, KeyError, ValueError):
            target = result_dir / f"{game_id}-{digest[:8]}"
    target.mkdir(parents=True, exist_ok=True)
    (target / "perf.txt").unlink(missing_ok=True)
    stop = target / "stop.request"
    snapshot = target / "snapshot.request"
    for stale in (stop, stop.with_suffix(".request.consumed"), snapshot):
        stale.unlink(missing_ok=True)
    cmd = [str(ROOT / "bin/gekkoaot-native-run"), "--module", str(module),
           "--dol", str(disc / "sys/main.dol"), "--game-id", game_id,
           "--disc-image", str(image), "--native-gx-bridge", str(bridge),
           "--sdk-manifest", str(manifest), "--stop-file", str(stop),
           "--snapshot-file", str(snapshot)]
    for name in ("boot.bin", "fst.bin", "bi2.bin"):
        if (disc / "sys" / name).exists():
            cmd += ["--" + name.replace(".bin", "-bin"), str(disc / "sys" / name)]
    secondary = ROOT / ".gekkoaot/cache/secondary-aot" / game_id
    # The controller writes the exact current REL/secondary-DOL set here.
    # Filename prefixes alone cannot distinguish separate executable modules.
    module_list = secondary / "modules.txt"
    secondary_modules = ([Path(line) for line in module_list.read_text().splitlines() if line.strip()]
                         if module_list.exists() else [])
    for path in secondary_modules:
        if not path.is_file():
            raise FileNotFoundError(f"secondary AOT module missing: {path}")
        cmd += ["--secondary-module", str(path)]
    env = os.environ.copy()
    env["GEKKOAOT_NATIVE_OS"] = "1"
    env["GEKKOAOT_NATIVE_PC_TRACE"] = "1"
    env["GEKKOAOT_USER_DIR"] = str(target)
    # The renderer and runtime both use the same opt-out default after v160.
    env.setdefault("GEKKOAOT_FRAME_INTERPOLATION", "0")
    (target / "command.json").write_text(json.dumps({"command": cmd, "environment": {
        key: env[key] for key in ("GEKKOAOT_NATIVE_OS", "GEKKOAOT_FRAME_INTERPOLATION")},
        "manifest": str(manifest), "secondary_modules": len(secondary_modules)}, indent=2) + "\n")
    perf = None
    start = time.monotonic()
    cpu_start = 0.0
    cpu_end = 0.0
    with (target / "run.log").open("w") as output:
        proc = subprocess.Popen(cmd, cwd=ROOT, env=env, stdout=output,
                                stderr=subprocess.STDOUT, start_new_session=True)
        cpu_start = process_cpu_seconds(proc.pid)
        next_snapshot = start + 5
        perf_started = False
        try:
            while proc.poll() is None and time.monotonic() - start < seconds:
                now = time.monotonic()
                if now >= next_snapshot:
                    snapshot.touch()
                    next_snapshot += 5
                if with_perf and not perf_started and now - start >= 5:
                    perf_started = True
                    perf_log = (target / "perf.txt").open("w")
                    perf = subprocess.Popen(["perf", "record", "-F", "199", "-e", "cycles:u",
                                             "--call-graph", "dwarf", "-o", str(target / "perf.data"),
                                             "-p", str(proc.pid), "--", "sleep", "10"],
                                            stdout=perf_log, stderr=subprocess.STDOUT)
                time.sleep(0.25)
            cpu_end = process_cpu_seconds(proc.pid)
            if proc.poll() is None:
                stop.touch()
                try:
                    proc.wait(timeout=8)
                except subprocess.TimeoutExpired:
                    os.killpg(proc.pid, signal.SIGTERM)
                    try:
                        proc.wait(timeout=3)
                    except subprocess.TimeoutExpired:
                        os.killpg(proc.pid, signal.SIGKILL)
                        proc.wait()
        finally:
            if perf:
                try:
                    perf.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    perf.terminate()
                    perf.wait()
                perf_log.close()
                if perf.returncode == 0:
                    with (target / "perf.txt").open("a") as report:
                        subprocess.run(["perf", "report", "--stdio", "--no-children",
                                        "-i", str(target / "perf.data"), "--sort", "symbol"],
                                       stdout=report, stderr=subprocess.STDOUT, timeout=30)
            snapshot.unlink(missing_ok=True)
            (target / "snapshot.request.mem1.bin").unlink(missing_ok=True)
    row = parse_log(target / "run.log", os_hooks, time.monotonic() - start,
                    max(0, cpu_end - cpu_start), module)
    row.update(game=image.stem, file=str(image), game_id=game_id)
    if proc.returncode and row["status"] == "progress-unverified":
        row.update(status="crash", cause=f"process exit {proc.returncode}")
    return row


def write_matrix(rows, result_dir):
    result_dir.mkdir(parents=True, exist_ok=True)
    with (result_dir / "compatibility.csv").open("w", newline="") as output:
        writer = csv.DictWriter(output, fieldnames=FIELDS)
        writer.writeheader()
        writer.writerows(rows)
    columns = ["game", "file", "game_id", "boot", "intro", "menu", "gameplay", "native_os",
               "vps", "guest_fps", "render_updates", "status", "cause", "hotspot"]
    lines = ["# GameCube NativeOS compatibility", "",
             "Automated evidence does not certify menu access or playability. Those fields remain unverified until visual and input checks.", "",
             "| " + " | ".join(columns) + " |", "| " + " | ".join("---" for _ in columns) + " |"]
    for row in rows:
        lines.append("| " + " | ".join(str(row.get(key, "")).replace("|", "\\|") for key in columns) + " |")
    (result_dir / "compatibility.md").write_text("\n".join(lines) + "\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--images", type=Path, default=DEFAULT_IMAGES)
    parser.add_argument("--results", type=Path, default=DEFAULT_RESULTS)
    parser.add_argument("--seconds", type=int, default=35)
    parser.add_argument("--build-timeout", type=int, default=1800)
    parser.add_argument("--reuse-cache", action="store_true")
    parser.add_argument("--no-perf", action="store_true")
    parser.add_argument("--only", help="case-insensitive filename fragment")
    args = parser.parse_args()
    images = sorted(path for path in args.images.rglob("*")
                    if path.is_file() and path.suffix.lower() in EXTENSIONS)
    if args.only:
        images = [path for path in images if args.only.lower() in path.name.lower()]
    if not images:
        parser.error("no GameCube images found")
    args.results.mkdir(parents=True, exist_ok=True)
    rows = []
    hashes = {}
    for index, image in enumerate(images, 1):
        print(f"[{index}/{len(images)}] {image.name}", flush=True)
        digest = sha256(image)
        if digest in hashes:
            row = dict(hashes[digest])
            row.update(game=image.stem, file=str(image), status="duplicate-image",
                       cause=f"identical SHA-256 to {hashes[digest]['file']}")
        else:
            try:
                disc = disc_for(image)
                if not args.reuse_cache or disc is None:
                    build_log = args.results / (image.stem.replace("/", "_") + ".build.log")
                    if prepare(image, args.build_timeout, build_log) != 0:
                        raise RuntimeError(f"compile failed; see {build_log}")
                    disc = disc_for(image)
                if disc is None:
                    raise FileNotFoundError("disc extraction unavailable")
                row = run_image(image, digest, disc, args.results, args.seconds, not args.no_perf,
                                args.reuse_cache)
            except (OSError, RuntimeError, TimeoutError, subprocess.TimeoutExpired) as error:
                row = {key: "" for key in FIELDS}
                row.update(game=image.stem, file=str(image), status="build-or-launch-failure",
                           cause=str(error))
            hashes[digest] = row
        row["sha256"] = digest
        rows.append(row)
        write_matrix(rows, args.results)
        print(f"  {row['status']}: {row['cause']}", flush=True)
    return 0 if all(row["status"] == "progress-unverified" for row in rows) else 1


if __name__ == "__main__":
    sys.exit(main())
