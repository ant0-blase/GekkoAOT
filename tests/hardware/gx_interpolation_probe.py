"""Render independent synthetic GX colors and check integer framebuffer logic."""
import ctypes as c
import ctypes.util
from pathlib import Path
import struct
import time
import os
os.environ.setdefault("GEKKOAOT_NATIVE_GX_WIDTH", "640")
os.environ.setdefault("GEKKOAOT_NATIVE_GX_HEIGHT", "480")
os.environ.setdefault("GEKKOAOT_ASPECT_MODE", "4:3")
os.environ.setdefault("GEKKOAOT_INTERPOLATION_MODE", "motion")


def main():
    root=Path(__file__).resolve().parents[2]
    if sdl:=ctypes.util.find_library('SDL3'): c.CDLL(sdl,mode=c.RTLD_GLOBAL)
    gx=c.CDLL(str(root/'.gekkoaot/build/NativeGX-Aurora/libgekkoaot-native-gx.so'))
    memory=(c.c_uint8*(24*1024*1024))()
    resolver=c.CFUNCTYPE(c.c_bool,c.c_void_p,c.c_uint32,c.c_uint32,c.c_uint32,c.c_uint32,c.POINTER(c.c_void_p),c.POINTER(c.c_uint32))
    @resolver
    def resolve(user,address,size,space,resource,data,available):
        offset=address&0x3fffffff
        if offset+size>len(memory): return False
        data[0]=c.addressof(memory)+offset;available[0]=len(memory)-offset;return True
    gx.gekkoaot_native_gx_init.argtypes=[resolver,c.c_void_p];gx.gekkoaot_native_gx_init.restype=c.c_bool
    gx.gekkoaot_native_gx_write_burst.argtypes=[c.c_void_p,c.c_uint32,c.c_uint32]
    gx.gekkoaot_native_gx_peek_argb.argtypes=[c.c_uint16,c.c_uint16,c.POINTER(c.c_uint32)]
    assert gx.gekkoaot_native_gx_init(resolve,None)
    def write(data):
        for i in range(0,len(data),4096):
            chunk=data[i:i+4096];buf=c.create_string_buffer(chunk);gx.gekkoaot_native_gx_write_burst(buf,len(chunk),0x80004000)
    def bp(r,v):write(b'\x61'+struct.pack('>I',r<<24|v))
    def cp(r,v):write(b'\x08'+bytes([r])+struct.pack('>I',v))
    def xf(r,values):write(b'\x10'+struct.pack('>I',(len(values)-1)<<16|r)+b''.join(struct.pack('>f',v) if isinstance(v,float) else struct.pack('>I',v) for v in values))
    cp(0x50,1<<9);cp(0x60,1);cp(0x70,9|(1<<21)|(4<<22))
    xf(0,[1.,0.,0.,0.,0.,1.,0.,0.,0.,0.,1.,0.])
    xf(0x1008,[1<<4,0]);xf(0x1018,[60<<6]);xf(0x103f,[1]);xf(0x1040,[5<<7]);xf(0x1050,[61])
    xf(0x101a,[32.,-32.,16777215.,374.,374.,16777215.])
    xf(0x1020,[1.,0.,1.,0.,1.,0.,1])
    bp(0,1);bp(0x20,342|342<<12);bp(0x21,405|405<<12)
    bp(0x59,171|(171<<10));bp(0x40,0);bp(0x41,0x18);bp(0x43,0)
    bp(0xf3,7|(7<<16)|(7<<19));bp(0x28,1<<6)
    bp(0xc0,0xfff8|(1<<19));bp(0xc1,(7<<13)|(7<<10)|(7<<7)|(4<<4)|(1<<19))
    bp(0x30,7);bp(0x31,7);bp(0x80,0);bp(0x84,0)
    c.memset(c.addressof(memory)+0x10000,0xff,32)
    palette=[0x0000,0xffff,0xf800,0x07e0]
    for i,value in enumerate(palette):
        c.memmove(c.addressof(memory)+0x20000+i*32,bytes(30)+struct.pack('>H',value),32)
    bp(0x88,7|(7<<10)|(8<<20));bp(0x94,0x10000>>5);bp(0x98,0x200|(1<<10))
    def rect(x0,y0,x1,y1,color):
        bp(0x64,(0x20000+color*32)>>5);bp(0x65,0x200|(1<<10))
        verts=[(x0/32-1,1-y0/32,-.5,0.,0.),(x1/32-1,1-y0/32,-.5,1.,0.),
               (x1/32-1,1-y1/32,-.5,1.,1.),(x0/32-1,1-y1/32,-.5,0.,1.)]
        write(b'\x80'+struct.pack('>H',4)+b''.join(struct.pack('>5f',*v) for v in verts))
    def frame():
        bp(0x49,0);bp(0x4a,63|(63<<10));bp(0x4b,0x40000>>5);bp(0x4d,4);bp(0x4e,256);bp(0x52,1<<14)
        gx.gekkoaot_native_gx_present_xfb_ex(0x40000,0,64,128,64)
        gx.gekkoaot_native_gx_present()
    for phase in (0,1):
        for i in range(8):rect(i*8,0,(i+1)*8,64,(i+phase+1)%2)
        rect(0,0,8,8,2);frame()
    gx.gekkoaot_native_gx_present_interpolated.argtypes=[c.c_float]
    gx.gekkoaot_native_gx_present_interpolated.restype=c.c_bool
    assert gx.gekkoaot_native_gx_present_interpolated(.5)

    # Read the actual presentation window, including the interpolation pass.
    sdl=c.CDLL(ctypes.util.find_library('SDL3'))
    sdl.SDL_GetWindows.argtypes=[c.POINTER(c.c_int)];sdl.SDL_GetWindows.restype=c.POINTER(c.c_void_p)
    sdl.SDL_GetWindowProperties.argtypes=[c.c_void_p];sdl.SDL_GetWindowProperties.restype=c.c_uint32
    sdl.SDL_GetNumberProperty.argtypes=[c.c_uint32,c.c_char_p,c.c_int64];sdl.SDL_GetNumberProperty.restype=c.c_int64
    sdl.SDL_GetWindowSizeInPixels.argtypes=[c.c_void_p,c.POINTER(c.c_int),c.POINTER(c.c_int)]
    count=c.c_int();windows=sdl.SDL_GetWindows(c.byref(count));assert count.value>0
    window=windows[0];props=sdl.SDL_GetWindowProperties(window)
    xid=sdl.SDL_GetNumberProperty(props,b'SDL.window.x11.window',0);assert xid
    width,height=c.c_int(),c.c_int();sdl.SDL_GetWindowSizeInPixels(window,c.byref(width),c.byref(height))
    x11=c.CDLL(ctypes.util.find_library('X11'))
    x11.XOpenDisplay.argtypes=[c.c_char_p];x11.XOpenDisplay.restype=c.c_void_p
    x11.XGetImage.argtypes=[c.c_void_p,c.c_ulong,c.c_int,c.c_int,c.c_uint,c.c_uint,c.c_ulong,c.c_int];x11.XGetImage.restype=c.c_void_p
    x11.XGetPixel.argtypes=[c.c_void_p,c.c_int,c.c_int];x11.XGetPixel.restype=c.c_ulong
    x11.XDestroyImage.argtypes=[c.c_void_p]
    display=x11.XOpenDisplay(None);assert display
    def pixel(x,y):
        viewport_width=min(width.value,int(height.value*4/3))
        viewport_height=min(height.value,int(width.value*3/4))
        left=(width.value-viewport_width)//2;top=(height.value-viewport_height)//2
        image=x11.XGetImage(display,xid,left+int((x+.5)*viewport_width/64),top+int((y+.5)*viewport_height/64),1,1,~0,2)
        assert image
        value=x11.XGetPixel(image,0,0)&0xffffff;x11.XDestroyImage(image)
        return value
    def check(expected,samples):
        deadline=time.monotonic()+5
        values=[]
        while time.monotonic()<deadline:
            if not gx.gekkoaot_native_gx_present_interpolated(.5):
                time.sleep(.01);continue
            time.sleep(.01)
            values=[pixel(x,y) for x,y in samples]
            if all(abs((v&255)-(e&255))<24 and abs(((v>>8)&255)-((e>>8)&255))<24 and abs(((v>>16)&255)-((e>>16)&255))<24 for v,e in zip(values,expected)):return
        raise AssertionError(f'expected {expected}, got {values}, window={width.value}x{height.value}')
    check([0,0xffffff,0xffffff,0,0xff0000],[(2,32),(6,32),(10,32),(14,32),(4,4)])
    print('GPU motion halfway translation and static HUD passed',flush=True)
    rect(0,0,64,64,3);frame();rect(0,0,64,64,2);frame()
    check([0xff0000]*3,[(12,32),(32,32),(52,32)])
    print('GPU motion scene-cut rejection passed',flush=True)
    gx.gekkoaot_native_gx_shutdown()

if __name__=='__main__': main()
