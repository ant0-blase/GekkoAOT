"""Real GPU sparse INDEX16 draws: mutation, aliases, matrices and rollover.

Run in independent processes with GEKKOAOT_NATIVE_GX_COMPACT_ARRAY_WINDOWS=0/1.
The sparse fixture deliberately makes its initial shader bias wrap below zero.
"""
import argparse
import ctypes as c
import ctypes.util
from pathlib import Path
import struct
import time
import os


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--bridge', type=Path, required=True)
    parser.add_argument('--stress', action='store_true')
    parser.add_argument('--resolved-batch', action='store_true', help='same resolved GPU state after SDK state changes')
    parser.add_argument('--small-arrays', action='store_true', help='INDEX8 and low INDEX16 native snapshots')
    parser.add_argument('--typed-bulk', action='store_true', help='batched native SDK command ABI')
    parser.add_argument('--typed-arrays', action='store_true', help='native SDK array command frontend')
    parser.add_argument('--direct-matrices', action='store_true', help='native SDK matrix commands')
    parser.add_argument('--batch', action='store_true', help='native indexed batches and unnotified mutations')
    parser.add_argument('--fragment-size', type=int, default=4096)
    args = parser.parse_args()
    if args.fragment_size < 1:
        parser.error('fragment-size must be positive')
    if sdl := ctypes.util.find_library('SDL3'):
        c.CDLL(sdl, mode=c.RTLD_GLOBAL)
    gx = c.CDLL(str(args.bridge.resolve()))
    memory = (c.c_uint8 * (24 * 1024 * 1024))()
    resolver = c.CFUNCTYPE(c.c_bool, c.c_void_p, c.c_uint32, c.c_uint32,
                          c.c_uint32, c.c_uint32, c.POINTER(c.c_void_p), c.POINTER(c.c_uint32))
    @resolver
    def resolve(user, address, size, space, resource, data, available):
        offset = address & 0x3fffffff
        if offset + size > len(memory):
            return False
        data[0] = c.addressof(memory) + offset
        available[0] = len(memory) - offset
        return True
    gx.gekkoaot_native_gx_init.argtypes = [resolver, c.c_void_p]
    gx.gekkoaot_native_gx_init.restype = c.c_bool
    gx.gekkoaot_native_gx_write_burst.argtypes = [c.c_void_p, c.c_uint32, c.c_uint32]
    gx.gekkoaot_native_gx_peek_argb.argtypes = [c.c_uint16, c.c_uint16, c.POINTER(c.c_uint32)]
    gx.gekkoaot_native_gx_memory_write.argtypes = [c.c_uint32, c.c_uint32]
    assert gx.gekkoaot_native_gx_init(resolve, None)
    if args.typed_arrays:
        gx.gekkoaot_native_gx_sdk_array.argtypes = [c.c_uint32]*4
    def write(data):
        for start in range(0, len(data), args.fragment_size):
            chunk = data[start:start + args.fragment_size]
            buf = c.create_string_buffer(chunk)
            gx.gekkoaot_native_gx_write_burst(buf, len(chunk), 0x80004000)
    def bp(reg, value):
        write(b'\x61' + struct.pack('>I', reg << 24 | value))
    def cp(reg, value):
        write(b'\x08' + bytes([reg]) + struct.pack('>I', value))
    def array(index, base, stride):
        if args.typed_bulk:
            packet = b'\x08' + bytes([0xa0+index]) + struct.pack('>I', base & 0x3fffffff) + b'\x08' + bytes([0xb0+index]) + struct.pack('>I', stride & 255)
            data = c.create_string_buffer(packet)
            command = (c.c_uint32*4)(0,index,base,stride)
            gx.gekkoaot_native_gx_write_commands.argtypes = [c.c_void_p,c.c_uint32,c.c_uint32,c.c_void_p,c.c_uint32]
            n=min(args.fragment_size,len(packet))
            gx.gekkoaot_native_gx_write_commands(data,n,0x80004000,command,1)
            if n<len(packet): write(packet[n:])
            return
        if args.typed_arrays:
            gx.gekkoaot_native_gx_sdk_array(index, base, stride, 0)
        cp(0xa0 + index, base); cp(0xb0 + index, stride)
    def xf(reg, values):
        if args.direct_matrices and reg < 0x100 and reg % 4 == 0 and len(values) == 24:
            xf(reg, values[:12]); xf(reg + 12, values[12:]); return
        packet = b'\x10' + struct.pack('>I', (len(values) - 1) << 16 | reg) + \
              b''.join(struct.pack('>f', value) if isinstance(value, float)
                       else struct.pack('>I', value) for value in values)
        count = len(values)
        direct = ((reg < 0x100 and reg % 4 == 0 and count in (8, 12) and reg + count <= 0x100) or
                  (0x400 <= reg < 0x460 and (reg - 0x400) % 3 == 0 and count == 9 and reg + count <= 0x460) or
                  (0x500 <= reg < 0x5f0 and (reg - 0x500) % 12 == 0 and count == 12 and reg + count <= 0x5f0))
        if args.direct_matrices and direct:
            command = (c.c_uint32*5)(0, 1, reg, count, 0)
            gx.gekkoaot_native_gx_write_sdk_commands_v317.argtypes = [c.c_void_p,c.c_uint32,c.c_uint32,c.c_void_p,c.c_uint32]
            data = c.create_string_buffer(packet)
            n = min(args.fragment_size, len(packet))
            gx.gekkoaot_native_gx_write_sdk_commands_v317(data, n, 0x80004000, command, 1)
            if n < len(packet): write(packet[n:])
        else:
            write(packet)
    base, uvbase = 0x30000, 0x100000
    def positions(right, first=65532):
        x0, x1 = (0., 1.) if right else (-1., 0.)
        values = [(x0, -1., -.5), (x1, -1., -.5), (x1, 1., -.5), (x0, 1., -.5)]
        payload = b''.join(struct.pack('>3f', *value) for value in values)
        c.memmove(c.addressof(memory) + base + first * 12, payload, len(payload))
    def quad(first=65532, matrix=0, dense=False):
        texindices = [0, 65535, 65535, 0] if dense else [65532, 65533, 65534, 65535]
        payload = b''.join(struct.pack('>BHH', matrix, first + i, texindices[i]) for i in range(4))
        write(b'\x80\x00\x04' + payload)
    def frame():
        bp(0x49, 0); bp(0x4a, 63 | (63 << 10)); bp(0x4b, 0x40000 >> 5)
        bp(0x4d, 4); bp(0x4e, 256); bp(0x52, 1 << 14)
        gx.gekkoaot_native_gx_present_xfb_ex(0x40000, 0, 64, 128, 64)
    def color(blue):
        bp(0x64, (0x20020 if blue else 0x20000) >> 5)
        bp(0x65, 0x200 | (1 << 10))
    left, right = c.c_uint32(), c.c_uint32()
    def check(expected, draw=None):
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            if draw:
                draw()
            gx.gekkoaot_native_gx_peek_argb(16, 32, c.byref(left))
            gx.gekkoaot_native_gx_peek_argb(48, 32, c.byref(right))
            frame()
            actual = left.value, right.value
            if all(want is None or want == got for want, got in zip(expected, actual)):
                return
            time.sleep(.01)
        raise AssertionError(f'GPU pixels {actual!r}, expected {expected!r}')
    try:
        # Per-vertex matrix index is direct; both position and UV use raw u16
        # indices near 65535. No compact path may rewrite the draw payload.
        cp(0x50, 1 | (3 << 9)); cp(0x60, 3)
        cp(0x70, 9 | (1 << 21) | (4 << 22))
        array(0, base, 12); array(4, uvbase, 8)
        xf(0, [1., 0., 0., 0., 0., 1., 0., 0., 0., 0., 1., 0.,
               1., 0., 0., 1., 0., 1., 0., 0., 0., 0., 1., 0.])
        xf(0x1008, [1 << 4, 0]); xf(0x1018, [60 << 6])
        xf(0x103f, [1]); xf(0x1040, [5 << 7]); xf(0x1050, [61])
        xf(0x101a, [32., -32., 16777215., 374., 374., 16777215.])
        xf(0x1020, [1., 0., 1., 0., 1., 0., 1])
        bp(0, 1); bp(0x20, 342 | 342 << 12); bp(0x21, 405 | 405 << 12)
        bp(0x59, 171 | (171 << 10)); bp(0x40, 0); bp(0x41, 0x18); bp(0x43, 0)
        bp(0xf3, 7 | (7 << 16) | (7 << 19)); bp(0x28, 1 << 6)
        bp(0xc0, 0xfff8 | (1 << 19))
        bp(0xc1, (7 << 13) | (7 << 10) | (7 << 7) | (4 << 4) | (1 << 19))
        bp(0x30, 7); bp(0x31, 7); bp(0x80, 0); bp(0x84, 0)
        c.memset(c.addressof(memory) + 0x10000, 0xff, 32)
        c.memmove(c.addressof(memory) + 0x20000, bytes(30) + struct.pack('>H', (22 << 11) | (16 << 5) | 15), 32)
        c.memmove(c.addressof(memory) + 0x20020, bytes(30) + struct.pack('>H', (15 << 11) | (31 << 5) | 22), 32)
        bp(0x88, 7 | (7 << 10) | (8 << 20)); bp(0x94, 0x10000 >> 5)
        bp(0x98, 0x200 | (1 << 10))
        for index, uv in zip(range(65532, 65536), [(0., 0.), (1., 0.), (1., 1.), (0., 1.)]):
            c.memmove(c.addressof(memory) + uvbase + index * 8, struct.pack('>2f', *uv), 8)
        positions(False); color(False); quad(); frame()
        if args.direct_matrices:
            # Real pixel proof for an SDK upload, its mutation and subsequent
            # raw/indexed matrix operations. Also exercise native normal/post
            # banks before shaders consume them later in this fixture.
            identity = [1., 0., 0., 0., 0., 1., 0., 0., 0., 0., 1., 0.]
            moved = list(identity); moved[3] = 1.
            xf(0, moved); color(True); quad(); frame()
            check((None, 0xff7b7db5), quad)
            xf(0, identity); color(False); quad(); frame()
            check((0xffb5417b, None), quad)
            xf(0x400, [1., 0., 0., 0., 1., 0., 0., 0., 1.])
            xf(0x500, identity)
        color(False); quad()
        positions(True)
        gx.gekkoaot_native_gx_memory_write(base + 65532 * 12, 48)
        color(True); quad(); frame()
        check((0xffb5417b, 0xff7b7db5))
        # Mutation immediately after a compact draw must preserve its snapshot.
        # Then change min/max while keeping the same binding/window high-water.
        positions(False, 700); gx.gekkoaot_native_gx_memory_write(base + 700 * 12, 48)
        color(False); quad(700); frame(); check((0xffb5417b, None), lambda: quad(700))
        # A per-vertex matrix moves that unchanged left-hand source to the right.
        color(True); quad(700, 3); frame(); check((None, 0xff7b7db5), lambda: quad(700, 3))
        # Same bytes at a different guest base, then a mutation through that base.
        alias = 0x200000
        c.memmove(c.addressof(memory) + alias, c.addressof(memory) + base, 786432)
        base = alias; cp(0xa0, base)
        positions(True); color(True); quad(); frame(); check((None, 0xff7b7db5), quad)
        positions(False); gx.gekkoaot_native_gx_memory_write(base + 65532 * 12, 48)
        color(False); quad(); frame(); check((0xffb5417b, None), quad)
        if args.stress:
            # Dense UV indices defeat compaction and fill transient storage.
            # Sparse position windows must stay exact across its rollover.
            c.memmove(c.addressof(memory) + uvbase, struct.pack('>2f', 0., 0.), 8)
            c.memmove(c.addressof(memory) + uvbase + 65535 * 8, struct.pack('>2f', 1., 1.), 8)
            color(True); positions(True)
            for draw in range(350):
                # Change an unused, mapped UV element each time. Identical
                # dense snapshots would otherwise be reused by the existing
                # persistent content cache and never exercise real rollover.
                c.memmove(c.addressof(memory) + uvbase + 100 * 8,
                          struct.pack('>2f', draw / 1000., 0.), 8)
                write(b'\x48')
                quad(dense=True)
            frame(); check((None, 0xff7b7db5), lambda: quad(dense=True))
        if args.small_arrays:
            # Low indices never enter suffix compaction. Verify full snapshots
            # for both widths, including recycling RAM without notifications.
            saved_base = base
            base = 0x600000; cp(0xa0, base)
            for index8 in (False, True):
                cp(0x50, 1 | ((2 if index8 else 3) << 9))
                def small_quad():
                    payload = b''.join(struct.pack('>BBH' if index8 else '>BHH',
                                                   0, 200 + i, 65532 + i) for i in range(4))
                    write(b'\x80\x00\x04' + payload)
                positions(False, 200); color(False); small_quad(); frame()
                check((0xffb5417b, None), small_quad)
                positions(False, 200); color(False); small_quad()
                positions(True, 200); color(True); small_quad(); frame()
                check((0xffb5417b, 0xff7b7db5))
                for _ in range(100): small_quad()
                frame(); check((None, 0xff7b7db5))
            base = saved_base; cp(0xa0, base)
            cp(0x50, 1 | (3 << 9))
            print('GX_SMALL_ARRAY_PIXELS_PASSED index8=1 low_index16=1 unnotified_mutation=1')
        # NBT3 has three distinct XYZ indices, not one nine-component element.
        # RGBA6 with odd stride exercises an unaligned first/last component and
        # the compact window's align-down prefix on the actual GPU shader.
        nrmbase, clrbase = 0x300000, 0x400000
        cp(0x50, 1 | (3 << 9) | (3 << 11) | (3 << 13))
        cp(0x70, 9 | (1 << 9) | (4 << 10) | (1 << 13) | (4 << 14) |
                  (1 << 21) | (4 << 22) | (1 << 31))
        array(1, nrmbase, 12); array(2, clrbase, 7)
        xf(0x1008, [1 | (2 << 2) | (1 << 4), 1])
        xf(0x100e, [1]); xf(0x1010, [1])
        bp(0, 1 | (1 << 4))
        bp(0xc0, 0xfffa | (1 << 19))
        bp(0xc1, (7 << 13) | (7 << 10) | (7 << 7) | (5 << 4) | (1 << 19))
        for index in (65000, 65002, 65007):
            c.memmove(c.addressof(memory) + nrmbase + index * 12,
                      struct.pack('>3f', 0., 0., 1.), 12)
        def raster_quad(red=False, matrix=0):
            rgba6 = ((63 << 18) if red else (63 << 12)) | 63
            for i in range(4):
                c.memmove(c.addressof(memory) + clrbase + (64999 + i) * 7,
                          rgba6.to_bytes(3, 'big'), 3)
            gx.gekkoaot_native_gx_memory_write(clrbase + 64999 * 7, 24)
            payload = b''.join(struct.pack('>B6H', matrix, 65532 + i, 65002, 65000,
                                           65007, 64999 + i, 65532 + i)
                               for i in range(4))
            write(b'\x80\x00\x04' + payload)
        positions(True); raster_quad(); frame()
        check((None, 0xff00ff00), raster_quad)
        raster_quad(True); frame()
        check((None, 0xffff0000), lambda: raster_quad(True))
        # LOAD_INDX_A reads its original CPU array at a high index, independent
        # of the per-draw GPU biases installed for vertex attributes.
        matrixbase, matrixindex = 0x500000, 65500
        array(12, matrixbase, 48)
        def load_matrix(translation):
            matrix = [1., 0., 0., translation, 0., 1., 0., 0., 0., 0., 1., 0.]
            c.memmove(c.addressof(memory) + matrixbase + matrixindex * 48,
                      struct.pack('>12f', *matrix), 48)
            write(b'\x20' + struct.pack('>HH', matrixindex, (11 << 12) | 12))
        load_matrix(-1.); raster_quad(True, 3); frame()
        check((0xffff0000, None), lambda: raster_quad(True, 3))
        load_matrix(0.); raster_quad(True, 3); frame()
        check((None, 0xffff0000), lambda: raster_quad(True, 3))
        if args.batch:
            # Restore position/UV-only format and identity matrix. Many indexed
            # primitives consume the same immutable resource. No state commands
            # intervene, so their native draw must be mergeable after coherence
            # has been proved (the old unconditional DirtyImmediates prevented it).
            cp(0x50, 1 | (3 << 9)); cp(0x70, 9 | (1 << 21) | (4 << 22))
            xf(0x1008, [1 << 4, 0]); xf(0x100e, [0]);
            bp(0, 1)
            bp(0xc0, 0xfff8 | (1 << 19))
            bp(0xc1, (7 << 13) | (7 << 10) | (7 << 7) | (4 << 4) | (1 << 19))
            xf(0, [1., 0., 0., 0., 0., 1., 0., 0., 0., 0., 1., 0.,
                   1., 0., 0., 1., 0., 1., 0., 0., 0., 0., 1., 0.])
            cp(0xa0, base + 16); cp(0xa0, base)
            positions(False); color(False); quad(); frame()
            check((0xffb5417b, None), quad)
            # Mutate without GXInvalidateVtxCache OR a guest write notification.
            # The first left-hand snapshot must survive; the second must draw
            # on the right. Then make many unchanged repetitions eligible.
            positions(False); quad()
            positions(True); quad()
            for _ in range(200): quad()
            frame(); check((0xffb5417b, 0xffb5417b))
            # Current/per-vertex matrices and format transitions cannot be
            # elided merely because array contents happen to be unchanged.
            color(True); positions(False); quad(matrix=3); frame()
            check((None, 0xff7b7db5), lambda: quad(matrix=3))
            color(False); positions(False)
            for _ in range(125):
                for _ in range(200):
                    if args.resolved_batch:
                        xf(0, [1., 0., 0., .125, 0., 1., 0., 0., 0., 0., 1., 0.])
                        xf(0, [1., 0., 0., 0., 0., 1., 0., 0., 0., 0., 1., 0.])
                    quad()
                frame()
            check((0xffb5417b, None))
            print('GX_NATIVE_BATCH_PIXELS_PASSED repeats=25000 unnotified_mutation=1 matrix_boundary=1')
        print('GX_COMPACT_ARRAY_PIXELS_PASSED sparse_indices=65532..65535 mutation=1 alias=1 matrix=direct+indexed nbt3=1 odd_stride_rgba6=1 rollover=' + str(int(args.stress)))
    finally:
        gx.gekkoaot_native_gx_shutdown()


if __name__ == '__main__':
    repeats = int(os.environ.get('GEKKOAOT_TEST_GX_RESTARTS', '1'))
    assert 1 <= repeats <= 3
    for _ in range(repeats):
        main()
