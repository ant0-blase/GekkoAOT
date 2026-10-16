"""Pinned Aurora adapters for independent render scale and filtering override."""
from pathlib import Path
import sys
root=Path(sys.argv[1])
def replace(path,marker,before,after):
    p=root/path;s=p.read_text()
    if marker in s:return
    if s.count(before)!=1:raise RuntimeError(f'{path}: graphics setting anchor changed')
    p.write_text(s.replace(before,after))
replace('lib/window.cpp','GEKKOAOT_RENDER_SCALE', '#include "window.hpp"',
'''#include "window.hpp"
// GEKKOAOT_RENDER_SCALE: output/window pixels stay independent of the 3D EFB.
extern "C" float gekkoaot_native_gx_render_scale() noexcept;''')
replace('lib/window.cpp','GEKKOAOT_RENDER_EXTENT','  const float scale = SDL_GetWindowDisplayScale(g_window);',
'''  // GEKKOAOT_RENDER_EXTENT
  const float renderScale = gekkoaot_native_gx_render_scale();
  const float boundedScale = std::min(renderScale, 8192.f / float(std::max(fb_w, fb_h)));
  fb_w = std::max(1, int(float(fb_w) * boundedScale));
  fb_h = std::max(1, int(float(fb_h) * boundedScale));
  const float scale = SDL_GetWindowDisplayScale(g_window);''')
replace('lib/gfx/texture.cpp','GEKKOAOT_ANISOTROPY_OVERRIDE',
'  u16 maxAnisotropy = wgpu_aniso(texObj.max_aniso());',
'''  // GEKKOAOT_ANISOTROPY_OVERRIDE: no effect on non-mipmapped UI/copy textures.
  u16 maxAnisotropy = webgpu::g_graphicsConfig.textureAnisotropy;''')
replace('lib/window.cpp','GEKKOAOT_OPENGL_WINDOW',
'  SDL_WindowFlags flags = SDL_WINDOW_HIGH_PIXEL_DENSITY;',
'''  SDL_WindowFlags flags = SDL_WINDOW_HIGH_PIXEL_DENSITY;
  // GEKKOAOT_OPENGL_WINDOW: select an EGL-compatible X11 visual for Dawn GL.
  if (backend == BACKEND_OPENGL || backend == BACKEND_OPENGLES) {
    SDL_SetHint(SDL_HINT_VIDEO_FORCE_EGL, "1");
    flags |= SDL_WINDOW_OPENGL;
  }''')

replace('lib/window.cpp','GEKKOAOT_RENDER_REFERENCE_DECL',
 'extern "C" float gekkoaot_native_gx_render_scale() noexcept;',
 'extern "C" float gekkoaot_native_gx_render_scale() noexcept;\n// GEKKOAOT_RENDER_REFERENCE_DECL\nextern "C" void gekkoaot_native_gx_render_extent(int*, int*) noexcept;')
replace('lib/window.cpp','GEKKOAOT_RENDER_REFERENCE_APPLY',
 '  const float renderScale = gekkoaot_native_gx_render_scale();',
 '  // GEKKOAOT_RENDER_REFERENCE_APPLY\n  gekkoaot_native_gx_render_extent(&fb_w, &fb_h);\n  const float renderScale = gekkoaot_native_gx_render_scale();')
