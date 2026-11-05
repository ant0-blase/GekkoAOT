#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Fingerprint for a structural word proof (runtime/sdk/word_proof.h).

Usage: tools/word_proof_fingerprint.py MASKS WORD...
  MASKS  one letter per word: F whole word, H high halfword, B branch
         opcode and AA/LK bits, - not part of the fingerprint
  WORD   big-endian instruction words in hex, read from your own executable

Prints the 64-bit hash to store next to MASKS in a WordProof.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from sdk_groundtruth_fingerprint import sequence_hash  # noqa: E402

MASKS = {'F': 0xffffffff, 'H': 0xffff0000, 'B': 0xfc000003, '-': 0}


def fingerprint(words, masks):
    if len(words) != len(masks):
        raise ValueError('one mask letter per word')
    return sequence_hash([w & MASKS[m] for w, m in zip(words, masks)])


def main():
    if len(sys.argv) < 3:
        print(__doc__, file=sys.stderr)
        return 2
    masks = sys.argv[1]
    words = [int(w, 16) for w in sys.argv[2:]]
    print(f'0x{fingerprint(words, masks):016x}ull')
    return 0


if __name__ == '__main__':
    sys.exit(main())
