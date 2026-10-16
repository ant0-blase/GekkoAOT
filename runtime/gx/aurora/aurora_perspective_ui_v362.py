"""Clip the perspective cards of 2D screens to the retail frame (v362).

Some titles build their menus from flat cards in front of a 3D camera
(Pikmin's title and file-select screens): elements waiting to slide in sit
just outside the 4:3 frustum. Hor+ widens that frustum and shows them in the
side bands. The bridge marks perspective draws of 2D screens (no
depth-writing 3D geometry for several frames); they keep their position (hor+
and the UI transform scale x identically) but take the UI scissor.
"""
from pathlib import Path
import sys

root = Path(sys.argv[1])
MARKER = 'GEKKOAOT_PERSPECTIVE_UI_V362'
path = root / 'lib/gx/shader_info.cpp'
text = path.read_text()
if MARKER not in text:
    edits = [
        ('extern "C" bool gekkoaot_native_gx_screen_fill() noexcept;\n',
         'extern "C" bool gekkoaot_native_gx_screen_fill() noexcept;\n'
         '// ' + MARKER + '\n'
         'extern "C" bool gekkoaot_native_gx_perspective_ui() noexcept;\n'),
        ('''  if (scale == 1.f || g_gxState.projType != GX_ORTHOGRAPHIC ||
      gfx::is_offscreen() || gekkoaot_native_gx_screen_fill() ||''',
         '''  if (scale == 1.f ||
      (g_gxState.projType != GX_ORTHOGRAPHIC && !gekkoaot_native_gx_perspective_ui()) ||
      gfx::is_offscreen() || gekkoaot_native_gx_screen_fill() ||'''),
    ]
    for before, after in edits:
        if text.count(before) != 1:
            raise RuntimeError(f'shader_info.cpp: perspective UI anchor changed: {before[:80]}')
        text = text.replace(before, after)
    path.write_text(text)
