"""Original GX texture -> async material map -> normal shading and POM pixels."""
import ctypes as c
import os
from pathlib import Path
import struct
import time
os.environ['GEKKOAOT_ENHANCED_RENDERER'] = '1'
from gx_interpolation_probe import main
from gx_gi_ssr_probe import Settings as LegacySettings
class Settings(c.Structure):
    _fields_ = LegacySettings._fields_ + [("vsync",c.c_uint32),("texture_scale",c.c_uint32),("texture_quality",c.c_uint32),("hd_budget_mib",c.c_uint32),("hd_cache_mib",c.c_uint32),("material_enhancement",c.c_uint32),("pom_quality",c.c_uint32),("material_normal_strength",c.c_float),("material_relief",c.c_float),("material_compare",c.c_uint32),("geometry_refinement",c.c_uint32),("texture_backend",c.c_uint32),("displacement",c.c_uint32),("dof_quality",c.c_uint32),("dof_strength",c.c_float),("dof_focus",c.c_float),("particle_lights",c.c_uint32),("particle_light_strength",c.c_float),("particle_light_radius",c.c_float)]
assert c.sizeof(Settings) == 300
os.environ['GEKKOAOT_FRAME_INTERPOLATION'] = '1'  # shared bootstrap oracle


def verify(gx, rect, frame, pixel, memory):
    def write(data):
        buf = c.create_string_buffer(data)
        gx.gekkoaot_native_gx_write_burst(buf, len(data), 0x80004000)

    def bp(reg, value):
        write(b'\x61' + struct.pack('>I', reg << 24 | value))

    def cp(reg, value):
        write(b'\x08' + bytes([reg]) + struct.pack('>I', value))

    def xf(reg, values):
        write(b'\x10' + struct.pack('>I', (len(values)-1) << 16 | reg) +
              b''.join(struct.pack('>f' if isinstance(v, float) else '>I', v) for v in values))

    get = gx.gekkoaot_native_gx_get_graphics_state
    get.argtypes = [c.POINTER(Settings), c.c_uint32, c.POINTER(c.c_uint32)]
    get.restype = c.c_bool
    apply = gx.gekkoaot_native_gx_apply_graphics_settings
    apply.argtypes = [c.POINTER(Settings), c.c_uint32]
    apply.restype = c.c_bool
    settings = Settings()
    extent = (c.c_uint32*4)()
    assert get(c.byref(settings), c.sizeof(settings), extent)
    settings.post = settings.interpolation = settings.ao_quality = settings.contact_shadows = 0

    skyGeometry=os.environ.get("GEKKOAOT_TEST_SKY_GEOMETRY")=="1"
    sky=skyGeometry or os.environ.get("GEKKOAOT_TEST_SKY_UPSCALE")=="1"
    streaming=os.environ.get("GEKKOAOT_TEST_STREAMING_VIDEO")=="1"
    style2D=sky or streaming or os.environ.get("GEKKOAOT_TEST_2D_UPSCALE")=="1"
    textureSize=64 if skyGeometry else 256 if sky else 32 if streaming else 16 if style2D else 64
    texture=bytearray()
    for by in range(0,textureSize,4):
        for bx in range(0,textureSize,4):
            for yy in range(4):
                for xx in range(4):
                    x,y=bx+xx,by+yy
                    seam=x%16<2 or y%16<2
                    r,g,b=(25,22,20) if seam else (120+(x*17+y*31+x*y*3)%41,90+(x*7+y*11)%29,70+(x*13+y*5)%37)
                    if sky:r,g,b=10+(x*7+y*3)%38,40+(x*3+y*7)%55,190+(x*11+y*13)%65
                    if skyGeometry:r,g,b=(25,80,210) if (x//8+y//8)%2 else (160,190,235)
                    elif style2D and not sky:r,g,b=(235,235,220) if (x==y or x==y+1 or x==4 or x==11 or y==8) else (20,20,25)
                    texture+=struct.pack('>H',(r>>3)<<11|(g>>2)<<5|(b>>3))
    if style2D and not streaming and not sky:
        texture=bytearray()
        for by in range(0,textureSize,8):
            for bx in range(0,textureSize,8):
                for yy in range(8):
                    for xx in range(0,8,2):
                        values=[]
                        for d in (0,1):
                            x,y=bx+xx+d,by+yy
                            values.append(15 if (x==y or x==y+1 or x==4 or x==11 or y==8) else 0)
                        texture.append(values[0]*16+values[1])
    c.memmove(c.addressof(memory)+0x60000,bytes(texture),len(texture))
    settings.texture_scale=1;settings.material_enhancement=0;settings.pom_quality=0
    settings.environment=2;settings.environment_strength=.8;settings.sun_azimuth=60;settings.sun_elevation=35
    settings.sun_shadows=settings.gi=settings.ssr=0
    def render(enhanced, light_x=0., light_color=0xffffffff, normal=True, perspective=True, spot=False, textured=True, label=None, fragment=True, videoTone=0, frames=None,lens=1.,particleBlend='additive',particleColor=0xff7020ff,particleIndexed=False,particleCrowd=False,hudOverlay=False,particleContinuation=False,particleCover=False,particleVisible=True,particleBatch=False,particleMatrices=False,particleTexture=None,flareOverlay=False):
        settings.enhanced = enhanced
        settings.fragment_lighting = int(fragment)
        assert apply(c.byref(settings), c.sizeof(settings))
        for _ in range(frames if frames is not None else (240 if skyGeometry else 80 if os.environ.get("GEKKOAOT_TEST_SR_BACKEND")=="2" else 32)):
            if streaming:
                moving=bytearray()
                for by in range(0,textureSize,4):
                    for bx in range(0,textureSize,4):
                        for yy in range(4):
                            for xx in range(4):
                                x,y=bx+xx,by+yy
                                r,g,b=170+(x*7+y*3+_)%75,30+(x*11+y*7+_)%45,20+(x*13+y*5+_)%40
                                if videoTone:r,b=b,r
                                moving+=struct.pack('>H',(r>>3)<<11|(g>>2)<<5|(b>>3))
                c.memmove(c.addressof(memory)+0x60000,bytes(moving),len(moving))
                gx.gekkoaot_native_gx_memory_write.argtypes=[c.c_uint32,c.c_uint32]
                gx.gekkoaot_native_gx_memory_write(0x60000,len(moving))

            # Clear guest depth independently of color, then cover the target.
            bp(0x40, 1 << 4); bp(0x41, 0); bp(0x51, 0xffffff)
            bp(0x49, 0); bp(0x4a, 63 | 63 << 10); bp(0x4b, 0x50000 >> 5)
            bp(0x4d, 4); bp(0x4e, 256); bp(0x52, (1 << 14) | (1 << 11))
            cp(0x50, 1 << 9 | ((1 << 11) if normal else 0)); cp(0x60, 1)
            cp(0x70, 9 | (1 << 21) | (4 << 22) | ((4 << 10) if normal else 0))
            xf(0, [1., 0., 0., 0., 0., 1., 0., 0., 0., 0., 1., 0.])
            xf(0x400, [1., 0., 0., 0., 1., 0., 0., 0., 1.])
            xf(0x1009, [1]); xf(0x100a, [0]); xf(0x100c, [0xffffffff])
            xf(0x100e, [2 | 4 | (2 << 7) | (1 << 10) | ((1 << 9) if spot else 0)])
            if style2D:xf(0x100e,[0])
            xf(0x1010, [1 << 10])
            xf(0x603, [light_color, 0., 0. if spot else 1., 1. if spot else 0.,
                        1., 0., 0., light_x, 0., 0., 0., 0., 1.])
            xf(0x101a, [32., -32., 16777215., 374., 374., 16777215.])
            xf(0x1020, [lens, 0., lens, 0., -.01010101, -1.01010101, 0] if perspective else
                        [.25, 0., .25, 0., -.01, -.5, 1])
            bp(0, int(textured) | (1 << 4)); bp(0x28, (1 << 6) if textured else 0)
            bp(0x80,1 | (1<<2));bp(0x88,(textureSize-1)|((textureSize-1)<<10)|((0 if style2D and not streaming and not sky else 4)<<20));bp(0x94,0x60000>>5)
            xf(0x1040,[5<<7])
            bp(0x40, 1 | (3 << 1) | (1 << 4)); bp(0x41, 0x18)
            bp(0xc0, (0xf8af if textured else 0xfffa) | (1 << 19))
            bp(0xc1, (7 << 13) | (7 << 10) | (7 << 7) | (5 << 4) | (1 << 19))
            vertices = [(-4., 4., -5.), (4., 4., -9.), (4., -4., -9.), (-4., -4., -5.)]
            uvs=[(0.,0.),(1.,0.),(1.,1.),(0.,1.)]
            if skyGeometry:
                k=8.
                vertices=[(-k,-k,-k),(-k,k,-k),(k,k,-k),(k,-k,-k),(-k,-k,k),(k,-k,k),(k,k,k),(-k,k,k),(-k,k,-k),(-k,k,k),(k,k,k),(k,k,-k),(-k,-k,-k),(k,-k,-k),(k,-k,k),(-k,-k,k),(-k,-k,-k),(-k,-k,k),(-k,k,k),(-k,k,-k),(k,-k,-k),(k,k,-k),(k,k,k),(k,-k,k)]
                uvs=uvs*6
            write(b'\x80'+struct.pack('>H',len(vertices)) + b''.join(struct.pack('>8f' if normal else '>5f',
                *((v + (.4472136, 0., .8944272) if normal else v) + uv)) for v,uv in zip(vertices,uvs)))
            if os.environ.get('GEKKOAOT_TEST_PARTICLE_LIGHTS')=='1' and particleVisible:
                cp(0x50,1<<9);cp(0x60,0);cp(0x70,9|(4<<1))
                xf(0x100c,[particleColor]);xf(0x100e,[0])
                bp(0,1<<4);bp(0x28,0)
                bp(0xc0,0xfffa|(1<<19));bp(0xc1,(7<<13)|(7<<10)|(7<<7)|(5<<4)|(1<<19))
                bp(0x40,1|(3<<1))
                bp(0x41,0x139 if particleBlend=='additive' else 0x4b9)
                if particleCrowd:
                    xf(0x100c,[0x303030ff])
                    for index in range(256):
                        z=-5.-index*.5;x=z*.5;y=-z*.1
                        cloud=[(x-.025,y+.025,z),(x+.025,y+.025,z),(x+.025,y-.025,z),(x-.025,y-.025,z)]
                        write(b'\x80\x00\x04'+b''.join(struct.pack('>3f',*v) for v in cloud))
                    xf(0x100c,[particleColor])
                flame=[(.35,.5,-4.5),(1.35,.5,-4.5),(1.35,-.5,-4.5),(.35,-.5,-4.5)]
                if particleTexture is not None:
                    payload=struct.pack('>H',particleTexture)*(64*64)
                    c.memmove(c.addressof(memory)+0x6c000,payload,len(payload))
                    gx.gekkoaot_native_gx_memory_write.argtypes=[c.c_uint32,c.c_uint32]
                    gx.gekkoaot_native_gx_memory_write(0x6c000,len(payload))
                    cp(0x60,1);cp(0x70,9|(1<<21)|(4<<22));xf(0x100c,[0xffffffff])
                    bp(0,1|(1<<4));bp(0x28,1<<6);bp(0x88,63|(63<<10)|(4<<20));bp(0x94,0x6c000>>5)
                    bp(0xc0,0xf8af|(1<<19))
                    write(b'\x80\x00\x04'+b''.join(struct.pack('>5f',*(v+uv)) for v,uv in zip(flame,uvs[:4])))
                elif particleBatch:
                    decoy=[(-.5,.5,5.),(.5,.5,5.),(.5,-.5,5.),(-.5,-.5,5.)]
                    points=decoy*33+flame
                    write(b'\x80'+struct.pack('>H',len(points))+b''.join(struct.pack('>3f',*v) for v in points))
                elif particleMatrices:
                    cp(0x50,1|(1<<9));xf(12,[1.,0.,0.,0.,0.,1.,0.,0.,0.,0.,1.,0.])
                    decoy=[(-.5,.5,5.),(.5,.5,5.),(.5,-.5,5.),(-.5,-.5,5.)]
                    write(b'\x80\x00\x08'+b''.join(bytes([matrix])+struct.pack('>3f',*v) for matrix,points in ((0,decoy),(3,flame)) for v in points))
                elif particleIndexed:
                    # Deliberately wrong decoys at indices 0..3: a gathered
                    # observer that decodes rewritten indices sees no light.
                    points=[(500.,500.,500.)]*13+flame
                    payload=b''.join(struct.pack('>3f',*v) for v in points)
                    c.memmove(c.addressof(memory)+0x68000,payload,len(payload))
                    gx.gekkoaot_native_gx_memory_write.argtypes=[c.c_uint32,c.c_uint32]
                    gx.gekkoaot_native_gx_memory_write(0x68000,len(payload))
                    cp(0x50,3<<9);cp(0xa0,0x68000);cp(0xb0,12)
                    write(b'\x80\x00\x04'+struct.pack('>4H',13,14,15,16))
                else:
                    write(b'\x80\x00\x04'+b''.join(struct.pack('>3f',*v) for v in flame))
            if particleCover:
                # A HUD panel occludes only the source's screen pixels; it
                # cannot extinguish physical lighting on the surrounding 3D.
                cp(0x50,1<<9);cp(0x60,0);cp(0x70,9|(4<<1))
                xf(0x1020,[1.,0.,1.,0.,0.,-.5,1]);xf(0x100e,[0]);xf(0x100c,[0x000000ff])
                bp(0,1<<4);bp(0x28,0)
                bp(0xc0,0xfffa|(1<<19));bp(0xc1,(7<<13)|(7<<10)|(7<<7)|(5<<4)|(1<<19))
                bp(0x40,0);bp(0x41,0x4b9)
                cover=[(.05,.18,-.25),(.33,.18,-.25),(.33,-.18,-.25),(.05,-.18,-.25)]
                write(b'\x80\x00\x04'+b''.join(struct.pack('>3f',*v) for v in cover))
            if flareOverlay:
                # A black additive flare is a colour no-op even with opaque
                # alpha. Its quad must not cut a sharp rectangle out of DOF.
                cp(0x50,1<<9);cp(0x60,0);cp(0x70,9|(4<<1))
                xf(0x1020,[1.,0.,1.,0.,0.,-.5,1]);xf(0x100e,[0]);xf(0x100c,[0x000000ff])
                bp(0,1<<4);bp(0x28,0)
                bp(0xc0,0xfffa|(1<<19));bp(0xc1,(7<<13)|(7<<10)|(7<<7)|(5<<4)|(1<<19))
                bp(0x40,0);bp(0x41,0x139)
                flare=[(-.6,.6,-.25),(.6,.6,-.25),(.6,-.6,-.25),(-.6,-.6,-.25)]
                write(b'\x80\x00\x04'+b''.join(struct.pack('>3f',*v) for v in flare))
            if hudOverlay:
                # Translucent HUD over a distant receiver, not an empty ortho
                # scene. Partially covered texels must not inherit world DOF.
                cp(0x50,1<<9);cp(0x60,0);cp(0x70,9|(4<<1))
                xf(0x1020,[1.,0.,1.,0.,0.,-.5,1]);xf(0x100e,[0])
                bp(0,1<<4);bp(0x28,0)
                bp(0xc0,0xfffa|(1<<19));bp(0xc1,(7<<13)|(7<<10)|(7<<7)|(5<<4)|(1<<19))
                bp(0x40,0);bp(0x41,0x4b9)
                for index,alpha in enumerate((64,128,248,255)):
                    xf(0x100c,[0xff20ff00|alpha])
                    left=.1+index*.1
                    hud=[(left,.5,-.25),(left+.1,.5,-.25),(left+.1,.05,-.25),(left,.05,-.25)]
                    write(b'\x80\x00\x04'+b''.join(struct.pack('>3f',*v) for v in hud))
            if os.environ.get('GEKKOAOT_TEST_CAPTURE_CONTINUATION')=='1' or particleContinuation:
                # Publish world, clearing only guest Z. This ends the recorder
                # packet but NOT the logical world -> weapon composition.
                bp(0x40,1<<4);bp(0x41,0)
                bp(0x49,0);bp(0x4a,63|(63<<10));bp(0x4b,0x48000>>5)
                bp(0x4d,4);bp(0x4e,256);bp(0x52,(1<<14)|(1<<11))
                gx.gekkoaot_native_gx_present_xfb_ex(0x48000,0,64,128,64)
                cp(0x50,1<<9|1<<11);cp(0x60,1)
                cp(0x70,9|(1<<21)|(4<<22)|(4<<10))
                xf(0x100c,[0xffffffff])
                xf(0x1020,[lens,0.,lens,0.,-.001001001,-.010010010,0])
                bp(0x40,1|(3<<1)|(1<<4));bp(0x41,0x18)
                weapon=[(.015,-.015,-.03),(.025,-.015,-.03),(.025,-.025,-.03),(.015,-.025,-.03)]
                write(b'\x80\x00\x04'+b''.join(struct.pack('>8f',*(v+(.4472136,0.,.8944272)+uv)) for v,uv in zip(weapon,uvs[:4])))
            # XFB presentation already submits this display-copy boundary.
            # A second generic present would present the next, empty EFB.
            bp(0x49, 0); bp(0x4a, 63 | 63 << 10); bp(0x4b, 0x40000 >> 5)
            bp(0x4d, 4); bp(0x4e, 256); bp(0x52, 1 << 14)
            gx.gekkoaot_native_gx_present_xfb_ex(0x40000, 0, 64, 128, 64)
            time.sleep(.025)
        sample=pixel.snapshot()
        values = [sample(x,y) for y in range(18,46,2) for x in range(18,46,2)]
        if label and (directory := os.environ.get('GEKKOAOT_GPU_CAPTURE_DIR')):
            latest = Path(directory)/'latest-gpu.png'
            if latest.exists():
                latest.replace(Path(directory)/('fragment-'+label+'.png'))
        return values

    original=render(1 if skyGeometry or not style2D else 0,label='original',perspective=skyGeometry or not style2D)
    if os.environ.get('GEKKOAOT_TEST_CAPTURE_CONTINUATION')=='1':
        settings.dof_quality=2;settings.dof_focus=1.;settings.dof_strength=1.
        blurred=render(1,frames=12,label='world-weapon-dof')
        changed=sum(a!=b for a,b in zip(original,blurred))
        assert changed>20,('world effects lost across display-copy depth clear before weapon',changed)
        settings.dof_quality=0
        assert render(1,frames=3)==original,'continuation DOF OFF must restore world pixels'
        print('GX_MATERIAL_PIXELS_PASSED CAPTURE_CONTINUATION_PIXELS_PASSED world_effects=retained changed='+str(changed),flush=True)
        return
    if os.environ.get('GEKKOAOT_TEST_CAPTURE_LIFETIME')=='1':
        # Color survives every display copy. Guest Z is cleared at the start
        # of each new scene; forty distinct zoom lenses must not fill the
        # 15-entry per-image camera table with obsolete frame identities.
        settings.dof_focus=1.;settings.dof_strength=1.;settings.dof_quality=2
        for i in range(40):render(1,frames=3,lens=.8+i*.025)
        sharp_lens=1.775
        blurred=render(1,frames=3,lens=sharp_lens,label='zoom-dof-on')
        settings.dof_quality=0
        sharp=render(1,frames=3,lens=sharp_lens,label='zoom-dof-off')
        changed=sum(a!=b for a,b in zip(sharp,blurred))
        assert changed>20,('scene effects disappeared after repeated aim zooms',changed)
        print('GX_MATERIAL_PIXELS_PASSED CAPTURE_LIFETIME_PIXELS_PASSED lenses=40 changed='+str(changed),flush=True)
        return
    if os.environ.get('GEKKOAOT_TEST_PARTICLE_LIGHTS')=='1':
        settings.particle_lights=2;settings.particle_light_strength=4.
        lit=render(1,label='particle-light-on')
        def energy(values):return sum(sum((p>>shift)&255 for shift in (16,8,0)) for p in values)
        changed=sum(a!=b for a,b in zip(original,lit))
        assert changed>10 and energy(lit)>energy(original),('particle light did not illuminate receiver',changed,energy(original),energy(lit))
        settings.particle_lights=0
        assert render(1,label='particle-light-off',frames=3)==original,'particle light OFF must restore the original'
        settings.particle_lights=2
        hud=render(1,perspective=False,label='particle-hud')
        settings.particle_lights=0
        assert render(1,perspective=False,frames=3)==hud,'orthographic particles must not illuminate HUD'
        alphaOriginal=render(1,frames=3,particleBlend='alpha',label='alpha-light-off')
        settings.particle_lights=2
        alphaLit=render(1,frames=3,particleBlend='alpha',label='alpha-light-on')
        alphaChanged=sum(a!=b for a,b in zip(alphaOriginal,alphaLit))
        assert alphaChanged>10 and energy(alphaLit)>energy(alphaOriginal),('alpha-over fire did not light the receiver',alphaChanged)
        indexed=render(1,frames=3,particleBlend='alpha',particleIndexed=True,label='gathered-alpha-light-on')
        assert indexed==alphaLit,'gathered position indices must light exactly the same surface as direct positions'
        crowded=render(1,frames=3,particleBlend='alpha',particleCrowd=True,label='crowded-alpha-light-on')
        assert crowded==alphaLit,'early occluded smoke candidates must not starve the visible flame'
        assert render(1,frames=3,particleBlend='alpha',particleBatch=True,label='large-batch-fire')==alphaLit,'flame beyond vertex 128 of a batch was ignored'
        assert render(1,frames=3,particleBlend='alpha',particleMatrices=True,label='mixed-matrix-fire')==alphaLit,'different particle matrices suppressed an entire batch'
        print('PARTICLE_BATCHES_PASSED beyond_vertex128=exact mixed_matrices=exact',flush=True)
        settings.particle_lights=0
        coveredOriginal=render(1,frames=3,particleBlend='alpha',particleCover=True,label='covered-fire-off')
        settings.particle_lights=2
        coveredLit=render(1,frames=3,particleBlend='alpha',particleCover=True,label='covered-fire-on')
        coveredChanged=sum(a!=b for a,b in zip(coveredOriginal,coveredLit))
        assert coveredChanged>10 and energy(coveredLit)>energy(coveredOriginal),('HUD over source extinguished world light',coveredChanged)
        noSource=render(1,frames=3,particleVisible=False,label='removed-fire-on')
        settings.particle_lights=0
        assert render(1,frames=3,particleVisible=False,label='removed-fire-off')==noSource,'cached colour must not retain a removed light position'
        settings.particle_lights=2
        print('PARTICLE_RADIANCE_CACHE_PASSED covered_changed='+str(coveredChanged)+' removed_source=exact',flush=True)
        settings.particle_lights=0
        coldCovered=render(1,frames=3,particleTexture=0xfc00,particleBlend='alpha',particleCover=True,label='decoded-source-off')
        settings.particle_lights=2
        knownCovered=render(1,frames=3,particleTexture=0xfc00,particleBlend='alpha',particleCover=True,label='decoded-source-on')
        sourceChanged=sum(a!=b for a,b in zip(coldCovered,knownCovered))
        assert sourceChanged>10 and energy(knownCovered)>energy(coldCovered),('decoded source depends on visibility in the framebuffer',sourceChanged)
        neutralOn=render(1,frames=3,particleTexture=0x4208,particleBlend='alpha',label='decoded-neutral-on')
        settings.particle_lights=0
        assert render(1,frames=3,particleTexture=0x4208,particleBlend='alpha',label='decoded-neutral-off')==neutralOn,'neutral texture inherited an old fire light after its contents changed'
        settings.particle_lights=2
        print('DECODED_PARTICLE_SOURCE_PASSED hidden_source_changed='+str(sourceChanged)+' content_change=exact',flush=True)
        for name,color in [('smoke',0x303030ff),('foliage',0x208020ff)]:
            on=render(1,frames=3,particleBlend='alpha',particleColor=color,label=name+'-on')
            settings.particle_lights=0
            off=render(1,frames=3,particleBlend='alpha',particleColor=color,label=name+'-off')
            assert on==off,name+' must not emit light merely because it uses alpha blending'
            settings.particle_lights=2
        settings.particle_lights=0
        # Two XFB publications per scene need a settled compositor image.
        # This oracle checks retained lighting, not toggle/input latency.
        continuedOriginal=render(1,frames=12,particleBlend='alpha',particleContinuation=True,label='world-weapon-fire-off')
        settings.particle_lights=2
        continuedLit=render(1,frames=12,particleBlend='alpha',particleContinuation=True,label='world-weapon-fire-on')
        continuedChanged=sum(a!=b for a,b in zip(continuedOriginal,continuedLit))
        assert continuedChanged>10 and energy(continuedLit)>energy(continuedOriginal),('world light lost after copy and weapon pass',continuedChanged)
        print('PARTICLE_CONTINUATION_PASSED changed='+str(continuedChanged),flush=True)
        settings.particle_light_strength=.5;settings.particle_light_radius=.5
        closeLight=render(1,frames=8,particleBlend='alpha',label='fire-reach-small')
        settings.particle_light_radius=3.
        broadLight=render(1,frames=8,particleBlend='alpha',label='fire-reach-buildings')
        assert energy(broadLight)>energy(closeLight),'larger reach did not illuminate more distant receivers'
        redGain=sum(max(0,((b>>16)&255)-((a>>16)&255)) for a,b in zip(closeLight,broadLight))
        blueGain=sum(max(0,(b&255)-(a&255)) for a,b in zip(closeLight,broadLight))
        assert redGain>blueGain*2,('projected fire light did not stay orange',redGain,blueGain)
        print('PARTICLE_LIGHT_REACH_PASSED red_gain='+str(redGain)+' blue_gain='+str(blueGain),flush=True)
        print('ALPHA_FIRE_PIXELS_PASSED changed='+str(alphaChanged)+' gathered=exact smoke=unchanged foliage=unchanged',flush=True)
        print('GX_MATERIAL_PIXELS_PASSED PARTICLE_LIGHT_PIXELS_PASSED changed='+str(changed)+' off=exact hud=unchanged',flush=True)
        return
    if os.environ.get('GEKKOAOT_TEST_DOF')=='1':
        settings.dof_focus=1.;settings.dof_strength=1.;settings.dof_quality=2
        blurred=render(1,label='dof-on')
        changed=sum(a!=b for a,b in zip(original,blurred))
        assert changed>20,('DOF leaves distant textured pixels unchanged',changed)
        flare=render(1,frames=3,flareOverlay=True,label='dof-additive-flare')
        assert flare==blurred,'additive lens flare cuts its rectangular bounds out of world DOF'
        print('LENS_FLARE_DOF_PASSED additive_alpha_255=no_metadata_holes',flush=True)
        settings.dof_focus=500.
        assert render(1,label='dof-focus-far',frames=3)==original,'near side of focus plane should stay sharp'
        settings.dof_focus=1.;settings.dof_quality=0
        assert render(1,label='dof-off',frames=3)==original,'DOF OFF does not restore pixels'
        settings.dof_quality=2
        ortho=render(1,perspective=False,label='dof-hud')
        settings.dof_quality=0
        assert render(1,perspective=False,frames=3)==ortho,'DOF blurs orthographic text/HUD'
        hudSharp=render(1,frames=3,hudOverlay=True,label='dof-translucent-hud-off')
        settings.dof_quality=2
        hudDof=render(1,frames=3,hudOverlay=True,label='dof-translucent-hud-on')
        indices=[((y-18)//2)*14+(x-18)//2 for y in range(20,30,2) for x in range(36,46,2)]
        assert all(hudSharp[i]==hudDof[i] for i in indices),'DOF blurs translucent HUD over the world'
        assert sum(a!=b for a,b in zip(hudSharp,hudDof))>20,'HUD protection must leave world DOF active'
        print('TRANSLUCENT_HUD_DOF_PASSED alpha=64,128,248,255 protected=exact world=blurred',flush=True)
        print('GX_MATERIAL_PIXELS_PASSED DEPTH_OF_FIELD_PIXELS_PASSED changed='+str(changed)+' focus_far=sharp hud=unchanged off=exact',flush=True)
        return
    if streaming:
        settings.texture_scale=4;settings.texture_backend=2
        red=render(0,perspective=False,label='video-red')
        blue=render(0,perspective=False,label='video-blue',videoTone=1)
        changed=sum(a!=b for a,b in zip(original,red))
        assert changed>5,('streaming reconstruction unchanged',changed)
        assert sum((p>>16&255)>(p&255)+30 for p in red)>100,'red current frame missing'
        assert sum((p&255)>(p>>16&255)+30 for p in blue)>100,'video stuck on an obsolete red frame'
        settings.texture_scale=1
        assert render(0,perspective=False)==original,'video original mode does not restore'
        # Direct YUVY XFBs bypass GX textures. Exercise this separate path too.
        settings.texture_scale=4
        assert apply(c.byref(settings),c.sizeof(settings))
        def ram_video(blue=False):
            for n in range(8):
                data=bytearray()
                for y in range(16):
                    for x in range(0,16,2):
                        a=45+(x*17+y*23+n)%150;b=45+(x*13+y*19+n)%150
                        data+=bytes((a,200 if blue else 100,b,90 if blue else 190))
                c.memmove(c.addressof(memory)+0x100000,bytes(data),len(data))
                gx.gekkoaot_native_gx_present_xfb_ex(0x100000,0,16,32,16)
                time.sleep(.04)
            snapshot=pixel.snapshot();return snapshot(32,32)
        ramRed=ram_video();ramBlue=ram_video(True)
        assert (ramRed>>16&255)>(ramRed&255)+30,('RAM red video missing',hex(ramRed))
        assert (ramBlue&255)>(ramBlue>>16&255)+30,('RAM blue video frozen',hex(ramBlue))
        print('GX_MATERIAL_PIXELS_PASSED STREAMING_VIDEO_UPSCALE_PASSED changed='+str(changed)+' current_frames=red,blue ram_yuvy=red,blue',flush=True)
        return
    if style2D:
        for factor in (2,4):
            settings.texture_scale=factor;settings.texture_backend=2
            reconstructed=render(int(skyGeometry),perspective=skyGeometry,label='hud-'+str(factor))
            assert sum(a!=b for a,b in zip(original,reconstructed))>5,('orthographic font pixels unchanged',factor,original,reconstructed)
        settings.texture_scale=1
        assert render(int(skyGeometry),perspective=skyGeometry,frames=2)==original,'original 2D pixels restore within two frames'
        settings.texture_scale=4
        assert render(int(skyGeometry),perspective=skyGeometry,frames=2)==reconstructed,'warm HD glyph resources reactivate within two frames'
        settings.texture_scale=1
        assert render(int(skyGeometry),perspective=skyGeometry,frames=2)==original,'repeated OFF restores original pixels'
        assert c.string_at(c.addressof(memory)+0x60000,len(texture))==bytes(texture),'original font atlas changed'
        print('GX_MATERIAL_PIXELS_PASSED '+('SKY_GEOMETRY_UPSCALE_PASSED' if skyGeometry else 'BLUE_BACKGROUND_UPSCALE_PASSED' if sky else 'ORTHOGRAPHIC_FONT_UPSCALE_PASSED'),flush=True)
        return
    if os.environ.get('GEKKOAOT_TEST_NATIVE_CNN')=='1':
        settings.texture_backend=int(os.environ.get("GEKKOAOT_TEST_SR_BACKEND","1"));settings.texture_quality=1
        for factor in (2,4):
            settings.texture_scale=factor
            result=render(1,label='native-cnn-'+str(factor))
            delta=sum(a!=b for a,b in zip(original,result))
            assert delta>5,('native CNN GPU output did not change',factor,delta)
        assert c.string_at(c.addressof(memory)+0x60000,len(texture))==bytes(texture),'source GX texture changed'
        settings.texture_scale=1
        assert render(1)==original,'original GX rendering restores exactly'
        print('GX_MATERIAL_PIXELS_PASSED NATIVE_CNN_GPU_2X_4X',flush=True)
        return
    settings.material_enhancement=1;settings.material_normal_strength=2.
    material=render(1,label='materials')
    settings.material_compare=1
    comparison=render(1,label='comparison')
    xs=list(range(18,46,2))
    for index,value in enumerate(comparison):
        assert value==(original[index] if xs[index%len(xs)]<32 else material[index]),('comparison',index,value)
    settings.material_compare=0
    settings.pom_quality=2;settings.material_relief=.05
    relief=render(1,label='pom')
    delta=sum(a!=b for a,b in zip(original,material))
    pom_delta=sum(a!=b for a,b in zip(material,relief))
    assert delta>10,(delta,original,material)
    assert pom_delta>5,(pom_delta,material,relief)
    if os.environ.get('GEKKOAOT_TEST_DISPLACEMENT')=='1':
        settings.pom_quality=0
        settings.displacement=1
        displaced=render(1,label='displacement')
        assert sum(a!=b for a,b in zip(material,displaced))>5,('displaced GPU positions did not change rendered pixels',material,displaced)
        settings.displacement=0
        assert render(1)==material,'displacement off restores original geometry with material shading'
        print('GX_DISPLACEMENT_PIXELS_PASSED',flush=True)
    settings.material_enhancement=0
    assert render(1)==original,'disabling restores original GX texture shading'
    print('GX_MATERIAL_PIXELS_PASSED',delta,pom_delta,flush=True)

if __name__=='__main__':main(verify)
