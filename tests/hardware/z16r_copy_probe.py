"""Exercise the retail BP depth-copy format 0xB through the native GX ABI."""

import ctypes as c
import ctypes.util
from pathlib import Path
import struct


def main():
    root = Path(__file__).resolve().parents[2]
    sdl = ctypes.util.find_library('SDL3')
    if sdl:
        c.CDLL(sdl, mode=c.RTLD_GLOBAL)
    bridge = c.CDLL(str(root / '.gekkoaot/build/NativeGX-Aurora/libgekkoaot-native-gx.so'))
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

    bridge.gekkoaot_native_gx_init.argtypes = [resolve_type, c.c_void_p]
    bridge.gekkoaot_native_gx_init.restype = c.c_bool
    bridge.gekkoaot_native_gx_write_burst.argtypes = [c.c_void_p, c.c_uint32, c.c_uint32]
    if not bridge.gekkoaot_native_gx_init(resolve, None):
        raise RuntimeError('native GX initialization failed')

    def bp(register, value):
        data = b'\x61' + struct.pack('>I', (register << 24) | value)
        buffer = c.create_string_buffer(data)
        bridge.gekkoaot_native_gx_write_burst(buffer, len(data), 0x80004000)

    bp(0x49, 0)               # source (0, 0)
    bp(0x4a, 1 | (1 << 10))   # 2x2 source
    bp(0x4b, 0x10000 >> 5)    # MEM1 destination
    bp(0x4d, 2)               # 64-byte stride
    bp(0x43, 3)               # depth-only EFB copy source
    bp(0x52, 0x1023b)         # format 0xB (Z16R with depth source)
    bridge.gekkoaot_native_gx_present()
    bridge.gekkoaot_native_gx_shutdown()
    print('Z16R_COPY_PROBE_COMPLETED')


if __name__ == '__main__':
    main()
