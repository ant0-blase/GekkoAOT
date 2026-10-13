"""Check PE raster bounds independently of retail games with native GPU draws."""
import ctypes as c
import ctypes.util
from pathlib import Path
import os
import struct


def main():
    root = Path(__file__).resolve().parents[2]
    if sdl := ctypes.util.find_library('SDL3'):
        c.CDLL(sdl, mode=c.RTLD_GLOBAL)
    gx = c.CDLL(os.environ.get('GEKKOAOT_TEST_GX_BRIDGE',
                              str(root / '.gekkoaot/build/NativeGX-Aurora/libgekkoaot-native-gx.so')))
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
    gx.gekkoaot_native_gx_read_bbox.argtypes = [c.POINTER(c.c_uint16)]
    gx.gekkoaot_native_gx_read_bbox.restype = c.c_bool
    assert gx.gekkoaot_native_gx_init(resolve, None)
    gx.gekkoaot_native_gx_set_aspect_mode(0)

    def write(data):
        for i in range(0, len(data), 4096):
            chunk = data[i:i+4096]
            buf = c.create_string_buffer(chunk)
            gx.gekkoaot_native_gx_write_burst(buf, len(chunk), 0x80004000)

    def bp(reg, value):
        write(b'\x61' + struct.pack('>I', reg << 24 | value))

    def cp(reg, value):
        write(b'\x08' + bytes([reg]) + struct.pack('>I', value))

    def xf(reg, values):
        write(b'\x10' + struct.pack('>I', (len(values)-1) << 16 | reg) +
              b''.join(struct.pack('>f', value) if isinstance(value, float)
                       else struct.pack('>I', value) for value in values))

    cp(0x50, 1 << 9); cp(0x60, 1); cp(0x70, 9 | (1 << 21) | (4 << 22))
    xf(0, [1., 0., 0., 0., 0., 1., 0., 0., 0., 0., 1., 0.])
    xf(0x1008, [1 << 4, 0]); xf(0x1018, [60 << 6]); xf(0x103f, [1])
    xf(0x1040, [5 << 7]); xf(0x1050, [61])
    xf(0x101a, [32., -32., 16777215., 374., 374., 16777215.])
    xf(0x1020, [1., 0., 1., 0., 1., 0., 1])
    bp(0, 1); bp(0x59, 171 | 171 << 10); bp(0x40, 0)
    bp(0x41, 0x18); bp(0x43, 0); bp(0xf3, 7 | 7 << 16 | 7 << 19)
    bp(0x28, 1 << 6); bp(0xc0, 0xfff8 | 1 << 19)
    bp(0xc1, 7 << 13 | 7 << 10 | 7 << 7 | 4 << 4 | 1 << 19)
    bp(0x30, 7); bp(0x31, 7); bp(0x80, 0); bp(0x84, 0)
    c.memset(c.addressof(memory) + 0x10000, 0xff, 32)
    c.memmove(c.addressof(memory) + 0x20000, bytes(30) + struct.pack('>H', 0xffff), 32)
    bp(0x88, 7 | 7 << 10 | 8 << 20); bp(0x94, 0x10000 >> 5)
    bp(0x64, 0x20000 >> 5); bp(0x65, 0x200 | 1 << 10); bp(0x98, 0x200 | 1 << 10)

    def scissor(left=0, right=63, top=0, bottom=63):
        bp(0x20, 342+top | (342+left) << 12)
        bp(0x21, 342+bottom | (342+right) << 12)

    def reset(left=1023, right=0, top=1023, bottom=0):
        bp(0x55, left | right << 10)
        bp(0x56, top | bottom << 10)

    def quad(low=-.5, high=.5, depth=-.5):
        vertices = [(low, low, depth, 0., 0.), (high, low, depth, 1., 0.),
                    (high, high, depth, 1., 1.), (low, high, depth, 0., 1.)]
        write(b'\x80' + struct.pack('>H', 4) + b''.join(struct.pack('>5f', *v) for v in vertices))

    def check(expected):
        bounds = (c.c_uint16 * 4)()
        assert gx.gekkoaot_native_gx_read_bbox(bounds)
        assert tuple(bounds) == expected, (tuple(bounds), expected)

    scissor()
    reset(); quad(); check((16, 47, 16, 47))
    # Reading stops accumulation; later draws leave all four cached lanes intact.
    quad(-1., 1.); check((16, 47, 16, 47))
    reset(); quad(-.5, .5); quad(-1., 0.); check((0, 47, 16, 63))
    # A reset with no surviving fragments must preserve the written sentinel.
    reset(); bp(0xf3, 0); quad(); check((1023, 0, 1023, 0))
    bp(0xf3, 7 | 7 << 16 | 7 << 19)
    scissor(20, 39, 22, 41); reset(); quad(-1., 1.); check((20, 39, 22, 41))
    scissor(); reset(17, 46, 19, 44); check((17, 46, 19, 44))
    # BP mask semantics apply before decoding either pair.
    bp(0xfe, 0x3ff); bp(0x55, 21); check((21, 46, 19, 44))
    reset(); quad(-.875, -.84375); check((4, 5, 58, 59))
    if os.environ.get('GEKKOAOT_TEST_BBOX_DEPTH') == '1':
        # Native Z test reference: write a small rectangle, then an equal-depth
        # large rectangle. Only the original rectangle survives GX_EQUAL.
        bp(0x40, 1 | 7 << 1 | 1 << 4)
        quad()
        reset(); bp(0x40, 1 | 2 << 1); quad(-1., 1.)
        check((16, 47, 16, 47))
    print('GX_BOUNDING_BOX_RASTER_PASSED', flush=True)
    gx.gekkoaot_native_gx_shutdown()
    print('GX_BOUNDING_BOX_SHUTDOWN_PASSED', flush=True)


if __name__ == '__main__':
    main()
