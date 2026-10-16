#!/usr/bin/env python3
"""Route Aurora texture/TLUT content hashing through gekkoaot_xxh3_128 (AVX2 dispatch)."""
from pathlib import Path
import sys

root = Path(sys.argv[1])
marker = "GEKKOAOT_XXH3_DISPATCH_V367"
path = root / "lib/gx/texture.cpp"
text = path.read_text()
if marker not in text:
    replacements = [
        ("#include <xxhash.h>\n",
         "#include <xxhash.h>\n\n// " + marker + ": runtime/gx/aurora/xxh3_dispatch.cpp\n"
         "XXH128_hash_t gekkoaot_xxh3_128(const void* data, size_t size) noexcept;\n"),
        ("XXH3_128bits(obj.data, textureBytes)", "gekkoaot_xxh3_128(obj.data, textureBytes)"),
        ("XXH3_128bits(tlut->data, tlutBytes)", "gekkoaot_xxh3_128(tlut->data, tlutBytes)"),
    ]
    for old, new in replacements:
        if text.count(old) != 1:
            raise SystemExit(f"lib/gx/texture.cpp: expected unique anchor {old!r}")
        text = text.replace(old, new, 1)
    path.write_text(text)
print(marker + "=1 texture-hash=xxh3-128 avx2=runtime-dispatch")
