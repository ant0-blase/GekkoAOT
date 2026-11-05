"""SRTG must sample from XF-lit color without a vertex color attribute."""
import ctypes as c
import struct
import time
from gx_material_probe import Settings
from gx_interpolation_probe import main


def verify(gx,rect,frame,pixel,memory):
    def write(data):
        buf=c.create_string_buffer(data);gx.gekkoaot_native_gx_write_burst(buf,len(data),0x80004000)
    def bp(r,v):write(b'\x61'+struct.pack('>I',r<<24|v))
    def cp(r,v):write(b'\x08'+bytes([r])+struct.pack('>I',v))
    def xf(r,values):write(b'\x10'+struct.pack('>I',(len(values)-1)<<16|r)+b''.join(struct.pack('>f',v) if isinstance(v,float) else struct.pack('>I',v) for v in values))
    get=gx.gekkoaot_native_gx_get_graphics_state
    get.argtypes=[c.POINTER(Settings),c.c_uint32,c.POINTER(c.c_uint32)];get.restype=c.c_bool
    apply=gx.gekkoaot_native_gx_apply_graphics_settings
    apply.argtypes=[c.POINTER(Settings),c.c_uint32];apply.restype=c.c_bool
    settings=Settings();extent=(c.c_uint32*4)();assert get(c.byref(settings),c.sizeof(settings),extent)
    settings.enhanced=1;settings.post=0;settings.texture_scale=1;settings.material_enhancement=0
    settings.ao_quality=settings.contact_shadows=settings.gi=settings.ssr=settings.dof_quality=settings.particle_lights=0
    texture=bytearray()
    for by in range(0,8,4):
        for bx in range(0,8,4):
            color=(0xf800,0x07e0,0x001f,0xffe0)[(by//4)*2+bx//4]
            texture+=struct.pack('>H',color)*16
    c.memmove(c.addressof(memory)+0x60000,bytes(texture),len(texture))
    for fragment in (0,1):
        settings.fragment_lighting=fragment;assert apply(c.byref(settings),c.sizeof(settings))
        for channel in (0,1):
            for _ in range(8):
                cp(0x50,1<<9);cp(0x60,0);cp(0x70,9)
                xf(0,[1.,0.,0.,0.,0.,1.,0.,0.,0.,0.,1.,0.])
                xf(0x1020,[1.,0.,1.,0.,1.,0.,1])
                xf(0x1009,[2]);xf(0x100a+channel,[0x40c0ffff]);xf(0x100c+channel,[0xffffffff])
                xf(0x100e+channel,[2|(1<<10)]);xf(0x1010+channel,[1<<10])
                xf(0x103f,[1]);xf(0x1040,[(2+channel)<<4]);xf(0x1050,[0x100]) # stale post-matrix/normalize must be ignored
                bp(0,1|(2<<4));bp(0x28,1<<6);bp(0x30,7);bp(0x31,7)
                bp(0x80,0);bp(0x84,0);bp(0x88,7|(7<<10)|(4<<20));bp(0x94,0x60000>>5)
                bp(0xc0,0x08fff8);bp(0xc1,0x08ffc0);bp(0xf3,7|(7<<16)|(7<<19));bp(0x40,0);bp(0x41,0x18)
                verts=[(-1.,1.,-.5),(1.,1.,-.5),(1.,-1.,-.5),(-1.,-1.,-.5)]
                write(b'\x80'+struct.pack('>H',4)+b''.join(struct.pack('>3f',*v) for v in verts))
                sample=c.c_uint32()
                gx.gekkoaot_native_gx_peek_argb(32,32,c.byref(sample))
                frame();time.sleep(.03);gx.gekkoaot_native_gx_present_interpolated(1.)
            sample=c.c_uint32();gx.gekkoaot_native_gx_peek_argb(32,32,c.byref(sample))
            print('COLOR_TEXGEN_SAMPLE',fragment,channel,hex(pixel(32,32)),hex(sample.value),flush=True)
            value=sample.value&0xffffff
            assert value&255>220 and (value>>8)&255<30 and (value>>16)&255<30,(fragment,channel,hex(value))
    print('GX_COLOR_TEXGEN_PASSED channels=2 vertex_and_fragment_lighting=1',flush=True)

if __name__=='__main__':main(verify)
