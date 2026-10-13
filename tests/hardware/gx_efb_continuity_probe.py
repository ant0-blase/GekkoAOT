"""Check EFB persistence across display copies and partial overlay draws."""
import os
import ctypes as c
import ctypes.util
from pathlib import Path
import struct
import time


def main():
    root=Path(__file__).resolve().parents[2]
    if sdl:=ctypes.util.find_library('SDL3'): c.CDLL(sdl,mode=c.RTLD_GLOBAL)
    gx=c.CDLL(os.environ.get('GEKKOAOT_TEST_GX_LIBRARY',str(root/'.gekkoaot/build/NativeGX-Aurora/libgekkoaot-native-gx.so')))
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
    xf(0x101a,[320.,-240.,16777215.,662.,582.,16777215.])
    xf(0x1020,[1.,0.,1.,0.,1.,0.,1])
    bp(0,1);bp(0x20,342|342<<12);bp(0x21,981|821<<12)
    bp(0x59,171|(171<<10));bp(0x40,0);bp(0x41,0x18);bp(0x43,0)
    bp(0xf3,7|(7<<16)|(7<<19));bp(0x28,1<<6)
    bp(0xc0,0xfff8|(1<<19));bp(0xc1,(7<<13)|(7<<10)|(7<<7)|(4<<4)|(1<<19))
    bp(0x30,7);bp(0x31,7);bp(0x80,0);bp(0x84,0)
    # CI4 palette entries use partially overlapping bit patterns so arithmetic
    # approximations cannot accidentally satisfy the integer logic tests.
    c.memset(c.addressof(memory)+0x10000,0xff,32)
    c.memmove(c.addressof(memory)+0x20000,bytes(30)+struct.pack('>H',(22<<11)|(16<<5)|15),32)
    c.memmove(c.addressof(memory)+0x20020,bytes(30)+struct.pack('>H',(15<<11)|(31<<5)|22),32)
    bp(0x88,7|(7<<10)|(8<<20));bp(0x94,0x10000>>5)
    bp(0x64,0x20000>>5);bp(0x65,0x200|(1<<10));bp(0x98,0x200|(1<<10))
    def quad(repeats=1):
        verts=[(-1.,-1.,-.5,0.,0.),(1.,-1.,-.5,1.,0.),(1.,1.,-.5,1.,1.),(-1.,1.,-.5,0.,1.)]
        write(b'\x80'+struct.pack('>H',4*repeats)+b''.join(struct.pack('>5f',*v) for v in verts*repeats))
    def frame(clear=False):
        bp(0x49,0);bp(0x4a,639|(479<<10));bp(0x4b,0x40000>>5);bp(0x4d,40);bp(0x4e,256);bp(0x52,(1<<14)|(int(clear)<<11))
        gx.gekkoaot_native_gx_present_xfb_ex(0x40000,0,640,1280,480)
        # VI scanout is the consumer; the legacy present API ends a live EFB.
    pixel=c.c_uint32()
    def check(expected,draw):
        # GXPeek is asynchronous. Pump frames until the GPU publishes the
        # requested snapshot; this timeout only fails the test, never the game.
        deadline=time.monotonic()+5
        while time.monotonic()<deadline:
            draw();gx.gekkoaot_native_gx_peek_argb(320,240,c.byref(pixel));frame()
            if pixel.value==expected:return
        raise AssertionError(f'expected {expected:08x}, got {pixel.value:08x}')
    def draw_red():
        bp(0x41,0x18);bp(0x64,0x20000>>5);bp(0x65,0x200|(1<<10));quad()
    def draw_logic(op):
        draw_red()
        bp(0x64,0x20020>>5);bp(0x65,0x200|(1<<10))
        bp(0x41,0x18|(1<<1)|(op<<12));quad()
    packed=lambda rgb:0xff000000|(rgb[0]<<16)|(rgb[1]<<8)|rgb[2]
    red=packed((181,65,123));green=packed((123,125,181))
    check(red,draw_red)
    # XF invalidates the decoded alias of BP GENMODE. An equal BP word must
    # restore one texgen instead of being removed by the bridge's word cache.
    xf(0x103f,[0]);bp(0,1)
    check(red,draw_red)
    bp(0x41,0x18);bp(0x64,0x20020>>5);bp(0x65,0x200|(1<<10))
    verts=[(-.5,-.5,-.5,0.,0.),(.5,-.5,-.5,1.,0.),(.5,.5,-.5,1.,1.),(-.5,.5,-.5,0.,1.)]
    write(b'\x80'+struct.pack('>H',4)+b''.join(struct.pack('>5f',*v) for v in verts))
    frame()
    edge=c.c_uint32()
    def observe(center_value,edge_value):
        deadline=time.monotonic()+5
        while time.monotonic()<deadline:
            gx.gekkoaot_native_gx_peek_argb(320,240,c.byref(pixel))
            gx.gekkoaot_native_gx_peek_argb(80,60,c.byref(edge));frame()
            if pixel.value==center_value and edge.value==edge_value:return
        raise AssertionError(f'center={pixel.value:08x}, edge={edge.value:08x}, expected={center_value:08x}/{edge_value:08x}')
    # No redraw of the background or overlay: empty copies must preserve both.
    observe(green,red)
    for _ in range(5):frame()
    observe(green,red)
    # BP59 changes raster and scissor origins together, even with no new XF
    # viewport write. Rebuild a sharp quad so the two-pixel translation is visible.
    def edge_probe(x,expected):
        deadline=time.monotonic()+5
        while time.monotonic()<deadline:
            gx.gekkoaot_native_gx_peek_argb(x,240,c.byref(pixel));frame()
            if pixel.value==expected:return
        raise AssertionError(f'BP59 pixel x={x}: {pixel.value:08x}, expected {expected:08x}')
    edge_probe(159,red);edge_probe(161,green)
    bp(0x59,170|(170<<10))
    draw_red();bp(0x64,0x20020>>5);bp(0x65,0x200|(1<<10))
    write(b'\x80'+struct.pack('>H',4)+b''.join(struct.pack('>5f',*v) for v in verts))
    edge_probe(161,red);edge_probe(163,green)
    bp(0x59,171|(171<<10))
    # A guest-requested clear still clears the subsequent EFB, not the XFB
    # snapshot already taken by this copy.
    bp(0x4f,0);bp(0x50,0);bp(0x51,0xffffff);bp(0x40,1<<4)
    frame(True);observe(0xff000000,0xff000000)
    # A HUD projection at the end of the frame must not hide the last camera.
    signature=gx.gekkoaot_native_gx_camera_signature
    signature.argtypes=[c.POINTER(c.c_float),c.POINTER(c.c_float)]
    signature.restype=c.c_bool
    fov=c.c_float();aspect=c.c_float()
    assert not signature(c.byref(fov),c.byref(aspect))
    xf(0x1020,[1.5,0.,2.,0.,-1.002,-.2002,0]);quad()
    xf(0x1020,[1.,0.,1.,0.,1.,0.,1]);quad()
    assert signature(c.byref(fov),c.byref(aspect))
    assert abs(fov.value-53.130102)<.001 and abs(aspect.value-4/3)<.001,(fov.value,aspect.value)
    # A smaller auxiliary camera must not replace the main viewport camera.
    xf(0x101a,[64.,-64.,16777215.,406.,406.,16777215.])
    xf(0x1020,[1.,0.,1.,0.,-1.002,-.2002,0]);quad()
    xf(0x1020,[1.,0.,1.,0.,1.,0.,1])
    assert signature(c.byref(fov),c.byref(aspect)) and abs(fov.value-53.130102)<.001
    frame();frame()
    assert signature(c.byref(fov),c.byref(aspect))
    frame()
    assert not signature(c.byref(fov),c.byref(aspect))
    gx.gekkoaot_native_gx_shutdown();print('GX_EFB_CONTINUITY_PIXELS_PASSED')
    print('GX_CAMERA_SIGNATURE_PASSED')

if __name__=='__main__':main()
