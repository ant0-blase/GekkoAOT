"""Enhanced scene composited through a texture copy (Luigi's Mansion pattern).

The scene is copied to an RGB565 texture mid-frame, then drawn back with an
orthographic full-screen quad. The displayed image must carry AO through the
copy's enhanced twin, while a perspective draw of the same copy, and every
draw after the first CPU EFB color peek, keep the original copy.
"""
import ctypes as c
import struct
import time
from gx_enhanced_probe import Settings
from gx_interpolation_probe import main


def verify(gx, rect, frame, pixel, memory):
    def write(data):
        buffer=c.create_string_buffer(data);gx.gekkoaot_native_gx_write_burst(buffer,len(data),0x80004000)
    def bp(r,v):write(b'\x61'+struct.pack('>I',r<<24|v))
    def cp(r,v):write(b'\x08'+bytes([r])+struct.pack('>I',v))
    def xf(r,values):write(b'\x10'+struct.pack('>I',(len(values)-1)<<16|r)+b''.join(struct.pack('>f',v) if isinstance(v,float) else struct.pack('>I',v) for v in values))
    get=gx.gekkoaot_native_gx_get_graphics_state;get.argtypes=[c.POINTER(Settings),c.c_uint32,c.POINTER(c.c_uint32)];get.restype=c.c_bool
    apply=gx.gekkoaot_native_gx_apply_graphics_settings;apply.argtypes=[c.POINTER(Settings),c.c_uint32];apply.restype=c.c_bool
    settings=Settings();extent=(c.c_uint32*4)();assert get(c.byref(settings),c.sizeof(settings),extent)
    settings.enhanced=1;settings.post=settings.interpolation=0;settings.contact_shadows=0
    settings.ao_strength=2;settings.ao_radius=.5
    def quad(x0,y0,x1,y1,z):
        vertices=[((x0/32-1)*-z,(1-y0/32)*-z,z,0.,0.),((x1/32-1)*-z,(1-y0/32)*-z,z,1.,0.),
                  ((x1/32-1)*-z,(1-y1/32)*-z,z,1.,1.),((x0/32-1)*-z,(1-y1/32)*-z,z,0.,1.)]
        write(b'\x80'+struct.pack('>H',4)+b''.join(struct.pack('>5f',*v) for v in vertices))
    def screen_quad():
        vertices=[(-1.,1.,-.5,0.,0.),(1.,1.,-.5,1.,0.),(1.,-1.,-.5,1.,1.),(-1.,-1.,-.5,0.,1.)]
        write(b'\x80'+struct.pack('>H',4)+b''.join(struct.pack('>5f',*v) for v in vertices))
    perspective_composite=False
    def scene():
        # Perspective world with an occluding step (AO next to its left edge).
        bp(0x41,0);bp(0x40,1<<4);bp(0x51,0xffffff)
        bp(0x49,0);bp(0x4a,63|(63<<10));bp(0x4b,0x50000>>5);bp(0x4d,4);bp(0x4e,256);bp(0x52,(1<<14)|(1<<11))
        cp(0x50,1<<9);cp(0x70,9|(1<<21)|(4<<22))
        # A main scene viewport is not square: square copied viewports are
        # treated as render-to-texture producers (shadow/reflection maps).
        xf(0x101a,[32.,-30.,16777215.,374.,372.,16777215.])
        xf(0x1020,[1.,0.,1.,0.,-.01010101,-1.01010101,0])
        bp(0x40,1|(3<<1)|(1<<4));bp(0x41,0x4b9)
        bp(0x88,7|(7<<10)|(8<<20));bp(0x94,0x10000>>5)
        bp(0x64,(0x20000+32)>>5);bp(0x65,0x200|(1<<10))
        quad(0,0,64,64,-4.);quad(24,20,40,44,-3.)
        # Mid-frame RGB565 texture copy of the scene (GPU resident).
        bp(0x49,0);bp(0x4a,63|(59<<10));bp(0x4b,0x60000>>5);bp(0x4d,4);bp(0x52,8<<3)
        # Composite the copy back to the screen without depth.
        if perspective_composite:xf(0x1020,[1.,0.,1.,0.,-.01010101,-1.01010101,0])
        else:xf(0x1020,[1.,0.,1.,0.,1.,0.,1])
        bp(0x40,0);bp(0x41,0x18)
        bp(0x88,63|(59<<10)|(4<<20));bp(0x94,0x60000>>5);bp(0x30,63);bp(0x31,59)
        if perspective_composite:quad(0,0,64,64,-2.)
        else:screen_quad()
        bp(0x88,7|(7<<10)|(8<<20));bp(0x94,0x10000>>5);bp(0x30,7);bp(0x31,7)
        bp(0x49,0);bp(0x4a,63|(63<<10));bp(0x4b,0x40000>>5);bp(0x4d,4);bp(0x4e,256);bp(0x52,1<<14)
        gx.gekkoaot_native_gx_present_xfb_ex(0x40000,0,64,128,64)
        xf(0x101a,[32.,-32.,16777215.,374.,374.,16777215.])
        time.sleep(.04)
    def observe(label):
        assert apply(c.byref(settings),c.sizeof(settings))
        for _ in range(8):scene()
        values=[pixel(x,y)&0xffffff for x,y in ((20,32),(32,32),(3,3))]
        print('ALIAS_SAMPLES',label,[hex(v) for v in values],flush=True)
        return values
    settings.ao_quality=0;original=observe('original')
    settings.ao_quality=3;composited=observe('ao-through-copy')
    assert composited[0]<original[0],('ortho composite of the scene copy must carry AO',original,composited)
    assert composited[2]==original[2],(original,composited)
    perspective_composite=True
    settings.ao_quality=0;perspective_original=observe('perspective-original')
    settings.ao_quality=3;perspective=observe('perspective-copy')
    assert perspective[0]==perspective_original[0],('a perspective draw keeps the original copy',perspective_original,perspective)
    perspective_composite=False
    value=c.c_uint32();gx.gekkoaot_native_gx_peek_argb(16,32,c.byref(value))
    after_peek=observe('after-color-peek')
    assert after_peek[0]==original[0],('CPU color peeks disable copy aliasing',original,after_peek)
    print('GX_COPY_ALIAS_PIXELS_PASSED ortho-composite-ao perspective-original peek-disables',flush=True)


if __name__=='__main__':main(verify)
