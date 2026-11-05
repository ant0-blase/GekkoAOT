#!/usr/bin/env python3
from pathlib import Path
import re
import sys

if len(sys.argv) != 2:
    raise SystemExit("usage: aurora_persistent_array_cache_v161h.py <aurora-source>")
root = Path(sys.argv[1])
marker = "GEKKOAOT_PERSISTENT_ARRAY_CACHE_V161H"

def load(rel):
    p = root / rel
    if not p.exists():
        raise SystemExit(f"missing Aurora source: {p}")
    return p, p.read_text(encoding="utf-8")

changed = False

p, s = load("lib/gfx/resources.hpp")
if marker not in s:
    m = re.search(r"inline constexpr uint64_t StorageBufferSize = [^;]+;[^\n]*\n", s)
    if not m:
        raise SystemExit("resources.hpp: StorageBufferSize anchor missing")
    anchor = m.group(0)
    inject = anchor + (
        "// GEKKOAOT_PERSISTENT_ARRAY_CACHE_V161H\n"
        "inline constexpr uint64_t PersistentArrayStorageSize = 64ull * 1024ull * 1024ull;\n"
        "inline constexpr uint64_t GpuStorageBufferSize = StorageBufferSize + PersistentArrayStorageSize;\n"
    )
    s = s.replace(anchor, inject, 1)
    p.write_text(s, encoding="utf-8")
    changed = True

p, s = load("lib/gfx/frame.cpp")
if marker not in s:
    old = """  createBuffer(g_resources.storageBuffer, wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst, StorageBufferSize,
               "Shared Storage Buffer");"""
    new = """  // GEKKOAOT_PERSISTENT_ARRAY_CACHE_V161H: lower StorageBufferSize bytes
  // remain transient; the upper region retains indexed GX array snapshots.
  createBuffer(g_resources.storageBuffer, wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst,
               GpuStorageBufferSize, "Shared Storage Buffer");"""
    if old not in s:
        raise SystemExit("frame.cpp: shared storage buffer anchor missing")
    s = s.replace(old, new, 1)
    p.write_text(s, encoding="utf-8")
    changed = True

p, s = load("lib/gfx/recording.cpp")
if marker not in s:
    old = """  for (auto& array : gx::g_gxState.arrays) {
    array.cachedRange = {};
  }"""
    count = s.count(old)
    if count < 1:
        raise SystemExit("recording.cpp: AttrArray cachedRange invalidation anchor missing")
    new = """  // GEKKOAOT_PERSISTENT_ARRAY_CACHE_V161H
  for (auto& array : gx::g_gxState.arrays) {
    if (array.cachedRange.size != 0u && array.cachedRange.offset < StorageBufferSize) {
      array.cachedRange = {};
    }
  }"""
    s = s.replace(old, new)
    p.write_text(s, encoding="utf-8")
    changed = True

print("GEKKOAOT_AURORA_ARRAY_CACHE_V161H=1 gpu-storage=transient+64MiB-persistent invalidation=guest-write-driven action=" +
      ("installed" if changed else "already-installed"))
