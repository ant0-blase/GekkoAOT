"""PN refinement changes actual GX geometry and its silhouette, without texture enhancement."""
import math
import ctypes as c
import os
from pathlib import Path
import struct
import time
os.environ['GEKKOAOT_ENHANCED_RENDERER'] = '1'
from gx_interpolation_probe import main
from gx_material_probe import Settings as LegacySettings
class Settings(c.Structure):
    _fields_ = LegacySettings._fields_ + [("geometry_refinement",c.c_uint32)]
assert c.sizeof(Settings) == 268
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

    vertices=[(1.,0.,0.),(-1.,0.,0.),(0.,1.,0.),(0.,-1.,0.),(0.,0.,1.),(0.,0.,-1.)]
    faces=[(0,2,4),(4,2,1),(1,2,5),(5,2,0),(4,3,0),(1,3,4),(5,3,1),(0,3,5)]
    settings.texture_scale=1;settings.material_enhancement=0
    settings.fragment_lighting=0;settings.environment=0;settings.gi=settings.ssr=settings.sun_shadows=0
    def render(enhanced, light_x=0., light_color=0xffffffff, normal=True, perspective=True, spot=False, textured=True, label=None, fragment=True,indexed=False,rigid=False,mixed=False,normal_scale=1.,motion=False):
        settings.enhanced = enhanced
        settings.fragment_lighting = 0
        assert apply(c.byref(settings), c.sizeof(settings))
        for frame_index in range(28):
            # Clear guest depth independently of color, then cover the target.
            bp(0x40, 1 << 4); bp(0x41, 0x18);bp(0x4f,0);bp(0x50,0); bp(0x51, 0xffffff)
            bp(0x49, 0); bp(0x4a, 63 | 63 << 10); bp(0x4b, 0x50000 >> 5)
            bp(0x4d, 4); bp(0x4e, 256); bp(0x52, (1 << 14) | (1 << 11))
            cp(0x50, ((3 if indexed else 1)<<9) | (((3 if indexed else 1)<<11) if normal else 0) | int(rigid)); cp(0x60, 1)
            cp(0x70, 9 | (1 << 21) | (4 << 22) | ((4 << 10) if normal else 0))
            xf(0, [1., 0., 0., 0., 0., 1., 0., 0., 0., 0., 1., -3.])
            xf(0x400, [1., 0., 0., 0., 1., 0., 0., 0., 1.])
            xf(0x1009, [1]); xf(0x100a, [0]); xf(0x100c, [0xffffffff])
            xf(0x100e, [0])
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
            xf(12,[1.,0.,0.,0.,0.,1.,0.,0.,0.,0.,1.,-3.])
            xf(0x409,[1.,0.,0.,0.,1.,0.,0.,0.,1.])
            if indexed:
                data=b''.join(struct.pack('>3f',*v) for v in vertices)
                normal_data=b''.join(struct.pack('>3f',*(c*normal_scale for c in v)) for v in vertices)
                c.memmove(c.addressof(memory)+0x70000,data,len(data));c.memmove(c.addressof(memory)+0x71000,normal_data,len(normal_data))
                cp(0xa0,0x70000);cp(0xa1,0x71000);cp(0xb0,12);cp(0xb1,12)
            shift=.18*math.sin(frame_index*.4) if motion else 0.
            current=[(v[0]+shift,v[1],v[2]) for v in vertices]
            if indexed and motion:
                posed=b''.join(struct.pack('>3f',*v) for v in current)
                c.memmove(c.addressof(memory)+0x70000,posed,len(posed))
            payload=bytearray()
            for f in faces:
                for i in f:
                    if rigid:payload+=bytes([0 if mixed and i%2 else 3])
                    payload+=struct.pack('>HH2f' if normal else '>H2f',*((i,i,0.,0.) if normal else (i,0.,0.))) if indexed else struct.pack('>8f' if normal else '>5f',*((current[i]+tuple(c*normal_scale for c in vertices[i]) if normal else current[i])+(0.,0.)))
            write(b'\x90'+struct.pack('>H',24)+payload)
            # XFB presentation already submits this display-copy boundary.
            # A second generic present would present the next, empty EFB.
            bp(0x49, 0); bp(0x4a, 63 | 63 << 10); bp(0x4b, 0x40000 >> 5)
            bp(0x4d, 4); bp(0x4e, 256); bp(0x52, 1 << 14)
            gx.gekkoaot_native_gx_present_xfb_ex(0x40000, 0, 64, 128, 64)
            time.sleep(.025)
        values = [pixel(x,y) for x,y in [(25,25),(38,25),(25,38),(38,38),(32,32),(20,32),(44,32)]]
        if label and (directory := os.environ.get('GEKKOAOT_GPU_CAPTURE_DIR')):
            latest = Path(directory)/'latest-gpu.png'
            if latest.exists():
                latest.replace(Path(directory)/('fragment-'+label+'.png'))
        return values

    settings.geometry_refinement=0
    original=render(1,label='mesh-original')
    settings.geometry_refinement=2
    refined=render(1,label='mesh-refined')
    assert all((v&0xffffff)==0 for v in original[:4]),original
    assert any((v&0xffffff)>0x808080 for v in refined[:4]),(original,refined)
    assert (original[4]&0xffffff)>0x808080 and (refined[4]&0xffffff)>0x808080
    settings.material_compare=1
    comparison=render(1,label='mesh-comparison')
    for i,x in enumerate([25,38,25,38,32,20,44]):
        assert comparison[i]==(original[i] if x<32 else refined[i]),('same-frame comparison',i,original,refined,comparison)
    settings.material_compare=0
    assert render(1,indexed=True)==refined,'indexed GX arrays produce the same refined silhouette'
    assert render(1,indexed=True,normal_scale=2.)==refined,'non-unit GX lighting normals retain their direction for geometry refinement'
    assert render(1,indexed=True,rigid=True)==refined,'rigid per-vertex XF indices preserve object transforms'
    assert render(1,indexed=True,rigid=True,mixed=True)==original,'unproved multi-matrix skinning keeps the original geometry'
    moved=render(1,motion=True,label='mesh-animated')
    assert moved!=refined,'current animated position updates the refined geometry'
    assert render(1)==refined,'returning to the source pose restores the refined silhouette'
    assert render(1,normal=False)==original,'missing normals with sharp dihedrals preserve the hard shape'
    settings.geometry_refinement=0
    assert render(1)==original,'original geometry restores exactly'
    print('GX_MESH_PIXELS_PASSED',original,refined,flush=True)

if __name__=='__main__':main(verify)
