"""Environment lighting: a world-stable sun and sky relight GX receivers."""
import ctypes as c
import math
import os
from pathlib import Path
import struct
import time
os.environ['GEKKOAOT_ENHANCED_RENDERER'] = '1'
from gx_interpolation_probe import main
from gx_enhanced_probe import Settings as LegacySettings
class Settings(c.Structure):
    _fields_ = LegacySettings._fields_ + [("fov_y", c.c_float), ("fragment_lighting", c.c_uint32),
        ("environment", c.c_uint32), ("sun_preset", c.c_uint32), ("environment_strength", c.c_float),
        ("sun_intensity", c.c_float), ("sun_temperature", c.c_float), ("sun_azimuth", c.c_float),
        ("sun_elevation", c.c_float), ("sky_intensity", c.c_float)]
assert c.sizeof(Settings) == 204
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
    assert settings.environment == 0 and abs(settings.environment_strength - .5) < 1e-6
    settings.post = settings.interpolation = settings.ao_quality = settings.contact_shadows = 0

    # A wall facing the camera: gray ambient plus one dim directional GX light.
    # The camera yaws +-8 degrees every frame: the world wall and the world
    # light turn together in view space, which proves the light world-stable
    # and tells the vertical world axis from the forward one.
    def render(enhanced, environment=0, light=(0., 0., 1e9), azimuth=35., elevation=55., frames=40, label=None,
               camera_light=False):
        settings.enhanced = enhanced
        settings.environment = environment
        settings.sun_azimuth, settings.sun_elevation = azimuth, elevation
        assert apply(c.byref(settings), c.sizeof(settings))
        for frame_index in range(frames):
            angle = math.radians(8. if frame_index % 2 else -8.)
            co, si = math.cos(angle), math.sin(angle)
            rotation = [[co, 0., si], [0., 1., 0.], [-si, 0., co]]
            view_light = list(light) if camera_light else [sum(rotation[r][k]*light[k] for k in range(3)) for r in range(3)]
            bp(0x40, 1 << 4); bp(0x41, 0); bp(0x51, 0xffffff)
            bp(0x49, 0); bp(0x4a, 63 | 63 << 10); bp(0x4b, 0x50000 >> 5)
            bp(0x4d, 4); bp(0x4e, 256); bp(0x52, (1 << 14) | (1 << 11))
            cp(0x50, 1 << 9 | (1 << 11)); cp(0x60, 0)
            cp(0x70, 9 | (1 << 21) | (4 << 22) | (4 << 10))
            xf(0, [rotation[0][0], 0., rotation[0][2], 0., 0., 1., 0., 0., rotation[2][0], 0., rotation[2][2], 0.])
            xf(0x400, [rotation[0][0], 0., rotation[0][2], 0., 1., 0., rotation[2][0], 0., rotation[2][2]])
            xf(0x1009, [1]); xf(0x100a, [0x505050ff]); xf(0x100c, [0xffffffff])
            xf(0x100e, [2 | 4 | (2 << 7) | (1 << 10)])
            xf(0x1010, [1 << 10])
            xf(0x603, [0x404040ff, 0., 1., 0., 1., 0., 0., *view_light, 0., 0., 1.])
            xf(0x101a, [32., -32., 16777215., 374., 374., 16777215.])
            xf(0x1020, [1., 0., 1., 0., -.01010101, -1.01010101, 0])
            bp(0, 1 << 4); bp(0x28, 0)
            bp(0x40, 1 | (3 << 1) | (1 << 4)); bp(0x41, 0x18)
            bp(0xc0, 0xfffa | (1 << 19))
            bp(0xc1, (7 << 13) | (7 << 10) | (7 << 7) | (5 << 4) | (1 << 19))
            # Two quads (8 vertices): enough geometry to define a world frame.
            vertices = [(-40., 40., -4.), (40., 40., -4.), (40., -40., -4.), (-40., -40., -4.)] * 2
            write(b'\x80\x00\x08' + b''.join(struct.pack('>6f', *(v + (0., 0., 1.))) for v in vertices))
            bp(0x49, 0); bp(0x4a, 63 | 63 << 10); bp(0x4b, 0x40000 >> 5)
            bp(0x4d, 4); bp(0x4e, 256); bp(0x52, 1 << 14)
            gx.gekkoaot_native_gx_present_xfb_ex(0x40000, 0, 64, 128, 64)
            time.sleep(.025)
        values = [pixel(x, 32) for x in (16, 32, 48)]
        if label and (directory := os.environ.get('GEKKOAOT_GPU_CAPTURE_DIR')):
            latest = Path(directory)/'latest-gpu.png'
            if latest.exists():
                latest.replace(Path(directory)/('environment-'+label+'.png'))
        return values

    def level(values):
        return values[1] & 255

    original = render(0, frames=12, label='original')
    # 80 ambient + 64 sun; the OpenGL readback is sRGB encoded (198).
    assert 135 < level(original) < 205, original
    # Off is the existing renderer, bit for bit (no AO/contact enabled here).
    assert render(1, frames=12) == original
    # Auto: the game's directional light is the sun. Facing it brightens the
    # receiver; a grazing sun leaves only the (darker) sky term.
    grazing_original = render(1, light=(1e9, 0., 0.), frames=12)
    facing = render(1, 1, label='auto-facing')
    assert level(facing) > level(original) + 12, (original, facing)
    # The sun is the game's light itself: when the game turns it, the sun
    # follows at once (same GX slot and color).
    followed = render(1, 1, light=(1e9, 0., 0.), frames=12)
    assert level(followed) < level(grazing_original) - 8, (followed, grazing_original)
    assert render(1, frames=12) == original  # Off, and a fresh session next
    grazing = render(1, 1, light=(1e9, 0., 0.), label='auto-grazing')
    assert level(grazing) < level(grazing_original) - 8, (grazing, grazing_original)
    # Neither a light fixed to the camera nor a near point light is the sun:
    # both scenes get the same world sun (Auto's fallback from the world
    # frame), i.e. the same relative shading of the wall.
    fixed = render(1, 1, camera_light=True, label='auto-camera-light')
    fixed_original = render(1, camera_light=True, frames=12)
    near = render(1, 1, light=(0., 0., 4.))
    near_original = render(1, light=(0., 0., 4.), frames=12)
    ratio_fixed, ratio_near = level(fixed) / level(fixed_original), level(near) / level(near_original)
    assert abs(ratio_fixed - ratio_near) < .04, (fixed, fixed_original, near, near_original)
    assert ratio_near < .9, (near, near_original)  # the fallback sun is behind the wall
    # Custom: the world frame (Y up) places the sun from azimuth/elevation.
    # The vertical locks after 60 world updates, then the sun fades in.
    toward = render(1, 2, light=(0., 0., 4.), azimuth=-90., elevation=1., frames=100, label='custom-toward')
    away = render(1, 2, light=(0., 0., 4.), azimuth=90., elevation=1., label='custom-away')
    assert level(toward) > level(near_original) + 12, (toward, near_original)
    assert level(away) < level(near_original) - 8, (away, near_original)
    # Switching off restores the original image.
    assert render(1, 0, frames=12) == original
    print('GX_ENVIRONMENT_PIXELS_PASSED', original, facing, grazing, fixed, near, toward, away, flush=True)


if __name__ == '__main__':
    main(verify)
