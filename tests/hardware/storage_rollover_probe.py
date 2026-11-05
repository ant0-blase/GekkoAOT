"""Opt-in real Aurora stress probe; invokes only the public native GX ABI.

The parent test checks rollover logs. This exercises submission/lifetime, not
pixel correctness. Run in a separate process because GPU failures can abort.
"""
import ctypes as c
import ctypes.util
from pathlib import Path
import struct


def main():
    root = Path(__file__).resolve().parents[2]
    # Match native-run: system SDL is loaded before the renderer DSO.
    sdl = ctypes.util.find_library('SDL3')
    if sdl:
        c.CDLL(sdl, mode=c.RTLD_GLOBAL)
    lib = c.CDLL(str(root / '.gekkoaot/build/NativeGX-Aurora/libgekkoaot-native-gx.so'))
    memory = (c.c_uint8 * (24 * 1024 * 1024))()
    resolve_type = c.CFUNCTYPE(c.c_bool, c.c_void_p, c.c_uint32, c.c_uint32,
                              c.c_uint32, c.c_uint32, c.POINTER(c.c_void_p), c.POINTER(c.c_uint32))

    @resolve_type
    def resolve(user, address, size, space, resource, data, available):
        offset = address & 0x3fffffff
        if offset > len(memory) or size > len(memory) - offset:
            return False
        data[0] = c.addressof(memory) + offset
        available[0] = len(memory) - offset
        return True

    lib.gekkoaot_native_gx_init.argtypes = [resolve_type, c.c_void_p]
    lib.gekkoaot_native_gx_init.restype = c.c_bool
    lib.gekkoaot_native_gx_write_burst.argtypes = [c.c_void_p, c.c_uint32, c.c_uint32]
    lib.gekkoaot_native_gx_write_burst.restype = None
    lib.gekkoaot_native_gx_present.restype = None
    lib.gekkoaot_native_gx_shutdown.restype = None
    if not lib.gekkoaot_native_gx_init(resolve, None):
        raise RuntimeError('native GX initialization failed')

    def emit(data):
        buffer = c.create_string_buffer(data)
        lib.gekkoaot_native_gx_write_burst(buffer, len(data), 0x80004000)

    def cp(register, value):
        emit(bytes([8, register]) + struct.pack('>I', value))

    def bp(register, value):
        emit(b'\x61' + struct.pack('>I', (register << 24) | value))

    bp(0, 0)  # GENMODE: one TEV stage; no texture or color channels.
    bp(0xe8, (1 << 10) | 662)  # Fog range, including its cached storage LUT.
    for register in range(0xe9, 0xee):
        bp(register, 0x100100)
    for register, value in ((0x50, 3 << 9), (0x60, 0), (0x70, 9), (0x80, 0),
                            (0x90, 0), (0xa0, 0x10000), (0xb0, 12)):
        cp(register, value)
    for index in (0, 1, 65535):
        raw = struct.pack('>fff', float(index == 1), float(index == 65535), 0.0)
        c.memmove(c.addressof(memory) + 0x10000 + index * 12, raw, len(raw))
    for frame in range(2):
        for draw in range(100):
            # Vertex invalidation forces fresh data; INDEX16 extends the
            # position window to 65536 * 12 bytes for this three-vertex draw.
            emit(b'\x48\x90\x00\x03\x00\x00\x00\x01\xff\xff')
        lib.gekkoaot_native_gx_present()
    lib.gekkoaot_native_gx_shutdown()
    print('STORAGE_STRESS_COMPLETED frames=2 draws_per_frame=100 fog=1')


if __name__ == '__main__':
    main()
