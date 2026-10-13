"""Apply the same aspect transform to UI geometry and its raster clip."""
from pathlib import Path
import sys
root = Path(sys.argv[1])


def patch(relative, edits, marker):
    path = root / relative
    text = path.read_text()
    if marker in text:
        return
    for before, after in edits:
        if text.count(before) != 1:
            raise RuntimeError(f'{relative}: UI scissor anchor changed: {before[:100]}')
        text = text.replace(before, after)
    path.write_text(text)


path = root / 'lib/gx/shader_info.cpp'
text = path.read_text()
if 'GEKKOAOT_UI_SCISSOR_V257' not in text:
    start = text.index('  bool gekkoaotOrthoSamplesEfbCopy = false;')
    end = text.index('  const bool gekkoaotTextureProducer =', start)
    text = text[:start] + '  const bool gekkoaotOrthoSamplesEfbCopy = widescreen_samples_screen_copy(info);\n\n' + text[end:]
    path.write_text(text)

patch('lib/gx/shader_info.cpp', [
    ('extern "C" bool gekkoaot_native_gx_screen_fill() noexcept;',
     'extern "C" bool gekkoaot_native_gx_screen_fill() noexcept;\n'
     'extern "C" float gekkoaot_native_gx_ui_clip_x(float, float, float, float) noexcept;'),
    ('gfx::Range build_uniform(const ShaderInfo& info) noexcept {', '''// GEKKOAOT_UI_SCISSOR_V257
bool widescreen_samples_screen_copy(const ShaderInfo& info) noexcept {
  if (g_gxState.projType != GX_ORTHOGRAPHIC) return false;
  const auto& view = g_gxState.logicalViewport;
  for (u32 i = 0; i < MaxTextures; ++i) {
    if (!info.sampledTextures.test(i)) continue;
    const auto& obj = g_gxState.loadedTextures[i];
    const auto copy = g_gxState.copyTextures.find(obj.data);
    if (obj.data && copy != g_gxState.copyTextures.end() &&
        gekkoaot_native_gx_screen_copy(copy->second.guestSource.width,
            copy->second.guestSource.height, view.width, view.height)) return true;
  }
  return false;
}

// Keep guest registers untouched: subsequent perspective/copy passes recover
// their original clip, even when the game does not reissue GXSetScissor.
// This runs every draw, independently of whether uniforms need rebuilding.
void update_widescreen_scissor(const ShaderInfo& info) noexcept {
  const auto& view = g_gxState.logicalViewport;
  const auto& clip = g_gxState.logicalScissor;
  const float scale = gekkoaot_native_gx_widescreen_x_scale();
  if (scale == 1.f || g_gxState.projType != GX_ORTHOGRAPHIC ||
      gfx::is_offscreen() || gekkoaot_native_gx_screen_fill() ||
      gekkoaot_native_gx_texture_viewport(view.left, view.top, view.width, view.height) ||
      widescreen_samples_screen_copy(info)) {
    set_render_scissor(map_logical_scissor(clip));
    return;
  }
  const auto logicalSize = logical_fb_size();
  const auto targetSize = gfx::get_render_target_size();
  if (!logicalSize.x || !logicalSize.y || !targetSize.x || !targetSize.y) {
    set_render_scissor(map_logical_scissor(clip));
    return;
  }
  const float sx = float(targetSize.x) / float(logicalSize.x);
  const float sy = float(targetSize.y) / float(logicalSize.y);
  const float left = gekkoaot_native_gx_ui_clip_x(float(clip.x), view.left, view.width, scale) * sx;
  const float right = gekkoaot_native_gx_ui_clip_x(float(clip.x) + float(clip.width), view.left, view.width, scale) * sx;
  const s32 x = std::clamp(s32(std::floor(left)), 0, s32(targetSize.x));
  const s32 y = std::clamp(s32(std::floor(float(clip.y) * sy)), 0, s32(targetSize.y));
  const s32 endX = std::clamp(s32(std::ceil(right)), x, s32(targetSize.x));
  const s32 endY = std::clamp(s32(std::ceil((float(clip.y) + float(clip.height)) * sy)), y, s32(targetSize.y));
  set_render_scissor({.x=x, .y=y, .width=endX-x, .height=endY-y});
}

gfx::Range build_uniform(const ShaderInfo& info) noexcept {'''),
], 'GEKKOAOT_UI_SCISSOR_V257')

patch('lib/gx/shader_info.hpp', [
    ('gfx::Range build_uniform(const ShaderInfo& info) noexcept;',
     '// GEKKOAOT_UI_SCISSOR_V257\nbool widescreen_samples_screen_copy(const ShaderInfo& info) noexcept;\n'
     'void update_widescreen_scissor(const ShaderInfo& info) noexcept;\n'
     'gfx::Range build_uniform(const ShaderInfo& info) noexcept;')
], 'GEKKOAOT_UI_SCISSOR_V257')

patch('lib/gx/command_processor.cpp', [
    ('  const bool uniformValid = (state.dirty & DirtyUniform) == 0 && cache.uniformRange.size != 0;',
     '  // GEKKOAOT_UI_SCISSOR_V257\n  update_widescreen_scissor(cache.shaderInfo);\n\n'
     '  const bool uniformValid = (state.dirty & DirtyUniform) == 0 && cache.uniformRange.size != 0;')
], 'GEKKOAOT_UI_SCISSOR_V257')
