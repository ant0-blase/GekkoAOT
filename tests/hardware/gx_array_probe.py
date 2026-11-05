"""Check real GPU output when persistent GX arrays are reused then mutated."""
import argparse
import ctypes as c
import ctypes.util
from pathlib import Path
import struct
import time


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--bridge', type=Path)
    parser.add_argument('--fragment-size', type=int, default=4096)
    args = parser.parse_args()
    if args.fragment_size < 1:
        parser.error('--fragment-size must be positive')
    root = Path(__file__).resolve().parents[2]
    if sdl := ctypes.util.find_library('SDL3'):
        c.CDLL(sdl, mode=c.RTLD_GLOBAL)
    gx = c.CDLL(str(args.bridge or root / '.gekkoaot/build/NativeGX-Aurora/libgekkoaot-native-gx.so'))
    memory = (c.c_uint8 * (24 * 1024 * 1024))()
    resolver = c.CFUNCTYPE(c.c_bool, c.c_void_p, c.c_uint32, c.c_uint32, c.c_uint32,
                          c.c_uint32, c.POINTER(c.c_void_p), c.POINTER(c.c_uint32))
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

    def write(data):
        for start in range(0, len(data), args.fragment_size):
            chunk = data[start:start + args.fragment_size]
            buf = c.create_string_buffer(chunk)
            gx.gekkoaot_native_gx_write_burst(buf, len(chunk), 0x80004000)
    def bp(reg, value):
        write(b'\x61' + struct.pack('>I', reg << 24 | value))
    def cp(reg, value):
        write(b'\x08' + bytes([reg]) + struct.pack('>I', value))
    def xf(reg, values):
        write(b'\x10' + struct.pack('>I', (len(values) - 1) << 16 | reg) +
              b''.join(struct.pack('>f', value) if isinstance(value, float)
                       else struct.pack('>I', value) for value in values))

    try:
        cp(0x50, 3 << 9); cp(0x60, 1); cp(0x70, 9 | (1 << 21) | (4 << 22))
        cp(0xa0, 0x30000); cp(0xb0, 12)
        xf(0, [1., 0., 0., 0., 0., 1., 0., 0., 0., 0., 1., 0.])
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

        def color(blue):
            bp(0x64, (0x20020 if blue else 0x20000) >> 5)
            bp(0x65, 0x200 | (1 << 10))
        def positions(right):
            x0, x1 = (0., 1.) if right else (-1., 0.)
            values = [(x0, -1., -.5), (x1, -1., -.5), (x1, 1., -.5), (x0, 1., -.5)]
            data = b''.join(struct.pack('>3f', *value) for value in values)
            c.memmove(c.addressof(memory) + 0x30000 + 700 * 12, data, len(data))
        def quad(first=700):
            uv = [(0., 0.), (1., 0.), (1., 1.), (0., 1.)]
            write(b'\x80' + struct.pack('>H', 4) +
                  b''.join(struct.pack('>H2f', first + i, *value) for i, value in enumerate(uv)))
        def frame():
            bp(0x49, 0); bp(0x4a, 63 | (63 << 10)); bp(0x4b, 0x40000 >> 5)
            bp(0x4d, 4); bp(0x4e, 256); bp(0x52, 1 << 14)
            gx.gekkoaot_native_gx_present_xfb_ex(0x40000, 0, 64, 128, 64)

        positions(False); color(False); quad(); frame()
        # A later frame consumes the retained persistent range without an upload.
        # Mutating it AFTER that draw must allocate another range in the same frame.
        color(False); quad()
        positions(True)
        gx.gekkoaot_native_gx_memory_write(0x30000 + 700 * 12, 48)
        color(True); quad()
        left, right = c.c_uint32(), c.c_uint32()
        gx.gekkoaot_native_gx_peek_argb(16, 32, c.byref(left))
        gx.gekkoaot_native_gx_peek_argb(48, 32, c.byref(right))
        frame()
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            gx.gekkoaot_native_gx_peek_argb(16, 32, c.byref(left))
            gx.gekkoaot_native_gx_peek_argb(48, 32, c.byref(right))
            if (left.value, right.value) == (0xffb5417b, 0xff7b7db5):
                break
            frame(); time.sleep(.01)
        else:
            raise AssertionError(f'earlier/later draw pixels: {left.value:08x}, {right.value:08x}')
        # GX_CULL_ALL is legal even though WebGPU cannot express it. A culled
        # polygon must preserve the EFB and allow subsequent ordinary draws.
        gx.gekkoaot_native_gx_peek_argb(48, 32, c.byref(right))
        before_cull = right.value
        bp(0, 1 | (3 << 14)); color(False); quad()
        gx.gekkoaot_native_gx_peek_argb(48, 32, c.byref(right))
        assert right.value == before_cull, f'culled polygon changed EFB: {right.value:08x}'
        bp(0, 1); color(True)
        # An explicit invalidation of unchanged bytes admits exact snapshot reuse.
        write(b'\x48'); quad(); frame()
        # A different guest pointer with identical bytes is also safe to reuse.
        c.memmove(c.addressof(memory) + 0x50000, c.addressof(memory) + 0x30000, 8448)
        cp(0xa0, 0x50000); quad(); frame()
        def check_right(draw):
            deadline = time.monotonic() + 5
            while time.monotonic() < deadline:
                draw()
                gx.gekkoaot_native_gx_peek_argb(48, 32, c.byref(right))
                frame()
                if right.value == 0xff7b7db5:
                    return
            raise AssertionError(f'reused snapshot pixel: {right.value:08x}')
        check_right(quad)
        # Grow the snapshot, then bind an identical shorter prefix through another
        # guest address. GPU array addressing must still select the same vertices.
        c.memmove(c.addressof(memory) + 0x50000 + 900 * 12,
                  c.addressof(memory) + 0x50000 + 700 * 12, 48)
        gx.gekkoaot_native_gx_memory_write(0x50000 + 900 * 12, 48)
        quad(900); frame()
        c.memmove(c.addressof(memory) + 0x60000, c.addressof(memory) + 0x50000, 10848)
        cp(0xa0, 0x60000)
        check_right(quad)
    finally:
        gx.gekkoaot_native_gx_shutdown()
    print('GX_ARRAY_PIXELS_PASSED earlier=ffb5417b later=ff7b7db5')


if __name__ == '__main__':
    main()
