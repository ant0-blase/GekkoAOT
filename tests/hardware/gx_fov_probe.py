"""Real GX perspective FOV override, live updates, Original and HUD pixel oracle."""
import ctypes as c
import os
from pathlib import Path
import struct
import time
from gx_enhanced_probe import Settings as EnhancedSettings
from gx_interpolation_probe import main
os.environ['GEKKOAOT_ENHANCED_RENDERER']='0'
os.environ['GEKKOAOT_FRAME_INTERPOLATION']='1'
os.environ['GEKKOAOT_POST_PROCESSING']='0'
class Settings(c.Structure):
    _fields_=EnhancedSettings._fields_+[('fov_y',c.c_float)]
assert c.sizeof(Settings)==168

def verify(gx,rect,frame,pixel,memory):
    def write(data):
        b=c.create_string_buffer(data);gx.gekkoaot_native_gx_write_burst(b,len(data),0x80004000)
    def bp(r,v):write(b'\x61'+struct.pack('>I',r<<24|v))
    def xf(r,values):write(b'\x10'+struct.pack('>I',(len(values)-1)<<16|r)+b''.join(struct.pack('>f',v) if isinstance(v,float) else struct.pack('>I',v) for v in values))
    get=gx.gekkoaot_native_gx_get_graphics_state;get.argtypes=[c.POINTER(Settings),c.c_uint32,c.POINTER(c.c_uint32)];get.restype=c.c_bool
    apply=gx.gekkoaot_native_gx_apply_graphics_settings;apply.argtypes=[c.POINTER(Settings),c.c_uint32];apply.restype=c.c_bool
    settings=Settings();extent=(c.c_uint32*4)();assert get(c.byref(settings),c.sizeof(settings),extent)
    settings.post=settings.enhanced=settings.interpolation=0
    # A caller with the pre-FOV 164-byte ABI retains the current override.
    settings.fov_y=120;assert apply(c.byref(settings),c.sizeof(settings))
    prefix=EnhancedSettings();c.memmove(c.byref(prefix),c.byref(settings),c.sizeof(prefix))
    assert apply(c.cast(c.byref(prefix),c.POINTER(Settings)),c.sizeof(prefix))
    assert get(c.byref(settings),c.sizeof(settings),extent) and settings.fov_y==120
    def quad(x0,y0,x1,y1,z,color):
        bp(0x64,(0x20000+color*32)>>5);bp(0x65,0x200|(1<<10))
        vertices=[((x0/32-1)*-z,(1-y0/32)*-z,z,0.,0.),((x1/32-1)*-z,(1-y0/32)*-z,z,1.,0.),((x1/32-1)*-z,(1-y1/32)*-z,z,1.,1.),((x0/32-1)*-z,(1-y1/32)*-z,z,0.,1.)]
        write(b'\x80'+struct.pack('>H',4)+b''.join(struct.pack('>5f',*v) for v in vertices))
    def scene():
        # Always overwrite the test canvas, including areas newly exposed by
        # the wider FOV; no previous-frame depth may survive the test setup.
        xf(0x1020,[1.,0.,1.,0.,1.,0.,1]);bp(0x40,0);rect(0,0,64,64,0)
        xf(0x1020,[1.,0.,1.,0.,-.01010101,-1.01010101,0]);bp(0x40,31)
        quad(24,20,40,44,-3.,2)
        xf(0x1020,[1.,0.,1.,0.,1.,0.,1]);rect(0,0,8,8,3)
        bp(0x49,0);bp(0x4a,63|(63<<10));bp(0x4b,0x40000>>5);bp(0x4d,4);bp(0x4e,256);bp(0x52,1<<14)
        gx.gekkoaot_native_gx_present_xfb_ex(0x40000,0,64,128,64)
    def observe(fov,name):
        settings.fov_y=fov;assert apply(c.byref(settings),c.sizeof(settings))
        for _ in range(8):scene();time.sleep(.03)
        result=[pixel(x,y)&0xffffff for x,y in ((25,32),(20,32),(32,32),(3,3))]
        if directory:=os.environ.get('GEKKOAOT_GPU_CAPTURE_DIR'):
            latest=Path(directory)/'latest-gpu.png'
            if latest.exists():latest.replace(Path(directory)/(name+'.png'))
        print('FOV_SAMPLES',name,[hex(v) for v in result],flush=True)
        return result
    original=observe(0,'fov-original');wide=observe(120,'fov-120');narrow=observe(60,'fov-60');restored=observe(0,'fov-restored')
    red=lambda v:(v>>16&255)>220 and (v&65535)<0x1010
    black=lambda v:v<0x080808
    assert red(original[0]) and black(original[1]),original
    assert black(wide[0]) and red(wide[2]),wide
    assert red(narrow[0]) and red(narrow[1]),narrow
    assert original[3]==wide[3]==narrow[3]==0x00ff00
    assert original==restored,(original,restored)
    print('GX_FOV_PIXELS_PASSED original+120+60+restore HUD unchanged ABI164 preserved',flush=True)
if __name__=='__main__':main(verify)
