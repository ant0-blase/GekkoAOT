"""Exercise BP mask consumption across a native GX display-copy intercept.

This probe uses the public bridge ABI and emits real BP FIFO words. The parent
test checks both a masked destination write and the first unmasked write after
an intercepted display-copy trigger.
"""
import ctypes as c
import ctypes.util
from pathlib import Path
import struct


def main():
    root = Path(__file__).resolve().parents[2]
    sdl = ctypes.util.find_library('SDL3')
    if sdl:
        c.CDLL(sdl, mode=c.RTLD_GLOBAL)
    lib = c.CDLL(str(root / '.gekkoaot/build/NativeGX-Aurora/libgekkoaot-native-gx.so'))
    memory = (c.c_uint8 * (24 * 1024 * 1024))()
    resolve_type = c.CFUNCTYPE(c.c_bool, c.c_void_p, c.c_uint32, c.c_uint32,
                              c.c_uint32, c.c_uint32, c.POINTER(c.c_void_p), c.POINTER(c.c_uint32))

    @resolve_type
    def resolve(_user, address, size, _space, _resource, data, available):
        offset = address & 0x3fffffff
        if offset > len(memory) or size > len(memory) - offset:
            return False
        data[0] = c.addressof(memory) + offset
        available[0] = len(memory) - offset
        return True

    lib.gekkoaot_native_gx_init.argtypes = [resolve_type, c.c_void_p]
    lib.gekkoaot_native_gx_init.restype = c.c_bool
    lib.gekkoaot_native_gx_write_burst.argtypes = [c.c_void_p, c.c_uint32, c.c_uint32]
    if not lib.gekkoaot_native_gx_init(resolve, None):
        raise RuntimeError('native GX initialization failed')

    def bp(register, value):
        data = b'\x61' + struct.pack('>I', (register << 24) | value)
        buffer = c.create_string_buffer(data)
        lib.gekkoaot_native_gx_write_burst(buffer, len(data), 0x80004000)

    # One-pixel source, valid MEM1 destination, 32-byte destination stride.
    bp(0x49, 0)
    bp(0x4a, 0)
    bp(0x4b, 0x10000 >> 5)
    # A zero one-shot mask must preserve the destination register. The raw
    # second write points to 0x20000, so its use by the host is observable.
    bp(0xfe, 0)
    bp(0x4b, 0x20000 >> 5)
    bp(0x4d, 1)
    bp(0x4e, 256)

    # Only the display-copy trigger bit survives this one-shot BP mask. The
    # trigger itself must consume the mask, despite being intercepted by the
    # bridge instead of going through Aurora's BP parser.
    bp(0xfe, 1 << 14)
    bp(0x52, 1 << 14)
    bp(0x4b, 0x30000 >> 5)
    bp(0x52, 1 << 14)
    lib.gekkoaot_native_gx_present()
    lib.gekkoaot_native_gx_shutdown()
    print('BP_MASK_DISPLAY_COPY_PROBE_COMPLETED')


if __name__ == '__main__':
    main()
