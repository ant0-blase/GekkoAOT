"""Render independent synthetic GX colors and check integer framebuffer logic."""
import ctypes as c
import ctypes.util
from pathlib import Path
import struct
import time


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
    def frame():
        bp(0x49,0);bp(0x4a,63|(63<<10));bp(0x4b,0x40000>>5);bp(0x4d,4);bp(0x4e,256);bp(0x52,1<<14)
        gx.gekkoaot_native_gx_present_xfb_ex(0x40000,0,64,128,64)
        gx.gekkoaot_native_gx_present()
    pixel=c.c_uint32()
    def check(expected,draw):
        # GXPeek is asynchronous. Pump frames until the GPU publishes the
        # requested snapshot; this timeout only fails the test, never the game.
        deadline=time.monotonic()+5
        while time.monotonic()<deadline:
            draw();gx.gekkoaot_native_gx_peek_argb(32,32,c.byref(pixel));frame()
            if pixel.value==expected:return
        raise AssertionError(f'expected {expected:08x}, got {pixel.value:08x}')
    def draw_red():
        bp(0x41,0x18);bp(0x64,0x20000>>5);bp(0x65,0x200|(1<<10));quad()
    def draw_logic(op):
        draw_red()
        bp(0x64,0x20020>>5);bp(0x65,0x200|(1<<10))
        bp(0x41,0x18|(1<<1)|(op<<12));quad()
    dst=(181,65,123);src=(123,125,181)
    packed=lambda rgb:0xff000000|(rgb[0]<<16)|(rgb[1]<<8)|rgb[2]
    check(packed(dst),draw_red)
    # Independent per-bit truth tables for all sixteen Boolean functions.
    tables=(0,8,4,12,2,10,6,14,1,9,5,13,3,11,7,15)
    for op,table in enumerate(tables):
        rgb=tuple(sum(((table>>(((s>>bit)&1)*2+((d>>bit)&1)))&1)<<bit
                      for bit in range(8)) for s,d in zip(src,dst))
        check(packed(rgb),lambda op=op:draw_logic(op))
    def overlapping_xor():
        draw_red();bp(0x64,0x20020>>5);bp(0x65,0x200|(1<<10))
        bp(0x41,0x18|(1<<1)|(6<<12));quad(2)
    check(packed(dst),overlapping_xor)
    # Normal draw after snapshots must retain projection, TEV and scissor.
    check(packed(dst),draw_red)
    gx.gekkoaot_native_gx_shutdown();print('GX_LOGIC_PIXELS_PASSED')

if __name__=='__main__':main()
