"""Sun cascades: replayed GX geometry casts a world-stable sun shadow."""
import ctypes as c
import math
import os
from pathlib import Path
import struct
import time
os.environ['GEKKOAOT_ENHANCED_RENDERER'] = '1'
from gx_interpolation_probe import main
from gx_environment_probe import Settings as EnvironmentSettings
class Settings(c.Structure):
    _fields_ = EnvironmentSettings._fields_ + [("sun_shadows", c.c_uint32)]
assert c.sizeof(Settings) == 208
os.environ['GEKKOAOT_FRAME_INTERPOLATION'] = '1'  # shared bootstrap oracle


def mul(a, b):
    return [[sum(a[i][k]*b[k][j] for k in range(3)) for j in range(3)] for i in range(3)]


def verify(gx, rect, frame, pixel, memory):
    def write(data):
        # The host burst ABI accepts at most 4096 bytes, including synthetic
        # stress streams. Chunking also exercises commands crossing bursts.
        for at in range(0,len(data),4096):
            chunk=data[at:at+4096];buf=c.create_string_buffer(chunk)
            gx.gekkoaot_native_gx_write_burst(buf, len(chunk), 0x80004000)

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
    assert settings.sun_shadows == 0
    settings.post = settings.interpolation = settings.ao_quality = settings.contact_shadows = 0

    # Camera at (0, 6, 8) looking at the origin; the world yaws +-20 degrees
    # every frame (world-stable light proof). Ground y=0, blocker y=2 above it.
    pitch = math.atan2(6., 8.)
    look = [[1., 0., 0.], [0., math.cos(pitch), -math.sin(pitch)], [0., math.sin(pitch), math.cos(pitch)]]
    distance = 10.

    def view_of(frame_index):
        a = math.radians(20. if frame_index % 2 else -20.)
        return mul(look, [[math.cos(a), 0., math.sin(a)], [0., 1., 0.], [-math.sin(a), 0., math.cos(a)]])

    def render(shadows, environment=1, frames=40, label=None, motion=0., stress=False, grazing=False, split=False, sky=False):
        settings.enhanced = 1
        settings.environment = environment
        settings.sun_shadows = shadows
        assert apply(c.byref(settings), c.sizeof(settings))
        for frame_index in range(frames):
            r = view_of(frame_index)
            light=(.98,.07,.15) if grazing else (0.,1.,0.)
            sun = [sum(r[k][i]*light[i] for i in range(3))*1e9 for k in range(3)]
            bp(0x40, 1 << 4); bp(0x41, 0); bp(0x51, 0xffffff)
            bp(0x49, 0); bp(0x4a, 63 | 63 << 10); bp(0x4b, 0x50000 >> 5)
            bp(0x4d, 4); bp(0x4e, 256); bp(0x52, (1 << 14) | (1 << 11))
            cp(0x50, 1 << 9 | (1 << 11)); cp(0x60, 0)
            cp(0x70, 9 | (1 << 21) | (4 << 22) | (4 << 10))
            # World-fixed geometry, camera translation independent of yaw.
            tx=motion*math.sin(frame_index*.17)
            rows=[r[0][0], r[0][1], r[0][2], tx, r[1][0], r[1][1], r[1][2], 0., r[2][0], r[2][1], r[2][2], -distance]
            normal=[r[0][0], r[0][1], r[0][2], r[1][0], r[1][1], r[1][2], r[2][0], r[2][1], r[2][2]]
            xf(0, rows);xf(0x400,normal)
            cp(0x30,0)
            if stress:xf(12,rows);xf(0x409,normal)
            xf(0x1009, [1]); xf(0x100a, [0x505050ff]); xf(0x100c, [0xffffffff])
            xf(0x100e, [2 | 4 | (2 << 7) | (1 << 10)])
            xf(0x1010, [1 << 10])
            xf(0x603, [0x404040ff, 0., 1., 0., 1., 0., 0., *sun, 0., 0., 1.])
            xf(0x101a, [32., -32., 16777215., 374., 374., 16777215.])
            xf(0x1020, [1., 0., 1., 0., -.01010101, -1.01010101, 0])
            bp(0, 1 << 4); bp(0x28, 0)
            bp(0x40, 1 | (3 << 1) | (1 << 4)); bp(0x41, 0x18)
            bp(0xc0, 0xfffa | (1 << 19))
            bp(0xc1, (7 << 13) | (7 << 10) | (7 << 7) | (5 << 4) | (1 << 19))
            ground = [(-20., 0., -20.), (-20., 0., 20.), (20., 0., 20.), (20., 0., -20.)]
            blocker = [(-1., 2., -1.), (-1., 2., 1.), (1., 2., 1.), (1., 2., -1.)]
            def geometry(vertices):
                return b'\x80'+struct.pack('>H',len(vertices))+b''.join(struct.pack('>6f', *(v+(0.,1.,0.))) for v in vertices)
            if sky:
                # An inward cube centred on the eye, with depth writes and
                # no face culling. It must neither cast a ceiling shadow nor
                # define environment/world-light evidence.
                skyRows=rows.copy();skyRows[3]=skyRows[7]=skyRows[11]=0.
                xf(0,skyRows)
                k=70.
                faces=[(-k,-k,-k),(-k,k,-k),(k,k,-k),(k,-k,-k),
                       (-k,-k,k),(k,-k,k),(k,k,k),(-k,k,k),
                       (-k,k,-k),(-k,k,k),(k,k,k),(k,k,-k),
                       (-k,-k,-k),(k,-k,-k),(k,-k,k),(-k,-k,k),
                       (-k,-k,-k),(-k,-k,k),(-k,k,k),(-k,k,-k),
                       (k,-k,-k),(k,k,-k),(k,k,k),(k,-k,k)]
                write(geometry(faces));xf(0,rows)
            if stress:
                # Distinct draw immediates, same original uniform. The last
                # blocker must still cast after more than 8192 retained draws.
                filler=geometry([(1e6,0.,1e6),(1e6+1.,0.,1e6),(1e6+1.,0.,1e6+1.),(1e6,0.,1e6+1.)])
                stream=geometry(ground)
                stream+=b''.join(b'\x08\x30'+struct.pack('>I',3 if i&1 else 0)+filler for i in range(8200))
                stream+=b'\x08\x30'+struct.pack('>I',0)+geometry(blocker)
                write(stream)
            elif split:
                write(geometry(ground))
                # Copy boundary between world and dynamic objects, as in Cars.
                # Retain EFB color/depth; both caster sets must share the atlas.
                # A partial copy avoids declaring this synthetic 64x64 scene
                # an offscreen square shadow viewport (the harness VI is 640x480).
                bp(0x49, 0); bp(0x4a, 31 | 31 << 10); bp(0x4b, 0x60000 >> 5)
                bp(0x4d, 4); bp(0x52, 12 << 3) # BP's rotated RGBA8 format
                write(geometry(blocker))
            else:write(geometry(ground if grazing else ground+blocker))
            bp(0x49, 0); bp(0x4a, 63 | 63 << 10); bp(0x4b, 0x40000 >> 5)
            bp(0x4d, 4); bp(0x4e, 256); bp(0x52, 1 << 14)
            gx.gekkoaot_native_gx_present_xfb_ex(0x40000, 0, 64, 128, 64)
            time.sleep(.025)
        # The camera looks at the origin (screen center), right under the
        # blocker; further down the screen lies open ground nearer the camera.
        values = [pixel(math.floor(32+32*tx/distance), 32), pixel(math.floor(32+32*tx/6.3), 46)]
        if grazing:values=[pixel(x,y) for y in (40,48,56) for x in (24,32,40,48)]
        if label and (directory := os.environ.get('GEKKOAOT_GPU_CAPTURE_DIR')):
            latest = Path(directory)/'latest-gpu.png'
            if latest.exists():
                latest.replace(Path(directory)/('sun-shadow-'+label+'.png'))
        return values

    def level(v):
        return v & 255

    unshadowed = render(0, label='off')
    shadowed = render(2, label='cascades')
    # The ground under the blocker loses the sun; open ground keeps it.
    assert level(shadowed[0]) < level(unshadowed[0]) - 25, (unshadowed, shadowed)
    assert abs(level(shadowed[1]) - level(unshadowed[1])) <= 3, (unshadowed, shadowed)
    continued = render(2, split=True, label='copy-continuation')
    assert level(continued[0]) < level(unshadowed[0]) - 25, (unshadowed, continued)
    assert abs(level(continued[1]) - level(shadowed[1])) <= 3, (shadowed, continued)
    with_sky = render(2,sky=True,label='sky-shell')
    assert all(abs(level(a)-level(b))<=3 for a,b in zip(with_sky,shadowed)),('sky shell corrupted real world shadows',shadowed,with_sky)
    medium = render(1)
    assert level(medium[0]) < level(unshadowed[0]) - 25, medium
    # Original lighting + cascades: the game's colors stay; only the ground
    # under the blocker darkens (shadow-only composition).
    original = render(0, environment=0, frames=12)
    shadow_only = render(2, environment=0, label='shadow-only')
    assert level(shadow_only[0]) < level(original[0]) - 20, (original, shadow_only)
    assert shadow_only[1] == original[1], (original, shadow_only)
    # Translating and turning together must keep the world-fixed blocker's
    # shadow on its receiver, rather than following the camera across ground.
    moving_off=render(0, motion=1.3)
    moving_on=render(2, motion=1.3)
    assert level(moving_on[0]) < level(moving_off[0])-20, (moving_off,moving_on)
    assert abs(level(moving_on[1])-level(moving_off[1]))<=3, (moving_off,moving_on)
    many_casters=render(2, frames=4, stress=True)
    assert level(many_casters[0])<level(unshadowed[0])-20, (unshadowed,many_casters)
    assert abs(level(many_casters[1])-level(unshadowed[1]))<=3, (unshadowed,many_casters)
    # A grazing sun on an unoccluded plane must not turn PCF footprints into
    # moving self-shadow walls. Compare a ground grid, not one favorable pixel.
    grazing_off=render(0,environment=0,grazing=True)
    grazing_on=render(2,environment=0,grazing=True)
    assert all(abs(level(a)-level(b))<=3 for a,b in zip(grazing_off,grazing_on)), (grazing_off,grazing_on)
    print('GX_SUN_SHADOW_PIXELS_PASSED', unshadowed, shadowed, medium, original, shadow_only, flush=True)


if __name__ == '__main__':
    main(verify)
