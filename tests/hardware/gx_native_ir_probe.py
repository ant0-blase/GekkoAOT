"""Synthetic real-GPU oracle for SDK/FIFO matrix order and partial XF writes.

Run in separate processes with GEKKOAOT_NATIVE_GX_FIFO_IR=0 and =1.
No game assets, renderer-state inspection, or assumed camera/world transform.
"""
import ctypes as c
import struct
import time
from gx_interpolation_probe import main


def verify(gx, rect, frame, pixel, memory):
    get = gx.gekkoaot_native_gx_get_graphics_state
    get.argtypes = [c.c_void_p, c.c_uint32, c.POINTER(c.c_uint32)]
    get.restype = c.c_bool
    extent = (c.c_uint32 * 4)()
    for size in (104, 112, 116):  # pre-HUD, pre-unlock, current ABI
        guarded = (c.c_ubyte * 128)(*([0xa5] * 128))
        assert get(guarded, size, extent)
        assert bytes(guarded)[size:] == bytes([0xa5]) * (128-size)
    for size in (0, 1, 103, 105, 128):
        guarded = (c.c_ubyte * 128)(*([0xa5] * 128))
        assert not get(guarded, size, extent)
        assert bytes(guarded) == bytes([0xa5]) * 128

    class SdkCommand(c.Structure):
        _fields_ = [(name, c.c_uint32) for name in ('offset', 'kind', 'a', 'b', 'c')]

    sdk = gx.gekkoaot_native_gx_write_sdk_commands_v317
    sdk.argtypes = [c.c_void_p, c.c_uint32, c.c_uint32, c.c_void_p, c.c_uint32]
    burst = gx.gekkoaot_native_gx_write_burst

    def raw(data):
        buf = c.create_string_buffer(data)
        burst(buf, len(data), 0x80004000)

    snapshot_width = 64
    def latch():
        def bp(reg, value):
            raw(b'\x61' + struct.pack('>I', reg << 24 | value))
        blocks = (snapshot_width + 15) // 16
        bp(0x49, 0); bp(0x4a, (snapshot_width-1) | 63 << 10); bp(0x4b, 0x40000 >> 5)
        bp(0x4d, blocks); bp(0x4e, 256); bp(0x52, 1 << 14)
        gx.gekkoaot_native_gx_present_xfb_ex(0x40000, 0, snapshot_width, blocks*32, 64)

    def samples():
        values = []
        for x in (4, 24, 56):
            value = c.c_uint32()
            gx.gekkoaot_native_gx_peek_argb(x, 32, c.byref(value))
            values.append(value.value & 0xffffff)
        return values

    def matrix(tx, chunk):
        data = b'\x10' + struct.pack('>I', 11 << 16) + struct.pack(
            '>12f', 1, 0, 0, tx, 0, 1, 0, 0, 0, 0, 1, 0)
        command = SdkCommand(0, 1, 0, 12, 0)
        for offset in range(0, len(data), chunk):
            part = data[offset:offset+chunk]
            buf = c.create_string_buffer(part)
            if offset == 0:
                sdk(buf, len(part), 0x80004000, c.byref(command), 1)
            else:
                burst(buf, len(part), 0x80004000)

    def observe(tx):
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            matrix(0, 53)
            rect(0, 0, 64, 64, 0)
            # An unaligned one-word XF write changes only translation X. It
            # must share state with the preceding typed SDK whole upload.
            raw(b'\x10' + struct.pack('>I', 3) + struct.pack('>f', tx))
            rect(0, 0, 32, 64, 2)
            samples(); latch()
            time.sleep(.03)
            left, middle, right = samples()
            is_red = lambda p: (p >> 16 & 255) > 200 and (p >> 8 & 255) < 40 and (p & 255) < 40
            is_black = lambda p: (p & 0xffffff) < 0x080808
            if is_black(left) and is_red(middle) and is_black(right):
                return
        raise AssertionError(f'partial XF translation: {left:06x}/{middle:06x}/{right:06x}')

    for chunk in (1, 5, 17, 53):
        # Complete fragmented SDK uploads before later raw writes/draws. Check
        # the rendered result rather than reconstructing the original packet.
        matrix(0, chunk)
        rect(0, 0, 64, 64, 0)
        matrix(.5, chunk)
        rect(0, 0, 32, 64, 2)
        samples(); latch()
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            latch()
            time.sleep(.03)
            left, middle, _ = samples()
            if left < 0x080808 and (middle >> 16 & 255) > 200 and (middle & 65535) < 0x2828:
                break
        else:
            raise AssertionError(f'SDK matrix chunk={chunk}: {left:06x}/{middle:06x}')
    observe(.5)
    # The 65-pixel logical framebuffer requires 252 bytes of padding per row
    # in the raster readback. A wrong stride reads a different row at y=32.
    snapshot_width = 65
    observe(.5)
    def pixel_state(reg, value, chunk):
        data = b'\x61' + struct.pack('>I', reg << 24 | value)
        command = SdkCommand(0, 2, reg, value, 0)
        for offset in range(0, len(data), chunk):
            part = data[offset:offset+chunk]
            buffer = c.create_string_buffer(part)
            if offset == 0:
                sdk(buffer, len(part), 0x80004000, c.byref(command), 1)
            else:
                burst(buffer, len(part), 0x80004000)

    # A typed SDK BP write shares masks and state with ordinary FIFO writes.
    # Its transport may straddle any gather boundary, but is applied once.
    for chunk in (1, 2, 5):
        for enabled in (False, True):
            deadline = time.monotonic() + 5
            while time.monotonic() < deadline:
                matrix(0, 53)
                pixel_state(0x41, 0x18, chunk)
                rect(0, 0, 64, 64, 0)
                pixel_state(0x41, 0x18 if enabled else 0x10, chunk)
                rect(0, 0, 64, 64, 2)
                samples(); latch(); time.sleep(.03)
                values = samples()
                expected = 0xff0000 if enabled else 0
                if values == [expected] * 3:
                    break
            else:
                raise AssertionError(f'SDK pixel chunk={chunk} color={enabled}: {values}')
    print('GX_NATIVE_IR_PIXEL_PIXELS_PASSED chunks=1,2,5 color_write_masks', flush=True)
    print('GX_NATIVE_IR_MATRIX_PIXELS_PASSED chunks=1,5,17,53 partial_xf=1 padded_width=65', flush=True)


if __name__ == '__main__':
    main(verify)
