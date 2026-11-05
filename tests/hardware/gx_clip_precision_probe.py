"""Far-plane GX overlay roundoff must not discard UI; real overflow still clips."""
import ctypes as c
import struct
import time
import os
os.environ["GEKKOAOT_FRAME_INTERPOLATION"]="1"
from gx_interpolation_probe import main


def verify(gx, rect, frame, pixel, memory):
    def write(data):
        buf=c.create_string_buffer(data)
        gx.gekkoaot_native_gx_write_burst(buf,len(data),0x80004000)
    def bp(r,v):write(b'\x61'+struct.pack('>I',r<<24|v))
    def cp(r,v):write(b'\x08'+bytes([r])+struct.pack('>I',v))
    def xf(r,values):write(b'\x10'+struct.pack('>I',(len(values)-1)<<16|r)+b''.join(struct.pack('>f',v) if isinstance(v,float) else struct.pack('>I',v) for v in values))
    for _ in range(16):
        rect(0,0,64,64,1);frame();time.sleep(.05)
        gx.gekkoaot_native_gx_present_interpolated(1.)
        if pixel(32,32)==0xffffff:break
    assert pixel(32,32)==0xffffff,'fixture white control failed' 
    def draw(z):
        cp(0x50,1<<9);cp(0x60,1);cp(0x70,9|(1<<21)|(4<<22))
        xf(0x1020,[1.,0.,1.,0.,1.,0.,1]);bp(0,1);bp(0x28,1<<6)
        bp(0xc0,0x08fff8);bp(0xc1,0x08ffc0);bp(0x40,0);bp(0x41,0x18)
        rect(0,0,64,64,0)  # Black, inside the ordinary clip volume.
        xf(0x1020,[1.,0.,1.,0.,.001,-.0000001,1])
        bp(0x64,(0x20000+32)>>5);bp(0x65,0x200|(1<<10))
        verts=[(-.75,.75,z,0.,0.),(.75,.75,z,1.,0.),(.75,-.75,z,1.,1.),(-.75,-.75,z,0.,1.)]
        write(b'\x80'+struct.pack('>H',4)+b''.join(struct.pack('>5f',*v) for v in verts))
        sample=c.c_uint32();gx.gekkoaot_native_gx_peek_argb(32,32,c.byref(sample))
        frame();time.sleep(.05)
        gx.gekkoaot_native_gx_present_interpolated(1.)
        sample=c.c_uint32();gx.gekkoaot_native_gx_peek_argb(32,32,c.byref(sample))
        return sample.value & 0xffffff
    for _ in range(3):inside=draw(-1000.)
    assert inside & 0xffffff == 0xffffff,('far overlay clipped',hex(inside))
    for _ in range(3):outside=draw(-1001.)
    assert outside & 0xffffff == 0,('real overflow admitted',hex(outside))
    print('GX_CLIP_PRECISION_PASSED far_plane_ui=visible outside=clipped',flush=True)

if __name__=='__main__':main(verify)
