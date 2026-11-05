"""Real GPU and PE oracle for typed state lists, cache reuse and mutation."""
import ctypes as c
import struct
import time
from gx_interpolation_probe import main


def verify(gx, rect, unused_frame, pixel, memory):
    def bp(reg, value):
        return b'\x61' + struct.pack('>I', reg << 24 | value)

    def cp(reg, value):
        return b'\x08' + bytes([reg]) + struct.pack('>I', value)

    def xf(reg, values):
        return b'\x10' + struct.pack('>I', (len(values)-1) << 16 | reg) + b''.join(
            struct.pack('>f', v) if isinstance(v, float) else struct.pack('>I', v) for v in values)

    def write(data):
        buffer = c.create_string_buffer(data)
        gx.gekkoaot_native_gx_write_burst(buffer, len(data), 0x80004000)

    def identity(tx=0.):
        return xf(0, [1., 0., 0., tx, 0., 1., 0., 0., 0., 0., 1., 0.])

    def frame():
        write(bp(0x49, 0) + bp(0x4a, 63 | 63 << 10) + bp(0x4b, 0x40000 >> 5) +
              bp(0x4d, 4) + bp(0x4e, 256) + bp(0x52, 1 << 14))
        gx.gekkoaot_native_gx_present_xfb_ex(0x40000, 0, 64, 128, 64)

    events = []
    callback_type = c.CFUNCTYPE(None, c.c_void_p, c.c_uint32, c.c_uint32)
    callback = callback_type(lambda user, reg, word: events.append((reg, word)))
    gx._native_state_callback = callback
    gx.gekkoaot_native_gx_set_pe_callback.argtypes = [callback_type, c.c_void_p]
    gx.gekkoaot_native_gx_set_pe_callback(callback, None)

    def call(data, address=0x60000):
        before = len(events)
        c.memmove(c.addressof(memory) + address, data, len(data))
        write(b'\x40' + struct.pack('>II', address, len(data)))
        assert len(events) == before + 1, (before, events)
        assert events[-1] == (0x48, 0x1234), events[-1]

    def state_list(tx):
        return (cp(0x50, 1 << 9) + cp(0x60, 1) + cp(0x70, 9 | 1 << 21 | 4 << 22) +
                identity(tx) + xf(0x101a, [32., -32., 16777215., 374., 374., 16777215.]) +
                xf(0x1020, [1., 0., 1., 0., 1., 0., 1]) +
                xf(0x103f, [1]) + xf(0x1040, [5 << 7]) +
                bp(0xfe, 0x18) + bp(0x41, 0x18) + bp(0x66, 0) + bp(0x48, 0x1234))

    def samples():
        values = []
        for x in (4, 24, 56):
            value = c.c_uint32()
            gx.gekkoaot_native_gx_peek_argb(x, 32, c.byref(value))
            values.append(value.value & 0xffffff)
        return values

    def render(tx, expected):
        data = state_list(tx)
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            write(identity()); rect(0, 0, 64, 64, 0)
            call(data); rect(0, 0, 32, 64, 2)
            samples(); frame(); time.sleep(.03)
            values = samples()
            if all((v == 0xff0000) if red else (v == 0) for v, red in zip(values, expected)):
                # Guaranteed hit independent of asynchronous snapshot cadence.
                call(data); call(data)
                return
        raise AssertionError(f'state list tx={tx}: {values}, expected red flags {expected}')

    render(.5, (False, True, False))
    render(-.5, (True, False, False))  # exact same address/size, changed guest bytes
    render(.5, (False, True, False))
    # A list with a draw cannot be pretranslated independently of the live VAT.
    # Its token prefix must execute ONCE, never once during planning then again
    # in the compatible path. The legal empty GXBegin draw is still consumed.
    uncached = bp(0x48, 0x1234) + b'\x80\x00\x00'
    call(uncached, 0x61000); call(uncached, 0x61000)
    # Copies and PE commands retain their order and execute on every hit.
    copy = bp(0x52, 1 << 14) + bp(0x48, 0x1234)
    gx.gekkoaot_native_gx_poke_efb.argtypes = [c.c_uint16, c.c_uint16, c.c_uint32, c.c_bool]
    gx.gekkoaot_native_gx_poke_efb.restype = c.c_bool
    for color, expected in ((2, 0xff0000), (3, 0x00ff00), (2, 0xff0000)):
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            write(identity()); rect(0, 0, 64, 64, color)
            samples()  # request the snapshot BEFORE the cached copy
            assert gx.gekkoaot_native_gx_poke_efb(24, 32, 0xff0000ff, False)
            call(copy, 0x62000)
            gx.gekkoaot_native_gx_present_xfb_ex(0x40000, 0, 64, 128, 64)
            time.sleep(.03)
            values = samples()
            if values == [expected, 0x0000ff, expected]:
                break
        else:
            raise AssertionError(f'cached copy/poke: {values}, expected {expected:06x},0000ff,{expected:06x}')
    print('GX_NATIVE_STATE_PIXELS_PASSED projection viewport vcd vat masks tokens invalidation copy mutable_lists', flush=True)


if __name__ == '__main__':
    main(verify)
