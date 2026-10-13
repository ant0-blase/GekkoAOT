"""Exercise real NativeGX framing, state, display lists, PE order and failures."""
import argparse
import ctypes as c
import ctypes.util
import json
import os
from pathlib import Path
import struct
import subprocess


def case(bridge, fragment, kind):
    if sdl := ctypes.util.find_library('SDL3'):
        c.CDLL(sdl, mode=c.RTLD_GLOBAL)
    gx = c.CDLL(str(bridge))
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
    events = []
    callback = c.CFUNCTYPE(None, c.c_void_p, c.c_uint32, c.c_uint32)
    @callback
    def pe(user, reg, value):
        events.append([reg, value])
    gx.gekkoaot_native_gx_init.argtypes = [resolver, c.c_void_p]
    gx.gekkoaot_native_gx_init.restype = c.c_bool
    gx.gekkoaot_native_gx_set_pe_callback.argtypes = [callback, c.c_void_p]
    gx.gekkoaot_native_gx_write_burst.argtypes = [c.c_void_p, c.c_uint32, c.c_uint32]
    gx.gekkoaot_native_gx_should_quit.restype = c.c_bool
    assert gx.gekkoaot_native_gx_init(resolve, None)
    gx.gekkoaot_native_gx_set_pe_callback(pe, None)
    def bp(reg, value):
        return b'\x61' + struct.pack('>I', reg << 24 | value)
    try:
        display_list = bp(0x47, 0x1111) + bp(0x48, 0x2222)
        c.memmove(c.addressof(memory) + 0x20000, display_list, len(display_list))
        # Large XF payload crosses many submitted spans; BP mask is consumed
        # once, and the display list changes the same state as the raw stream.
        data = (bp(0x47, 0x1234) + bp(0xfe, 0xff) + bp(0x47, 0x56) +
                b'\x10' + struct.pack('>I', 255 << 16) + bytes(256 * 4) +
                b'\x40' + struct.pack('>II', 0x80020000, len(display_list)) +
                bp(0x48, 0x3333))
        expected = [[0x47, 0x1234], [0x47, 0x1256], [0x47, 0x1111],
                    [0x48, 0x2222], [0x48, 0x3333]]
        if kind == 'invalid':
            data += b'\xf8' + bp(0x47, 0x9999) # following command must not run
        elif kind == 'recovery':
            # Existing narrow reserved-byte recovery, unchanged by streaming.
            data += b'\x18' + bp(0x48, 0x8888) + bp(0x47, 0x9999)
            expected += [[0x48, 0x8888], [0x47, 0x9999]]
        for offset in range(0, len(data), fragment):
            chunk = data[offset:offset + fragment]
            buf = c.create_string_buffer(chunk)
            gx.gekkoaot_native_gx_write_burst(buf, len(chunk), 0x80004000)
            c.memset(buf, 0xa5, len(chunk)) # producer immediately reuses memory
        assert events == expected, (kind, fragment, events)
        assert bool(gx.gekkoaot_native_gx_should_quit()) == (kind == 'invalid')
        print(json.dumps({'case': kind, 'fragment': fragment, 'events': events}), flush=True)
    finally:
        gx.gekkoaot_native_gx_shutdown()


def main():
    root = Path(__file__).resolve().parents[2]
    parser = argparse.ArgumentParser()
    parser.add_argument('--bridge', type=Path,
                        default=root / '.gekkoaot/build/NativeGX-Aurora/libgekkoaot-native-gx.so')
    parser.add_argument('--case', choices=['valid', 'invalid', 'recovery'])
    parser.add_argument('--fragment-size', type=int, default=4096)
    args = parser.parse_args()
    if args.case:
        case(args.bridge, args.fragment_size, args.case)
        return
    for direct in [0, 1]:
        for mode in [0, 1]:
            env = dict(os.environ, GEKKOAOT_NATIVE_GX_STREAM_FIFO=str(mode),
                       GEKKOAOT_NATIVE_GX_DIRECT_STATE=str(direct),
                       GEKKOAOT_GRAPHICS_BACKEND='vulkan')
            for kind in ['valid', 'invalid', 'recovery']:
                for fragment in ([4096] if kind == 'recovery' else [1, 3, 7, 32, 4096]):
                    subprocess.run(['python3', str(Path(__file__)), '--bridge', str(args.bridge),
                                    '--case', kind, '--fragment-size', str(fragment)], env=env, check=True)
    print('GX native/decoded state and stream/reference framing, display-list, PE ordering and failure cases passed', flush=True)


if __name__ == '__main__':
    main()
