"""Keep proven opaque full-screen rectangles covering the full image."""
from pathlib import Path
import sys

path = Path(sys.argv[1]) / 'lib/gx/shader_info.cpp'
text = path.read_text()
if 'GEKKOAOT_SCREEN_FILL_V253' not in text:
    edits = [
        ('extern "C" bool gekkoaot_native_gx_screen_copy',
         '// GEKKOAOT_SCREEN_FILL_V253\n'
         'extern "C" bool gekkoaot_native_gx_screen_fill() noexcept;\n'
         'extern "C" bool gekkoaot_native_gx_screen_copy'),
        ('(g_gxState.projType == GX_ORTHOGRAPHIC && !gekkoaotOrthoSamplesEfbCopy));',
         '(g_gxState.projType == GX_ORTHOGRAPHIC && !gekkoaotOrthoSamplesEfbCopy &&\n'
         '       !gekkoaot_native_gx_screen_fill()));'),
    ]
    for before, after in edits:
        if text.count(before) != 1:
            raise RuntimeError(f'{path}: screen fill anchor changed')
        text = text.replace(before, after)
    path.write_text(text)
