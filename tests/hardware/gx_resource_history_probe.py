"""EFB producer revisions, actual texture reads and merged-draw provenance."""
import ctypes as c
import os
import struct
import time
os.environ['GEKKOAOT_ENHANCED_DIAGNOSTICS']='all'
from gx_enhanced_probe import Settings
from gx_interpolation_probe import main


def verify(gx, rect, frame, pixel, memory):
    def write(data):
        buffer=c.create_string_buffer(data)
        gx.gekkoaot_native_gx_write_burst(buffer,len(data),0x80004000)
    def bp(reg,value):write(b'\x61'+struct.pack('>I',reg<<24|value))
    def quad(x0,x1):
        vertices=((x0/32-1,1.,-.5,0.,0.),(x1/32-1,1.,-.5,1.,0.),
                  (x1/32-1,-1.,-.5,1.,1.),(x0/32-1,-1.,-.5,0.,1.))
        write(b'\x80\x00\x04'+b''.join(struct.pack('>5f',*v) for v in vertices))
    settings=Settings();extent=(c.c_uint32*4)()
    gx.gekkoaot_native_gx_get_graphics_state.argtypes=[c.POINTER(Settings),c.c_uint32,c.POINTER(c.c_uint32)]
    gx.gekkoaot_native_gx_apply_graphics_settings.argtypes=[c.POINTER(Settings),c.c_uint32]
    assert gx.gekkoaot_native_gx_get_graphics_state(c.byref(settings),c.sizeof(settings),extent)
    settings.enhanced=1;settings.ao_quality=settings.contact_shadows=settings.post=settings.interpolation=0
    assert gx.gekkoaot_native_gx_apply_graphics_settings(c.byref(settings),c.sizeof(settings))
    def scene():
        bp(0x88,7|7<<10|8<<20);bp(0x94,0x10000>>5)
        bp(0xc0,0xfff8|1<<19);bp(0xc1,7<<13|7<<10|7<<7|4<<4|1<<19)
        rect(0,0,64,64,2)
        bp(0x49,0);bp(0x4a,63|63<<10);bp(0x4b,0x60000>>5);bp(0x4d,4)
        bp(0x52,8<<3) # RGB565 GXCopyTex, GPU resident
        bp(0x88,63|63<<10|4<<20);bp(0x94,0x60000>>5)
        quad(0,16);quad(16,32) # Adjacent matching draws may merge in Aurora.
        # The copy remains bound, but neither color nor alpha samples it.
        bp(0xc0,0xfffc|1<<19);bp(0xc1,7<<13|7<<10|7<<7|6<<4|1<<19)
        quad(32,64)
        frame();time.sleep(.04)
    for _ in range(6):scene()
    deadline=time.monotonic()+5
    while time.monotonic()<deadline:
        values=[]
        for x in (8,24,48):
            value=c.c_uint32();gx.gekkoaot_native_gx_peek_argb(x,32,c.byref(value));values.append(value.value&0xffffff)
        if values==[0xff0000,0xff0000,0xffffff]:break
        scene()
    else:raise AssertionError(values)
    print('GX_RESOURCE_HISTORY_PIXELS_PASSED copies_revisions actual_reads merged_draws bound_unsampled',flush=True)


if __name__=='__main__':main(verify)
