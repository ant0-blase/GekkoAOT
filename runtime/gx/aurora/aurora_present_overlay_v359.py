"""Draw the GekkoAOT performance HUD over Aurora's own presents (v359).

GekkoAOT presents through the bridge's post-processing pass or, when that is
off, through Aurora's VI scanout (present_texture) and the image blend
(present_texture_blend). The bridge draws the HUD in its own pass; this hook
gives the two Aurora paths the same overlay before their command buffers are
finished, so the HUD shows whatever path presents.
"""
from pathlib import Path
import sys

root = Path(sys.argv[1])
MARKER = 'GEKKOAOT_PRESENT_OVERLAY_V359'
path = root / 'lib/aurora.cpp'
text = path.read_text()
if MARKER not in text:
    edits = [
        ('namespace aurora {\n',
         '// ' + MARKER + ': implemented by the GekkoAOT bridge in the same shared object.\n'
         'void gekkoaot_native_gx_draw_overlay(const wgpu::CommandEncoder& encoder, const wgpu::TextureView& view,\n'
         '                                     std::uint32_t width, std::uint32_t height) noexcept;\n\n'
         'namespace aurora {\n'),
        ('''  pass.Draw(3);
  pass.End();

  constexpr wgpu::CommandBufferDescriptor cmdBufDescriptor{.label = "GekkoAOT VI scanout command buffer"};''',
         '''  pass.Draw(3);
  pass.End();
  ::gekkoaot_native_gx_draw_overlay(encoder, currentView, webgpu::g_graphicsConfig.surfaceConfiguration.width,
                                    webgpu::g_graphicsConfig.surfaceConfiguration.height);

  constexpr wgpu::CommandBufferDescriptor cmdBufDescriptor{.label = "GekkoAOT VI scanout command buffer"};'''),
        ('''  pass.Draw(3);
  pass.End();
  constexpr wgpu::CommandBufferDescriptor command_desc{.label = "GekkoAOT v48 interpolation command buffer"};''',
         '''  pass.Draw(3);
  pass.End();
  ::gekkoaot_native_gx_draw_overlay(encoder, swap_view, webgpu::g_graphicsConfig.surfaceConfiguration.width,
                                    webgpu::g_graphicsConfig.surfaceConfiguration.height);
  constexpr wgpu::CommandBufferDescriptor command_desc{.label = "GekkoAOT v48 interpolation command buffer"};'''),
    ]
    for before, after in edits:
        if text.count(before) != 1:
            raise RuntimeError(f'aurora.cpp: present overlay anchor changed: {before[:80]}')
        text = text.replace(before, after)
    path.write_text(text)
