"""Preserve observed EFB texture producer projections in widescreen."""
from pathlib import Path
import sys

path = Path(sys.argv[1]) / 'lib/gx/shader_info.cpp'
text = path.read_text()
marker = 'GEKKOAOT_TEXTURE_VIEWPORT_V250'
if marker not in text:
    edits = [
        ('extern "C" bool gekkoaot_native_gx_screen_copy',
         '// GEKKOAOT_TEXTURE_VIEWPORT_V250\n'
         'extern "C" bool gekkoaot_native_gx_texture_viewport(float, float, float, float) noexcept;\n'
         'extern "C" bool gekkoaot_native_gx_screen_copy'),
        ('  const bool gekkoaotApplyWideX =\n',
         '  const bool gekkoaotTextureProducer = gekkoaotOffscreen ||\n'
         '      gekkoaot_native_gx_texture_viewport(gekkoaotView.left, gekkoaotView.top,\n'
         '          gekkoaotView.width, gekkoaotView.height);\n'
         '  const bool gekkoaotApplyWideX = !gekkoaotTextureProducer && (\n'),
        ('(g_gxState.projType == GX_ORTHOGRAPHIC && !gekkoaotOrthoSamplesEfbCopy);',
         '(g_gxState.projType == GX_ORTHOGRAPHIC && !gekkoaotOrthoSamplesEfbCopy));'),
    ]
    for before, after in edits:
        if text.count(before) != 1:
            raise RuntimeError(f'{path}: texture viewport anchor changed: {before}')
        text = text.replace(before, after)
    path.write_text(text)
