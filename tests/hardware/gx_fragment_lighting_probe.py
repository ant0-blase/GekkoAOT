"""GX point lighting moves per fragment, without adding a second light term."""
import ctypes as c
import os
from pathlib import Path
import struct
import time
os.environ['GEKKOAOT_ENHANCED_RENDERER'] = '1'
from gx_interpolation_probe import main
from gx_enhanced_probe import Settings as LegacySettings
class Settings(c.Structure):
    _fields_ = LegacySettings._fields_ + [("fov_y", c.c_float), ("fragment_lighting", c.c_uint32)]
assert c.sizeof(Settings) == 172
os.environ['GEKKOAOT_FRAME_INTERPOLATION'] = '1'  # shared bootstrap oracle


def verify(gx, rect, frame, pixel, memory):
    def write(data):
        buf = c.create_string_buffer(data)
        gx.gekkoaot_native_gx_write_burst(buf, len(data), 0x80004000)

    def bp(reg, value):
        write(b'\x61' + struct.pack('>I', reg << 24 | value))

    def cp(reg, value):
        write(b'\x08' + bytes([reg]) + struct.pack('>I', value))

    def xf(reg, values):
        write(b'\x10' + struct.pack('>I', (len(values)-1) << 16 | reg) +
              b''.join(struct.pack('>f' if isinstance(v, float) else '>I', v) for v in values))

    get = gx.gekkoaot_native_gx_get_graphics_state
    get.argtypes = [c.POINTER(Settings), c.c_uint32, c.POINTER(c.c_uint32)]
    get.restype = c.c_bool
    apply = gx.gekkoaot_native_gx_apply_graphics_settings
    apply.argtypes = [c.POINTER(Settings), c.c_uint32]
    apply.restype = c.c_bool
    settings = Settings()
    extent = (c.c_uint32*4)()
    assert get(c.byref(settings), c.sizeof(settings), extent)
    settings.post = settings.interpolation = settings.ao_quality = settings.contact_shadows = 0

    def render(enhanced, light_x=0., light_color=0xffffffff, normal=True, perspective=True, spot=False, textured=True, label=None, fragment=True):
        settings.enhanced = enhanced
        settings.fragment_lighting = int(fragment)
        assert apply(c.byref(settings), c.sizeof(settings))
        for _ in range(12):
            # Clear guest depth independently of color, then cover the target.
            bp(0x40, 1 << 4); bp(0x41, 0); bp(0x51, 0xffffff)
            bp(0x49, 0); bp(0x4a, 63 | 63 << 10); bp(0x4b, 0x50000 >> 5)
            bp(0x4d, 4); bp(0x4e, 256); bp(0x52, (1 << 14) | (1 << 11))
            cp(0x50, 1 << 9 | ((1 << 11) if normal else 0)); cp(0x60, 1)
            cp(0x70, 9 | (1 << 21) | (4 << 22) | ((4 << 10) if normal else 0))
            xf(0, [1., 0., 0., 0., 0., 1., 0., 0., 0., 0., 1., 0.])
            xf(0x400, [1., 0., 0., 0., 1., 0., 0., 0., 1.])
            xf(0x1009, [1]); xf(0x100a, [0]); xf(0x100c, [0xffffffff])
            xf(0x100e, [2 | 4 | (2 << 7) | (1 << 10) | ((1 << 9) if spot else 0)])
            xf(0x1010, [1 << 10])
            xf(0x603, [light_color, 0., 0. if spot else 1., 1. if spot else 0.,
                        1., 0., 0., light_x, 0., -3., 0., 0., 1.])
            xf(0x101a, [32., -32., 16777215., 374., 374., 16777215.])
            xf(0x1020, [1., 0., 1., 0., -.01010101, -1.01010101, 0] if perspective else
                        [.25, 0., .25, 0., -.01, 0., 1])
            bp(0, int(textured) | (1 << 4)); bp(0x28, (1 << 6) if textured else 0)
            bp(0x64, (0x20000 + 32) >> 5); bp(0x65, 0x200 | (1 << 10))
            bp(0x40, 1 | (3 << 1) | (1 << 4)); bp(0x41, 0x18)
            bp(0xc0, (0xf8af if textured else 0xfffa) | (1 << 19))
            bp(0xc1, (7 << 13) | (7 << 10) | (7 << 7) | (5 << 4) | (1 << 19))
            vertices = [(-4., 4., -4.), (4., 4., -4.), (4., -4., -4.), (-4., -4., -4.)]
            write(b'\x80\x00\x04' + b''.join(struct.pack('>8f' if normal else '>5f',
                *((v + (0., 0., 1.) if normal else v) + (0., 0.))) for v in vertices))
            # XFB presentation already submits this display-copy boundary.
            # A second generic present would present the next, empty EFB.
            bp(0x49, 0); bp(0x4a, 63 | 63 << 10); bp(0x4b, 0x40000 >> 5)
            bp(0x4d, 4); bp(0x4e, 256); bp(0x52, 1 << 14)
            gx.gekkoaot_native_gx_present_xfb_ex(0x40000, 0, 64, 128, 64)
            time.sleep(.025)
        values = [pixel(x, 32) for x in (16, 32, 48)]
        if label and (directory := os.environ.get('GEKKOAOT_GPU_CAPTURE_DIR')):
            latest = Path(directory)/'latest-gpu.png'
            if latest.exists():
                latest.replace(Path(directory)/('fragment-'+label+'.png'))
        return values

    original = render(0, label='original')
    enhanced = render(1, label='enhanced')
    assert enhanced[1] & 255 > (original[1] & 255) + 100, (original, enhanced)
    # Moving a real GX light updates the uniform, without a pipeline change.
    moved = render(1, light_x=2., label='moved')
    assert moved[2] & 255 > (moved[0] & 255) + 60, moved
    red = render(1, light_color=0xff0000ff)
    assert red[1] >> 16 > 220 and red[1] & 0xffff == 0, red
    spotlight = render(1, spot=True)
    assert spotlight[1] & 255 > (spotlight[0] & 255) + 80, spotlight
    # Original remains selectable; unproved normals and ortho stay vertex-lit.
    assert render(1, fragment=False) == original
    assert render(0) == original
    assert render(1, textured=False) == enhanced
    assert render(1, normal=False) == render(0, normal=False)
    assert render(1, perspective=False) == render(0, perspective=False)
    print('GX_FRAGMENT_LIGHTING_PIXELS_PASSED', original, enhanced, moved, red, spotlight, flush=True)


if __name__ == '__main__':
    main(verify)
