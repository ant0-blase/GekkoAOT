"""Scene-capture coverage rules that need Aurora's recorder and binding state.

* An inset perspective viewport (minimap, picture-in-picture) is recognised by
  containment in a larger camera already retained in the same render pass.
* The capture reason is classified once per draw (begin_draw at each entry).
* A color texture copy of a pass with scene receivers gets an enhanced twin;
  orthographic draws sampling that exact copy revision bind the twin.
* Blended fragments with alpha >= 0.98 replace the receiver.
Applied after the capture/contribution overlays; every anchor is preflighted.
Each file is edited once: edit() always starts from the file on disk.
"""
from pathlib import Path
import sys

root = Path(sys.argv[1])
marker = 'GEKKOAOT_CAPTURE_COVERAGE_V1'
updates = {}


def edit(name, changes):
    path = root / name
    source = path.read_text()
    if marker in source:
        return
    for old, new in changes:
        if source.count(old) != 1:
            raise RuntimeError(f'{name}: capture coverage anchor changed: {old[:100]} ({source.count(old)})')
        source = source.replace(old, new)
    updates[path] = '// ' + marker + '\n' + source


edit('lib/gfx/frame_packet.hpp', [
    ('std::array<uint32_t,12> enhancedReasons{};',
     'std::array<uint32_t,16> enhancedReasons{};'),
    ('  TextureHandle resolveTarget;\n',
     '  TextureHandle resolveTarget;\n  TextureHandle enhancedAlias;\n')])
edit('lib/gfx/recording.cpp', [(
    'unsigned enhanced::observe_scene(const GekkoAOT::GX::SceneCapture& capture) noexcept {',
    'bool enhanced::inset_viewport(const std::array<float,4>& viewport) noexcept {\n'
    '  if(!g_recorder.active()||g_recorder.inOffscreen||g_recorder.currentRenderPass==UINT32_MAX)return false;\n'
    '  const auto& scene=current_render_passes()[g_recorder.currentRenderPass].sceneCapture;\n'
    '  return GekkoAOT::GX::IsInsetViewport(scene,viewport);\n'
    '}\n'
    'unsigned enhanced::observe_scene(const GekkoAOT::GX::SceneCapture& capture) noexcept {')])
edit('lib/gx/command_processor.cpp', [
    ('  gfx::enhanced::observe_draw();\n  u32 vtxSize;',
     '  gfx::enhanced::begin_draw();\n  gfx::enhanced::observe_draw();\n  u32 vtxSize;'),
    ('    if (indexCount != 0) {\n      push_gx_draw(prim, fmt, vtxCount, vertRange, idxRange, indexCount);',
     '    if (indexCount != 0) {\n      gfx::enhanced::begin_draw();\n      push_gx_draw(prim, fmt, vtxCount, vertRange, idxRange, indexCount);'),
    ('  const bool bindGroupsValid =\n',
     '  if (gfx::enhanced::alias_bindings_stale()) texture::invalidate_bindings();\n  const bool bindGroupsValid =\n')])
edit('lib/gx/texture.cpp', [
    ('#include "texture.hpp"\n', '#include "texture.hpp"\n#include "../gfx/gekkoaot_enhanced_renderer.hpp"\n'),
    ('    } else if (copyRef != nullptr) {\n      handle = copyRef->handle;',
     '    } else if (copyRef != nullptr) {\n      handle = gfx::enhanced::copy_texture(copyRef->handle, copyRef->revision);')])
# A blended fragment covering at least 98% of the pixel is the receiver: the
# surface underneath contributes under 2%, and the contribution target still
# weights it exactly. Retail engines tag characters with alpha 254/255.
edit('lib/gx/shader.cpp', [
    ('let gekkoaot_opaque = prev.a >= 1.0;', 'let gekkoaot_opaque = prev.a >= 0.98;'),
    ('mode==1?"gekkoaot_alpha >= 1.0"', 'mode==1?"gekkoaot_alpha >= 0.98"')])
for path, source in updates.items():
    path.write_text(source)
