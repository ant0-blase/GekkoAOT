"""Real GPU depth-based AO/contact and unchanged GX EFB checks on a synthetic scene."""
import ctypes as c
import os
from pathlib import Path
import struct
import time
os.environ['GEKKOAOT_ENHANCED_RENDERER']='1'
os.environ['GEKKOAOT_POST_PROCESSING']='0'
os.environ['GEKKOAOT_FRAME_INTERPOLATION']='0'
from gx_settings_probe import Settings as LegacySettings
from gx_interpolation_probe import main
class Settings(c.Structure):
    _fields_=LegacySettings._fields_+[(k,c.c_uint32) for k in ('hud','hud_items','unlock_predict','enhanced','ao_quality','contact_shadows')]+[(k,c.c_float) for k in ('ao_strength','ao_radius','shadow_strength','shadow_distance','shadow_thickness','shadow_softness','light_x','light_y','light_z')]
assert c.sizeof(Settings)==164

def verify(gx,rect,frame,pixel,memory):
    def write(data):
        b=c.create_string_buffer(data);gx.gekkoaot_native_gx_write_burst(b,len(data),0x80004000)
    def bp(r,v):write(b'\x61'+struct.pack('>I',r<<24|v))
    def cp(r,v):write(b'\x08'+bytes([r])+struct.pack('>I',v))
    def xf(r,values):write(b'\x10'+struct.pack('>I',(len(values)-1)<<16|r)+b''.join(struct.pack('>f',v) if isinstance(v,float) else struct.pack('>I',v) for v in values))
    get=gx.gekkoaot_native_gx_get_graphics_state;get.argtypes=[c.POINTER(Settings),c.c_uint32,c.POINTER(c.c_uint32)];get.restype=c.c_bool
    apply=gx.gekkoaot_native_gx_apply_graphics_settings;apply.argtypes=[c.POINTER(Settings),c.c_uint32];apply.restype=c.c_bool
    settings=Settings();extent=(c.c_uint32*4)();assert get(c.byref(settings),c.sizeof(settings),extent)
    settings.enhanced=1;settings.post=settings.interpolation=0;settings.ao_quality=settings.contact_shadows=0
    # The near plane is 1; relative effect lengths use four near-plane units.
    settings.ao_strength=2;settings.ao_radius=.5
    settings.shadow_strength=1;settings.shadow_distance=.5;settings.shadow_thickness=.0875;settings.shadow_softness=0
    settings.light_x=.7;settings.light_y=0;settings.light_z=.7
    c.memmove(c.addressof(memory)+0x30000,bytes(30)+struct.pack('>H',0x3fff),32)
    c.memmove(c.addressof(memory)+0x31000,struct.pack('>16H',*([0x9084]*16)),32)
    layered=prepass=False
    geometry_unit=1.
    horizontal_focal=1.
    viewport_depth=16777215.
    sample_points=((20,32),(7,32),(32,32),(3,3),(44,32),(3,58),(51,55))
    prepass_compare=2
    uniform_detour=False
    custom_lighting_normals=False
    transparent=True
    lighting=None
    negative_tev=False
    def quad(x0,y0,x1,y1,z,focal=1.):
        z*=geometry_unit
        vertices=[((x0/32-1)*-z/(focal*horizontal_focal),(1-y0/32)*-z/focal,z,0.,0.),((x1/32-1)*-z/(focal*horizontal_focal),(1-y0/32)*-z/focal,z,1.,0.),((x1/32-1)*-z/(focal*horizontal_focal),(1-y1/32)*-z/focal,z,1.,1.),((x0/32-1)*-z/(focal*horizontal_focal),(1-y1/32)*-z/focal,z,0.,1.)]
        if custom_lighting_normals:vertices=[v[:3]+(1.,0.,0.)+v[3:] for v in vertices]
        write(b'\x80'+struct.pack('>H',4)+b''.join(struct.pack('>8f' if custom_lighting_normals else '>5f',*v) for v in vertices))
    def depth_clear(color=False):
        # A real GX display copy with only Z update enabled clears the guest
        # depth buffer while retaining color. No private renderer test API.
        bp(0x41,(1<<3) if color else 0);bp(0x40,1<<4);bp(0x51,0xffffff)
        width,height=(int(extent[0]),int(extent[1])) if color else (64,64)
        bp(0x49,0);bp(0x4a,(width-1)|((height-1)<<10));bp(0x4b,0x50000>>5)
        bp(0x4d,(width*2+31)//32);bp(0x4e,256);bp(0x52,(1<<14)|(1<<11))
    def base_scene():
        depth_clear()
        cp(0x50,(1<<9)|((1<<11) if custom_lighting_normals else 0))
        cp(0x70,9|(1<<21)|(4<<22)|((4<<10) if custom_lighting_normals else 0))
        if custom_lighting_normals:xf(0x400,[1.,0.,0.,0.,1.,0.,0.,0.,1.])
        xf(0x101a,[32.,-32.,viewport_depth,374.,374.,viewport_depth])
        xf(0x1020,[horizontal_focal,0.,1.,0.,-.01010101,-1.01010101*geometry_unit,0])
        bp(0x40,1|(3<<1)|(1<<4));bp(0x41,0x4b9)
        bp(0xc0,0x2fff8 if negative_tev else 0xfff8|(1<<19))
        bp(0x64,(0x20000+(0 if negative_tev else 32))>>5);bp(0x65,0x200|(1<<10))
        if prepass:
            bp(0x41,0);quad(0,0,64,64,-4.);quad(24,20,40,44,-3.)
            bp(0x41,0x4b9);bp(0x40,1|(prepass_compare<<1)) # EQUAL/LEQUAL, Z writes off
        quad(0,0,64,64,-4.);quad(24,20,40,44,-3.)
        bp(0x40,1|(3<<1)|(1<<4))
        # A translucent, depth-writing material over the right AO band must
        # preserve the opaque receiver behind its own color contribution.
        # Opacity is a per-fragment decision; glass is not an opaque occluder.
        bp(0x64,0x30000>>5);bp(0x65,0x200|(1<<10));bp(0x98,0x200|(2<<10));
        if transparent:quad(42,20,47,44,-2.5)
        bp(0x98,0x200|(1<<10))
        if lighting:
            bp(0x64,0x31000>>5);bp(0x65,0x200|(1<<10))
            bp(0x40,1|(3<<1)|(1<<4))
            bp(0x41,(1<<11)|1|(1<<3) if lighting=='subtract' else 1|(1<<3)|(2<<8))
            quad(0,0,64,64,-4.)
            bp(0x41,0x4b9)
        if layered:
            depth_clear()
            # A close viewmodel uses its own projection and compressed Z range.
            # It must not replace world depth or occlude another camera in AO.
            xf(0x101a,[32.,-32.,1677721.5,374.,374.,1677721.5])
            xf(0x1020,[1.7*horizontal_focal,0.,1.7,0.,-.01010101,-1.01010101*geometry_unit,0])
            if uniform_detour:
                # Upload this lens in an excluded, zero-write draw. The next
                # real draw reuses the lens without another XF upload.
                bp(0x40,0);bp(0x41,0)
                quad(49,49,61,61,-3.,1.7)
            bp(0x40,1|(3<<1)|(1<<4));bp(0x41,0x4b9)
            bp(0x64,(0x20000+32)>>5);bp(0x65,0x200|(1<<10))
            quad(49,49,61,61,-3.,1.7);quad(54,53,57,57,-2.5,1.7)
        # An opaque perspective overlay with LEQUAL and no matching Z prepass
        # is not world geometry. It must clear retained world enhancement data.
        xf(0x101a,[32.,-32.,16777215.,374.,374.,16777215.])
        xf(0x1020,[1.2,0.,1.2,0.,-.01010101,-1.01010101*geometry_unit,0])
        bp(0x40,1|(3<<1));bp(0x41,0x4b9)
        bp(0x64,(0x20000+32)>>5);bp(0x65,0x200|(1<<10))
        quad(0,51,8,64,-2.7,1.2)
        # Opaque orthographic HUD must retain exactly its original pixels.
        cp(0x50,1<<9);cp(0x70,9|(1<<21)|(4<<22))
        xf(0x1020,[1.,0.,1.,0.,1.,0.,1]);bp(0x40,0);rect(0,0,8,8,2)
        bp(0x49,0);bp(0x4a,63|(63<<10));bp(0x4b,0x40000>>5);bp(0x4d,4);bp(0x4e,256);bp(0x52,1<<14)
        gx.gekkoaot_native_gx_present_xfb_ex(0x40000,0,64,128,64)
    scene = base_scene  # observe() calls scene() by late binding
    def observe(name):
        assert apply(c.byref(settings),c.sizeof(settings))
        for _ in range(8):scene();time.sleep(.04)
        values=[pixel(x,y)&0xffffff for x,y in sample_points]
        if directory:=os.environ.get('GEKKOAOT_GPU_CAPTURE_DIR'):
            latest=Path(directory)/'latest-gpu.png'
            if latest.exists():latest.replace(Path(directory)/(name+'.png'))
        efb=c.c_uint32()
        for _ in range(3):
            gx.gekkoaot_native_gx_peek_argb(16,32,c.byref(efb));scene();time.sleep(.04)
        print('ENHANCED_SAMPLES',name,[hex(v) for v in values],hex(efb.value),flush=True)
        return values,efb.value
    # The OGL presentation surface performs sRGB transfer; Vulkan's present
    # route here is UNORM. Compare composition equations in linear light.
    srgb=os.environ.get('GEKKOAOT_GRAPHICS_BACKEND')=='OGL'
    def linear(byte):
        v=byte/255
        return v/12.92 if srgb and v<=.04045 else ((v+.055)/1.055)**2.4 if srgb else v
    def encoded(v):
        v=max(0.,min(1.,v))
        return 255*(12.92*v if srgb and v<=.0031308 else 1.055*v**(1/2.4)-.055 if srgb else v)
    original,efb=observe('original')
    settings.ao_quality=3;ao,ao_efb=observe('ao')
    assert ao_efb==efb,(hex(efb),hex(ao_efb))
    assert ao[0]<original[0],(original,ao)
    assert all(ao[i]==original[i] for i in (2,3,5)),(original,ao)
    # Rising Sun and Animaniacs use this full-range XF scale/offset. The
    # decoded reversed range starts at -1 ULP; it is still a valid camera.
    viewport_depth=16777216.
    rounded,rounded_efb=observe('rounded-full-depth-range-ao')
    assert rounded==ao and rounded_efb==ao_efb,(ao,rounded)
    viewport_depth=16777215.
    transparent=False
    uncovered,_=observe('opaque-behind-glass')
    transparent=True
    # The 3/7 alpha glass attenuates the opaque correction by 4/7. It
    # contributes no invented depth/normal and its own white remains intact.
    expected=encoded(1.-(1.-linear(uncovered[4]&255))*4/7)
    assert abs((ao[4]&255)-expected)<=2,(ao,uncovered,expected)
    assert ao[4]>uncovered[4] and ao[4]<original[4],(original,ao,uncovered)
    for mode in ('subtract','modulate'):
        lighting=mode;settings.ao_quality=0
        light_original,light_efb=observe(mode+'-original')
        settings.ao_quality=3
        light_ao,light_ao_efb=observe(mode+'-ao')
        assert light_ao_efb==light_efb
        assert light_ao[3]==original[3],(mode,original,light_ao)
        for shift in (0,8,16):
            a=linear((ao[0]>>shift)&255);base=linear((light_original[0]>>shift)&255)
            expected=encoded(a-1.+base if mode=='subtract' else a*base)
            assert abs(((light_ao[0]>>shift)&255)-expected)<=2,(mode,shift,ao,light_original,light_ao,expected)
        assert light_ao[0]<light_original[0],(mode,light_original,light_ao)
    lighting=None
    settings.ao_quality=0;settings.contact_shadows=1;contact,contact_efb=observe('contact')
    assert contact_efb==efb
    assert contact[0]<original[0],(original,contact)
    assert contact[3]==original[3]
    settings.light_z=-.7;settings.shadow_thickness=2.
    backlight,_=observe('contact-behind-receiver')
    assert backlight==original,('a light behind the receiver cannot shade its front',original,backlight)
    settings.light_z=.7;settings.shadow_thickness=.0875
    settings.ao_quality=2;settings.msaa=4
    both,_=observe('ao-contact-msaa4')
    assert both[0]<original[0] and both[3]==original[3],(original,both)
    settings.msaa=1;settings.ao_quality=3;settings.contact_shadows=0
    layered=True
    retained,_=observe('world-plus-viewmodel')
    assert retained[:6]==ao[:6] and retained[6]<original[6],(ao,retained)
    uniform_detour=True
    reused,_=observe('eligible-reuses-excluded-projection')
    assert reused==retained,(retained,reused)
    prepass=True
    z_prepass,_=observe('z-prepass-world-plus-viewmodel')
    assert z_prepass==retained,(retained,z_prepass)
    prepass_compare=3
    lequal,_=observe('lequal-z-prepass-world-plus-viewmodel')
    assert lequal==retained,(retained,lequal)
    settings.msaa=4;settings.ao_quality=2;settings.contact_shadows=1
    layered_msaa,_=observe('layered-msaa4')
    assert layered_msaa[:6]==both[:6],(both,layered_msaa)
    geometry_unit=100.
    scaled,_=observe('layered-scaled-100x')
    assert all(abs((a&255)-(b&255))<=1 for a,b in zip(scaled,layered_msaa)),(layered_msaa,scaled)
    custom_lighting_normals=True
    custom,_=observe('custom-lighting-normals')
    assert custom==scaled,(scaled,custom)
    # Final unclamped TEV wraps to its eight-bit color before PE blending.
    # The -0.5 intermediate therefore becomes 0.5, not zero or signed HDR.
    # Check the original EFB and the contribution composition together.
    negative_tev=True;custom_lighting_normals=False;geometry_unit=1.;layered=prepass=False
    settings.msaa=1;settings.ao_quality=settings.contact_shadows=0
    signed_original,signed_efb=observe('wrapped-tev-original')
    settings.ao_quality=3
    signed_ao,signed_ao_efb=observe('wrapped-tev-ao')
    assert signed_ao_efb==signed_efb
    assert abs((signed_original[0]&255)-encoded(.5))<=1,(signed_original,encoded(.5))
    assert signed_ao[0]<signed_original[0],(signed_original,signed_ao)
    assert all(((a>>shift)&255)<=((b>>shift)&255) for a,b in zip(signed_ao,signed_original) for shift in (0,8,16)),(signed_original,signed_ao)
    negative_tev=False;bp(0xc0,0xfff8|(1<<19))
    # An anisotropic lens projects a spherical view-space AO radius to an
    # ellipse. The vertical blocker here is beyond the horizontal pixel radius
    # but lies INSIDE the same physical AO radius in Y.
    horizontal_focal=.25;transparent=False
    sample_points=((32,14),(32,8))
    settings.ao_quality=settings.contact_shadows=0
    anisotropic_original,anisotropic_efb=observe('anisotropic-lens-original')
    settings.ao_quality=3
    anisotropic_ao,anisotropic_ao_efb=observe('anisotropic-lens-ao')
    assert anisotropic_ao_efb==anisotropic_efb
    assert (anisotropic_original[0]&255)-(anisotropic_ao[0]&255)>=4,('vertical AO footprint uses vertical focal length',anisotropic_original,anisotropic_ao)
    horizontal_focal=1.;transparent=True
    sample_points=((20,32),(7,32),(32,32),(3,3),(44,32),(3,58),(51,55))
    # A distant oblique plane has no occluder. Subpixel AO/contact rays
    # must not turn floating-point cancellation or its own slope into shadows.
    custom_lighting_normals=False;geometry_unit=1.;layered=prepass=False
    settings.msaa=1;settings.ao_quality=2;settings.contact_shadows=1
    settings.ao_strength=.7;settings.ao_radius=1.
    settings.shadow_strength=.7;settings.shadow_distance=8.;settings.shadow_thickness=.25
    settings.light_x=-.5;settings.light_y=.7;settings.light_z=.3
    def contact_scene():
        depth_clear()
        cp(0x50,1<<9);cp(0x70,9|(1<<21)|(4<<22))
        xf(0x101a,[32.,-32.,16777215.,374.,374.,16777215.])
        xf(0x1020,[.367609,0.,.700208,0.,-.0000416684,-.05000209,0])
        bp(0x40,1|(3<<1)|(1<<4));bp(0x41,0x4b9)
        bp(0x64,(0x20000+32)>>5);bp(0x65,0x200|(1<<10))
        vertices=[]
        for x,y in [(0,0),(64,0),(64,64),(0,64)]:
            # Intersect view rays with one exact plane, including far sky scale.
            rx=(x/32-1)/.367609;ry=(1-y/32)/.700208
            z=-60./(1.+.2*rx+.3*ry)
            vertices.append((rx*-z,ry*-z,z,0.,0.))
        write(b'\x80'+struct.pack('>H',4)+b''.join(struct.pack('>5f',*v) for v in vertices))
        bp(0x49,0);bp(0x4a,63|(63<<10));bp(0x4b,0x40000>>5);bp(0x4d,4);bp(0x4e,256);bp(0x52,1<<14)
        gx.gekkoaot_native_gx_present_xfb_ex(0x40000,0,64,128,64)
    scene = contact_scene  # observe() calls scene() by late binding
    settings.ao_quality=settings.contact_shadows=0
    plane_original,_=observe('oblique-plane-original')
    settings.ao_quality=2
    plane_ao,_=observe('oblique-plane-ao')
    assert plane_ao==plane_original,('flat surface has no AO occluder',plane_original,plane_ao)
    settings.ao_quality=0;settings.contact_shadows=1
    plane_contact,_=observe('oblique-plane-contact')
    assert plane_contact==plane_original,('flat surface cannot contact-shadow itself',plane_original,plane_contact)
    # RGB-only clears invalidate all scene receivers even while GX alpha is
    # retained. Old palette entries must not exhaust during animated lenses.
    settings.contact_shadows=0;settings.ao_quality=3;settings.ao_strength=2;settings.ao_radius=.5
    lens=1.
    def lens_scene():
        nonlocal lens
        bp(0x43,1) # RGBA6: retain actual guest alpha through RGB-only clears
        depth_clear(color=True);lens+=.01
        cp(0x50,1<<9);cp(0x70,9|(1<<21)|(4<<22))
        xf(0x101a,[32.,-32.,16777215.,374.,374.,16777215.])
        xf(0x1020,[lens,0.,lens,0.,-.01010101,-1.01010101,0])
        bp(0x40,1|(3<<1)|(1<<4));bp(0x41,0x4b9)
        bp(0x64,(0x20000+32)>>5);bp(0x65,0x200|(1<<10))
        quad(0,0,64,64,-4.,lens);quad(24,20,40,44,-3.,lens)
        bp(0x49,0);bp(0x4a,63|(63<<10));bp(0x4b,0x40000>>5);bp(0x4d,4);bp(0x4e,256);bp(0x52,1<<14)
        gx.gekkoaot_native_gx_present_xfb_ex(0x40000,0,64,128,64)
    scene = lens_scene  # observe() calls scene() by late binding
    assert apply(c.byref(settings),c.sizeof(settings))
    for _ in range(32):scene();time.sleep(.04)
    moving,_=observe('rgb-only-clears-animated-lenses')
    assert (moving[0]&255)<250,('RGB clear must release camera palette entries',moving,lens)
    print('GX_ENHANCED_PIXELS_PASSED AO+contact+MSAA4 opaque-alpha-blend+translucent-contribution subtractive-and-multiplicative-lighting HUD unchanged EFB unchanged depth-clear-world-retained separate-viewmodel-camera Z-prepass scene-unit-invariance-100x custom-lighting-normal-basis',flush=True)
if __name__=='__main__':main(verify)
