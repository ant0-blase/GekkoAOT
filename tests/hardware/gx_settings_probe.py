"""Actual GPU pixel/attachment checks for live quality and post-processing."""
import ctypes as c
import os
import time
os.environ['GEKKOAOT_FRAME_INTERPOLATION']='1'
os.environ['GEKKOAOT_POST_PROCESSING']='0'
from gx_interpolation_probe import main
class Settings(c.Structure):
    _fields_=[(k,c.c_uint32) for k in ('output_width','output_height','fullscreen','render_percent','msaa','anisotropy','post','fxaa','interpolation','render_width','render_height')]+[(k,c.c_float) for k in ('sharpen','bloom','vignette','grain','tone','local_contrast','cinematic','brightness','contrast','saturation','gamma','exposure','temperature','vibrance','tint')]
def verify(gx,rect,frame,pixel,memory):
    apply=gx.gekkoaot_native_gx_apply_graphics_settings
    apply.argtypes=[c.POINTER(Settings),c.c_uint32]; apply.restype=c.c_bool
    get=gx.gekkoaot_native_gx_get_graphics_state
    get.argtypes=[c.POINTER(Settings),c.c_uint32,c.POINTER(c.c_uint32)]; get.restype=c.c_bool
    settings=Settings();extent=(c.c_uint32*4)(); assert get(c.byref(settings),c.sizeof(settings),extent)
    settings.brightness=settings.contrast=settings.saturation=settings.gamma=1
    settings.post=1;settings.interpolation=0
    settings.sharpen=settings.bloom=settings.vignette=settings.grain=settings.tone=settings.local_contrast=0
    assert not apply(c.byref(settings),0)
    def update():
        assert apply(c.byref(settings),c.sizeof(settings))
        # A quality transition must reopen the recorder before the guest's
        # next viewport/draw, not only before its next display copy.
        rect(0,0,64,64,2);frame()
        # Independent RAM XFB source: exact red YUVY, without relying on a
        # retail FIFO fixture's texture/TEV state. Three changing observations
        # deliberately exercise the existing RAM-XFB admission policy too.
        for y in (79,80,81):
            data=bytes([y,90,y,240])*(64*64//2)
            c.memmove(c.addressof(memory)+0x80000,data,len(data))
            gx.gekkoaot_native_gx_present_xfb_ex(0x80000,0,64,128,64)
            time.sleep(.03)
        for _ in range(10):
            gx.gekkoaot_native_gx_present_xfb_ex(0x80000,0,64,128,64)
            time.sleep(.03)
    update(); value=pixel(32,32);assert ((value>>16)&255)>230 and (value&65535)<4000,hex(value)
    settings.saturation=0;update();value=pixel(32,32)
    channels=[(value>>k)&255 for k in (16,8,0)]
    assert max(channels)-min(channels)<4 and 20<channels[0]<200,(hex(value),channels)
    settings.brightness=0;update();assert pixel(32,32)<0x030303
    settings.brightness=1;settings.saturation=1
    old_width,old_height=extent[0],extent[1]
    settings.render_percent=200;settings.msaa=4;settings.anisotropy=2
    update();assert get(c.byref(settings),c.sizeof(settings),extent)
    assert (extent[0],extent[1],extent[2],extent[3])==(old_width*2,old_height*2,4,2),list(extent)
    settings.render_percent=100;settings.msaa=1;settings.anisotropy=16
    update();assert get(c.byref(settings),c.sizeof(settings),extent)
    assert list(extent)==[old_width,old_height,1,16],list(extent)
    settings.render_width=1920;settings.render_height=1200;settings.render_percent=50
    update();assert get(c.byref(settings),c.sizeof(settings),extent)
    assert list(extent)[:2]==[960,600],list(extent)
    pause=gx.gekkoaot_native_gx_set_paused;pause.argtypes=[c.c_bool];pause.restype=None
    pause(True);gx.gekkoaot_native_gx_should_quit();pause(False)
    settings.render_width=settings.render_height=0;settings.render_percent=100
    settings.post=0;settings.fxaa=1;update()
    print('GPU live grayscale, brightness, render scale, MSAA and sampler quality passed',flush=True)
if __name__=='__main__':main(verify)
