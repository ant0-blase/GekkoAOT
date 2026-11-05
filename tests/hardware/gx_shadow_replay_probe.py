"""Actual indexed geometry replay into independent light-space depth targets.

The synthetic fixture explicitly knows its view space. This does not prove a
retail game's common camera/world frame or test composed CSM shadows.
"""
import ctypes as c
import os
import struct
import time
from gx_fragment_lighting_probe import Settings
from gx_interpolation_probe import main

def verify(gx, rect, frame, pixel, memory):
    def write(data):
        buf=c.create_string_buffer(data)
        gx.gekkoaot_native_gx_write_burst(buf,len(data),0x80004000)
    def bp(r,v):write(b'\x61'+struct.pack('>I',r<<24|v))
    def cp(r,v):write(b'\x08'+bytes([r])+struct.pack('>I',v))
    def xf(r,values):write(b'\x10'+struct.pack('>I',(len(values)-1)<<16|r)+b''.join(
        struct.pack('>f' if isinstance(v,float) else '>I',v) for v in values))
    get=gx.gekkoaot_native_gx_get_graphics_state
    get.argtypes=[c.POINTER(Settings),c.c_uint32,c.POINTER(c.c_uint32)];get.restype=c.c_bool
    apply=gx.gekkoaot_native_gx_apply_graphics_settings
    apply.argtypes=[c.POINTER(Settings),c.c_uint32];apply.restype=c.c_bool
    settings=Settings();extent=(c.c_uint32*4)()
    assert get(c.byref(settings),c.sizeof(settings),extent)
    settings.enhanced=1
    settings.post=settings.interpolation=settings.ao_quality=settings.contact_shadows=0
    assert apply(c.byref(settings),c.sizeof(settings))
    replay=gx.gekkoaot_native_gx_shadow_depth_validation
    replay.argtypes=[c.POINTER(c.c_float),c.c_uint32];replay.restype=c.c_bool
    assert not replay((c.c_float*16)(*([float('nan')]*16)),64)
    assert not replay((c.c_float*16)(*([1.]*16)),63)
    assert not replay((c.c_float*16)(*([1.]*16)),64)

    events=[]
    callback_type=c.CFUNCTYPE(None,c.c_void_p,c.c_uint32,c.c_uint32)
    callback=callback_type(lambda _,r,v:events.append((r,v)))
    gx._shadow_callback=callback
    gx.gekkoaot_native_gx_set_pe_callback.argtypes=[callback_type,c.c_void_p]
    gx.gekkoaot_native_gx_set_pe_callback(callback,None)
    c.memmove(c.addressof(memory)+0x31000,bytes(30)+struct.pack('>H',0x0fff),32)
    array=0x70000

    def scene(tx=0.,transparent=False):
        bp(0x40,1<<4);bp(0x41,0);bp(0x51,0xffffff)
        bp(0x49,0);bp(0x4a,63|(63<<10));bp(0x4b,0x50000>>5)
        bp(0x4d,4);bp(0x4e,256);bp(0x52,(1<<14)|(1<<11))
        cp(0x50,1<<9);cp(0x60,1);cp(0x70,9|(1<<21)|(4<<22))
        xf(0,[1.,0.,0.,0.,0.,1.,0.,0.,0.,0.,1.,0.])
        xf(0x101a,[32.,-32.,16777215.,374.,374.,16777215.])
        xf(0x1020,[1.,0.,1.,0.,-.01010101,-1.01010101,0])
        bp(0,1);bp(0x28,1<<6);bp(0x40,1|(3<<1)|(1<<4));bp(0x41,0x18)
        bp(0xc0,0xfff8|(1<<19));bp(0xc1,(7<<13)|(7<<10)|(7<<7)|(4<<4)|(1<<19))
        bp(0xf3,128|(6<<16)|(7<<19))
        bp(0x98,0x200|(1<<10));bp(0x64,(0x20000+32)>>5);bp(0x65,0x200|(1<<10))
        vertices=[(-4.,4.,-4.,0.,0.),(4.,4.,-4.,1.,0.),(4.,-4.,-4.,1.,1.),(-4.,-4.,-4.,0.,1.)]
        write(b'\x80\x00\x04'+b''.join(struct.pack('>5f',*v) for v in vertices))
        if transparent:
            bp(0x98,0x200|(2<<10));bp(0x64,0x31000>>5);bp(0x65,0x200|(1<<10))
        positions=[(tx-1,1.,-2.),(tx+1,1.,-2.),(tx+1,-1.,-2.),(tx-1,-1.,-2.)]
        c.memmove(c.addressof(memory)+array,b''.join(struct.pack('>3f',*v) for v in positions),48)
        cp(0x50,3<<9);cp(0xa0,array);cp(0xb0,12)
        write(b'\x80\x00\x04'+b''.join(struct.pack('>H2f',i,u,v) for i,(u,v) in
              enumerate(((0.,0.),(1.,0.),(1.,1.),(0.,1.)))))
        # Guest RAM may change after submission; replay must consume the
        # frame-owned GPU snapshot and not decode these vertices again.
        c.memset(c.addressof(memory)+array,0,48)
        before=len(events);bp(0x48,0x2468)
        assert events[before:]==[(0x48,0x2468)]
        bp(0x49,0);bp(0x4a,63|(63<<10));bp(0x4b,0x40000>>5)
        bp(0x4d,4);bp(0x4e,256);bp(0x52,1<<14)
        gx.gekkoaot_native_gx_present_xfb_ex(0x40000,0,64,128,64)

    def observe(matrix,expected,tx=0.,transparent=False):
        assert replay((c.c_float*16)(*matrix),64)
        before=len(events)
        for _ in range(6):scene(tx,transparent);time.sleep(.035)
        values=[pixel(x,32)&255 for x in (8,32,48)]
        if os.environ.get('GEKKOAOT_GRAPHICS_BACKEND')=='OGL':
            values=[round(255*((v/255/12.92) if v/255<=.04045 else ((v/255+.055)/1.055)**2.4)) for v in values]
        assert all(abs(a-b)<=3 for a,b in zip(values,expected)),(values,expected)
        assert len(events)-before==6,'GPU replay must not duplicate PE tokens'

    # Independent orthographic light camera: original projection remains
    # perspective. Nearer indexed blocker writes .8, background writes .6.
    directional=[.25,0,0,0, 0,.25,0,0, 0,0,.1,1, 0,0,0,1]
    observe(directional,[153,204,153])
    observe(directional,[153,153,204],tx=2.)
    observe(directional,[153,153,153],transparent=True)
    # Negative-Z cubemap face, reversed depth, near=1/far=10.
    point=[-1,0,0,0, 0,1,0,0, 0,0,1/9,10/9, 0,0,-1,0]
    observe(point,[43,113,43])
    assert replay(None,0)
    print('GX_SHADOW_REPLAY_PIXELS_PASSED indexed_snapshot dynamic_geometry alpha_test directional_and_point_depth tokens_once',flush=True)

if __name__=='__main__':main(verify)
