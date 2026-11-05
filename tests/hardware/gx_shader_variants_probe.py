"""48 legal TEV variants with identical colors; every async draw must appear."""
import ctypes as c
import os
from pathlib import Path
import struct
import time
os.environ['GEKKOAOT_FRAME_INTERPOLATION']='1'
os.environ['GEKKOAOT_POST_PROCESSING']='0'
from gx_interpolation_probe import main

def verify(gx,rect,frame,pixel,memory):
    def bp(r,v):
        b=c.create_string_buffer(b'\x61'+struct.pack('>I',r<<24|v))
        gx.gekkoaot_native_gx_write_burst(b,5,0x80004000)
    def scene():
        bp(0xc0,0xfff8|(1<<19));bp(0xc1,(7<<13)|(7<<10)|(7<<7)|(4<<4)|(1<<19))
        rect(0,0,64,64,0)
        for index in range(48):
            # D + mix(A,B,C): A=zero, C=zero, D=texture. B can vary
            # legally without changing any output color or alpha. Different
            # TEV configs still require distinct pipeline-cache entries.
            bp(0xc0,(15<<12)|((index%16)<<8)|(15<<4)|8|(1<<19))
            bp(0xc1,(7<<13)|((index//16)<<10)|(7<<7)|(4<<4)|(1<<19))
            rect(index,0,index+1,64,1)
        bp(0x49,0);bp(0x4a,63|(63<<10));bp(0x4b,0x40000>>5);bp(0x4d,4);bp(0x4e,256);bp(0x52,1<<14)
        gx.gekkoaot_native_gx_present_xfb_ex(0x40000,0,64,128,64)
    for _ in range(3):scene();time.sleep(.03)
    # Actual compositor capture, once: inspect all 48 distinct stripes and the
    # untouched black region. A single last-draw check could hide skipped work.
    pixel(32,32)
    from PIL import Image
    image=Image.open(Path(os.environ['GEKKOAOT_GPU_CAPTURE_DIR'])/'latest-gpu.png').convert('RGB')
    w,h=image.size;vw=min(w,int(h*4/3));vh=min(h,int(w*3/4))
    # The last stripe borders black and the presentation sampler filters its
    # edge. Keep the oracle strict enough to reject any missing stripe.
    for x in range(64):
        value=image.getpixel(((w-vw)//2+int((x+.5)*vw/64),(h-vh)//2+vh//2))
        assert (min(value)>230 if x<48 else max(value)<10),(x,value)
    print('GX_SHADER_VARIANTS_PIXELS_PASSED variants=48 all-stripes-visible',flush=True)
if __name__=='__main__':main(verify)
