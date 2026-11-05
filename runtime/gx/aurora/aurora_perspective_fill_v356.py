"""Let a perspective full-screen quad keep the retail width (v356).

Some titles draw fades and flashes as a 3D quad in front of the camera. It
covers exactly the original view, so the hor+ projection scale would shrink it
to the 4:3 centre. The bridge classifies such quads as screen fills; honour
that flag for perspective draws as well as orthographic ones.
"""
from pathlib import Path
import sys

root = Path(sys.argv[1])
path = root / 'lib/gx/shader_info.cpp'
text = path.read_text()
MARKER = 'GEKKOAOT_PERSPECTIVE_FILL_V356'
if MARKER not in text:
    before = '      (g_gxState.projType == GX_PERSPECTIVE && !gekkoaotOffscreen) ||'
    after = ('      // ' + MARKER + '\n'
             '      (g_gxState.projType == GX_PERSPECTIVE && !gekkoaotOffscreen &&\n'
             '       !gekkoaot_native_gx_screen_fill()) ||')
    if text.count(before) != 1:
        raise RuntimeError('shader_info.cpp: perspective fill anchor changed')
    path.write_text(text.replace(before, after))
