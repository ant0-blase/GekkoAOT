#!/usr/bin/env python3
"""Export a generated RGBA8 DDS and its preserved authored GX base mip.

Offline diagnostic only; the game has no Python/image-library dependency.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import zlib


def png(path, width, height, pixels):
    def chunk(kind, data):
        return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data))
    rows = b''.join(b'\0' + pixels[y*width*4:(y+1)*width*4] for y in range(height))
    path.write_bytes(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>2I5B', width, height, 8, 6, 0, 0, 0)) +
                     chunk(b'IDAT', zlib.compress(rows)) + chunk(b'IEND', b''))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('dds', type=Path)
    parser.add_argument('--scale', type=int, choices=(2, 4), required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    data = args.dds.read_bytes()
    if len(data) < 128 or data[:4] != b'DDS ' or struct.unpack_from('<I', data, 88)[0] != 32:
        parser.error('expected an uncompressed generated RGBA8 DDS')
    if struct.unpack_from('<I', data, 44)[0] == 0x48444d31:
        parser.error('HD appearance mips contain reconstructed RGB, not the original source; use the source GX decoder capture for before/after inspection')
    height, width = struct.unpack_from('<2I', data, 12)
    mips = struct.unpack_from('<I', data, 28)[0]
    extra = 1 if args.scale == 2 else 2
    if width % args.scale or height % args.scale or mips <= extra:
        parser.error('dimensions/mip chain do not contain the requested authored level')
    offset = 128 + sum(max(1, width >> level)*max(1, height >> level)*4 for level in range(extra))
    original_width, original_height = width//args.scale, height//args.scale
    source = data[offset:offset+original_width*original_height*4]
    if len(source) != original_width*original_height*4:
        parser.error('truncated authored mip')
    args.output.mkdir(parents=True, exist_ok=True)
    png(args.output/'original.png', original_width, original_height, source)
    png(args.output/'reconstructed.png', width, height, data[128:128+width*height*4])
    info = dict(cache=str(args.dds.resolve()), cache_sha256=hashlib.sha256(data).hexdigest(),
                original=[original_width, original_height], reconstructed=[width, height],
                authored_base_sha256=hashlib.sha256(source).hexdigest(), mips=mips,
                note='Source is the byte-preserved authored GX mip. Inspect runtime method logs to distinguish neural reconstruction from protected non-AI fallback.')
    (args.output/'texture.json').write_text(json.dumps(info, indent=2)+'\n')
    print(json.dumps(info))


if __name__ == '__main__':
    main()
