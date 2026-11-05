#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Clean-room policy guard (docs/CLEAN_ROOM.md).

Fails when code translated from game executables reappears, or when a file
of runtime/sdk/ holds a table of instruction words instead of fingerprints:
more than 64 32-bit constants whose primary opcode is a common PowerPC one.
Per-instruction fingerprint hashes must stay at most 8 bits wide.
"""
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
FORBIDDEN = [
    'runtime/native/hle/gen',
    'runtime/native/hle/families',
    'tools/ppc2cpp.py',
    'tools/gen_game_library.py',
]
# Primary opcodes of common integer, branch, load/store and FP instructions.
COMMON_OPCODES = {4, 7, 8, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 23, 24, 25, 26, 27,
                  28, 29, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47,
                  48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 59, 60, 61, 63}
LIMIT = 64
WORD = re.compile(r'0x([0-9a-fA-F]{8})(?![0-9a-fA-F])')


def main():
    failures = []
    for rel in FORBIDDEN:
        if (ROOT / rel).exists():
            failures.append(f'{rel} must not exist')
    for path in ROOT.rglob('translated_kernel.h'):
        if '.git' not in path.parts and 'build' not in path.parts:
            failures.append(f'{path.relative_to(ROOT)} must not exist')
    for path in sorted((ROOT / 'runtime' / 'sdk').rglob('*')):
        if not path.is_file():
            continue
        text = path.read_text(encoding='utf-8', errors='ignore')
        count = sum(1 for m in WORD.finditer(text) if int(m.group(1), 16) >> 26 in COMMON_OPCODES)
        if count > LIMIT:
            failures.append(f'{path.relative_to(ROOT)}: {count} instruction-like 32-bit constants '
                            f'(limit {LIMIT}); store fingerprints (runtime/sdk/word_proof.h)')
    signature = (ROOT / 'runtime' / 'sdk' / 'structural_signature.h').read_text(encoding='utf-8')
    bits = re.search(r'kStructuralWordHashBits\s*=\s*(\d+)', signature)
    if not bits or int(bits.group(1)) > 8:
        failures.append('runtime/sdk/structural_signature.h: per-instruction hashes must be at most '
                        '8 bits wide (kStructuralWordHashBits)')
    for failure in failures:
        print(failure, file=sys.stderr)
    if not failures:
        print('clean-room policy: OK')
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
