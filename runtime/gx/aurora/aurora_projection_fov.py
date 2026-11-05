"""Adjust scene perspective focal lengths, preserving GX lens/depth conventions."""
from pathlib import Path
import sys
p=Path(sys.argv[1])/'lib/gx/shader_info.cpp'
s=p.read_text();marker='GEKKOAOT_PROJECTION_FOV_V1'
if marker not in s:
    old='  if constexpr (UseReversedZ) {\n    proj.m2 = proj.m2 * Vec4{-1.f, -1.f, -1.f, -1.f};'
    new='''  // GEKKOAOT_PROJECTION_FOV_V1: final scene projection, before capture/replay.
  // Orthographic UI and screen-derived render targets keep their own mapping.
  if (g_gxState.projType == GX_PERSPECTIVE && !gekkoaotTextureProducer &&
      !gekkoaot_native_gx_screen_fill() && !gekkoaot_native_gx_perspective_ui()) {
    const float scale=gekkoaot_native_gx_fov_scale(proj.m1[1]);
    if(scale!=1.f){proj.m0[0] *= scale;proj.m1[1] *= scale;}
  }
  if constexpr (UseReversedZ) {
    proj.m2 = proj.m2 * Vec4{-1.f, -1.f, -1.f, -1.f};'''
    anchor='#include "../gfx/gekkoaot_enhanced_renderer.hpp"'
    if s.count(old)!=1 or s.count(anchor)!=1:raise RuntimeError('FOV projection anchor changed')
    s=s.replace(anchor,anchor+'\nextern "C" float gekkoaot_native_gx_fov_scale(float vertical) noexcept;').replace(old,new)
    p.write_text(s)

# Keep coplanar far-plane UI visible when the retail projection rounds a few
# float ULPs outside the host clip volume. The native range is -w..0. This
# contraction keeps real out-of-range geometry clipped; it does not turn off
# depth testing or clipping (the same 1e-7 tolerance is used by Dolphin's XF).
s=p.read_text();marker='GEKKOAOT_XF_CLIP_PRECISION_V376'
if marker not in s:
    anchor='  if constexpr (UseReversedZ) {\n    proj.m2 = proj.m2 * Vec4{-1.f, -1.f, -1.f, -1.f};'
    assert s.count(anchor)==1
    s=s.replace(anchor,'  // '+marker+'\n  proj.m2 = proj.m2 * Vec4{1.f-1e-7f, 1.f-1e-7f, 1.f-1e-7f, 1.f-1e-7f};\n'+anchor)
    p.write_text(s)
