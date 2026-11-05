#!/usr/bin/env python3
"""Compose a successful-present counter into the pinned Aurora source."""
from pathlib import Path
import sys

root = Path(sys.argv[1])
marker = "GEKKOAOT_PRESENT_COUNTER_V190"
for rel, replacements in (
    ("lib/gfx/frame.hpp", [("void after_present() noexcept;", "void after_present() noexcept;\n// " + marker + "\nuint64_t presentation_count() noexcept;")]),
    ("lib/gfx/frame.cpp", [
        ("std::atomic_int64_t g_lastPresentNs = 0;", "std::atomic_int64_t g_lastPresentNs = 0;\n// " + marker + "\nstd::atomic_uint64_t g_presentationCount = 0;"),
        ("void after_present() noexcept {", "uint64_t presentation_count() noexcept {\n  return g_presentationCount.load(std::memory_order_relaxed);\n}\n\nvoid after_present() noexcept {\n  g_presentationCount.fetch_add(1, std::memory_order_relaxed);")]),
):
    path = root / rel
    text = path.read_text()
    if marker in text:
        continue
    for old, new in replacements:
        if text.count(old) != 1:
            raise SystemExit(f"{rel}: expected unique anchor {old!r}")
        text = text.replace(old, new, 1)
    path.write_text(text)
print(marker + "=1 source=successful-surface-present")
