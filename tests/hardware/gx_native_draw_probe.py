"""Primitive-list pixels, inherited transforms, mutable arrays and PE order."""
import ctypes as c
import struct
import time
from gx_interpolation_probe import main


def verify(gx, rect, frame, pixel, memory):
    def cp(reg, value):
        return b'\x08' + bytes([reg]) + struct.pack('>I', value)

    def bp(reg, value):
        return b'\x61' + struct.pack('>I', reg << 24 | value)

    def write(data):
        buffer = c.create_string_buffer(data)
        gx.gekkoaot_native_gx_write_burst(buffer, len(data), 0x80004000)

    def identity(tx):
        values = [1., 0., 0., tx, 0., 1., 0., 0., 0., 0., 1., 0.]
        write(b'\x10' + struct.pack('>I', 11 << 16) + struct.pack('>12f', *values))

    events = []
    callback_type = c.CFUNCTYPE(None, c.c_void_p, c.c_uint32, c.c_uint32)
    callback = callback_type(lambda user, reg, word: events.append((reg, word)))
    gx._draw_callback = callback
    gx.gekkoaot_native_gx_set_pe_callback.argtypes = [callback_type, c.c_void_p]
    gx.gekkoaot_native_gx_set_pe_callback(callback, None)
    address, array = 0x60000, 0x70000
    vertices = b''.join(struct.pack('>H2f', i, u, v)
                        for i, (u, v) in enumerate(((0., 0.), (1., 0.), (1., 1.), (0., 1.))))
    data = (cp(0x50, 3 << 9) + cp(0x60, 1) + cp(0x70, 9 | 1 << 21 | 4 << 22) +
            cp(0x80, 0) + cp(0x90, 0) + cp(0xa0, array) + cp(0xb0, 12) +
            bp(0x64, (0x20000 + 2 * 32) >> 5) + bp(0x65, 0x200 | 1 << 10) +
            bp(0x48, 0x1234) + b'\x80\x00\x04' + vertices + bp(0x48, 0x5678))
    c.memmove(c.addressof(memory) + address, data, len(data))
    command = b'\x40' + struct.pack('>II', address, len(data))

    def render(x0, x1, tx, expected):
        positions = ((x0, .75, -.5), (x1, .75, -.5), (x1, -.75, -.5), (x0, -.75, -.5))
        payload = b''.join(struct.pack('>3f', *p) for p in positions)
        c.memmove(c.addressof(memory) + array, payload, len(payload))
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            identity(0.)
            # The previous list left indexed descriptors active. Restore the
            # direct background layout independently of the cached list.
            write(cp(0x50, 1 << 9))
            rect(0, 0, 64, 64, 0)
            identity(tx)
            before = len(events)
            write(command)
            assert events[before:] == [(0x48, 0x1234), (0x48, 0x5678)], events[before:]
            frame()
            values = []
            for x in (12, 32, 52):
                value = c.c_uint32()
                gx.gekkoaot_native_gx_peek_argb(x, 32, c.byref(value))
                values.append(value.value & 0xffffff)
            time.sleep(.03)
            if values == expected:
                # Hit with an identical entry VCD, then mutate only array
                # contents in the next render. No cache invalidation callback.
                for _ in range(3):
                    before = len(events)
                    write(command)
                    assert events[before:] == [(0x48, 0x1234), (0x48, 0x5678)]
                return
        raise AssertionError(f'primitive list array={x0,x1} tx={tx}: {values} != {expected}')

    render(-.875, -.375, 0., [0xff0000, 0, 0])
    render(.375, .875, 0., [0, 0, 0xff0000])
    render(-.875, -.375, .625, [0, 0xff0000, 0])
    render(-.875, -.375, 0., [0xff0000, 0, 0])
    print('GX_NATIVE_DRAW_PIXELS_PASSED indexed_arrays inherited_matrices tokens mutation cache_hits', flush=True)


if __name__ == '__main__':
    main(verify)
