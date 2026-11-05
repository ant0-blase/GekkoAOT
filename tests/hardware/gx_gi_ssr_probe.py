"""Screen-space GI and reflections: a red wall lights and mirrors in a white floor."""
import ctypes as c
import math
import os
from pathlib import Path
import struct
import time
os.environ['GEKKOAOT_ENHANCED_RENDERER'] = '1'
from gx_interpolation_probe import main
from gx_sun_shadow_probe import Settings as ShadowSettings
class Settings(c.Structure):
    _fields_ = ShadowSettings._fields_ + [("gi", c.c_uint32), ("ssr", c.c_uint32),
        ("gi_strength", c.c_float), ("ssr_strength", c.c_float)]
assert c.sizeof(Settings) == 224
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
    assert settings.gi == 0 and settings.ssr == 0
    settings.post = settings.interpolation = settings.ao_quality = settings.contact_shadows = 0
    settings.enhanced = 1; settings.environment = 0

    # Camera at (0, 3, 5) aimed at the wall base (0, 0, -3): that edge is the
    # screen center; the floor right in front of the wall lies just below it.
    pitch = math.atan2(3., 8.)
    r = [[1., 0., 0.], [0., math.cos(pitch), -math.sin(pitch)], [0., math.sin(pitch), math.cos(pitch)]]
    eye = (0., 3., 5.)
    t = [-sum(r[i][k]*eye[k] for k in range(3)) for i in range(3)]

    def quad(vertices, color, normal):
        xf(0x100c, [color])
        write(b'\x80\x00\x04' + b''.join(struct.pack('>6f', *(v + normal)) for v in vertices))

    def render(gi=0, ssr=0, gi_strength=2., ssr_strength=4., label=None, material='diffuse'):
        settings.gi, settings.ssr = gi, ssr
        settings.gi_strength, settings.ssr_strength = gi_strength, ssr_strength
        assert apply(c.byref(settings), c.sizeof(settings))
        for _ in range(12):
            bp(0x40, 1 << 4); bp(0x41, 0); bp(0x51, 0xffffff)
            bp(0x49, 0); bp(0x4a, 63 | 63 << 10); bp(0x4b, 0x50000 >> 5)
            bp(0x4d, 4); bp(0x4e, 256); bp(0x52, (1 << 14) | (1 << 11))
            cp(0x50, 1 << 9 | (1 << 11)); cp(0x60, 0)
            cp(0x70, 9 | (1 << 21) | (4 << 22) | (4 << 10))
            xf(0, [r[0][0], r[0][1], r[0][2], t[0], r[1][0], r[1][1], r[1][2], t[1], r[2][0], r[2][1], r[2][2], t[2]])
            xf(0x400, [r[0][0], r[0][1], r[0][2], r[1][0], r[1][1], r[1][2], r[2][0], r[2][1], r[2][2]])
            # Identical base colors: the specular configuration uses white
            # ambient and a black light. Only its GX material evidence differs.
            xf(0x1009, [1]); xf(0x100a, [0xffffffff]); xf(0x100e, [0]); xf(0x1010, [0])
            xf(0x603, [0, 0., 1., 0., 1., 0., 0., 0., 1e9, 0., 0., 1.])
            xf(0x101a, [32., -32., 16777215., 374., 374., 16777215.])
            xf(0x1020, [1., 0., 1., 0., -.01010101, -1.01010101, 0])
            bp(0, 1 << 4); bp(0x28, 0)
            bp(0x40, 1 | (3 << 1) | (1 << 4)); bp(0x41, 0x18)
            bp(0xc0, 0xfffa | (1 << 19))
            bp(0xc1, (7 << 13) | (7 << 10) | (7 << 7) | (5 << 4) | (1 << 19))
            if material == 'specular':xf(0x100e, [2 | 4 | (1 << 9)])
            if material == 'unused-specular':xf(0x100f, [2 | 4 | (1 << 9)])
            quad([(-20., 0., -20.), (-20., 0., 20.), (20., 0., 20.), (20., 0., -20.)], 0x909090ff, (0., 1., 0.))
            xf(0x100e, [0]); xf(0x100f, [0])
            quad([(-20., 0., -3.), (20., 0., -3.), (20., 12., -3.), (-20., 12., -3.)], 0xff1818ff, (0., 0., 1.))
            bp(0x49, 0); bp(0x4a, 63 | 63 << 10); bp(0x4b, 0x40000 >> 5)
            bp(0x4d, 4); bp(0x4e, 256); bp(0x52, 1 << 14)
            gx.gekkoaot_native_gx_present_xfb_ex(0x40000, 0, 64, 128, 64)
            time.sleep(.025)
        values = [pixel(32, 35), pixel(32, 26)]
        if label and (directory := os.environ.get('GEKKOAOT_GPU_CAPTURE_DIR')):
            latest = Path(directory)/'latest-gpu.png'
            if latest.exists():
                latest.replace(Path(directory)/('gi-ssr-'+label+'.png'))
        return values

    def rgb(v):
        values=(v >> 16 & 255, v >> 8 & 255, v & 255)
        if os.environ.get('GEKKOAOT_GRAPHICS_BACKEND')=='OGL':
            # OGL's presentation readback is sRGB; compare linear byte deltas
            # to the Vulkan readback, as the shadow-depth fixture does.
            values=tuple(round(255*((x/255/12.92) if x/255<=.04045 else
                ((x/255+.055)/1.055)**2.4)) for x in values)
        return values

    base = render(label='off')
    floor, wall = rgb(base[0]), rgb(base[1])
    assert floor[0] == floor[2] and wall[0] > wall[2] + 100, (floor, wall)
    # Bounce light: the floor in front of the red wall turns red.
    bounce = rgb(render(gi=1, label='gi')[0])
    assert bounce[0] > floor[0] + 6 and bounce[0] - floor[0] > bounce[2] - floor[2] + 4, (floor, bounce)
    # Bright diffuse surfaces and unused specular channels must stay exact.
    assert render(ssr=1, label='diffuse') == base
    assert render(ssr=1, material='unused-specular') == base
    specular_base = render(material='specular')
    assert specular_base == base, (specular_base, base)
    # Grazing reflection only on the floor marked with sampled GX specular.
    mirror = rgb(render(ssr=1, label='ssr', material='specular')[0])
    assert mirror[0] > floor[0] + 10 and mirror[2] <= floor[2] + 2, (floor, mirror)
    # Off restores the original image.
    assert render() == base
    print('GX_GI_SSR_PIXELS_PASSED', floor, wall, bounce, mirror, flush=True)


if __name__ == '__main__':
    main(verify)
