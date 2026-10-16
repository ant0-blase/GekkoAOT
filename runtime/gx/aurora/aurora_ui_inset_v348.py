"""Move HUD inset viewports/scissors with the UI transform (v348).

A live 3D portrait, minimap or mirror is drawn into a small viewport (or a
full viewport clipped by a small scissor) inside the HUD. The HUD around it is
narrowed to its 4:3 proportions about the canvas centre, so the inset must be
moved and narrowed by the same transform, and its perspective must not be
widened a second time. The bridge decides which spans are confirmed insets.
"""
from pathlib import Path
import sys

root = Path(sys.argv[1])
path = root / 'lib/gx/shader_info.cpp'
text = path.read_text()
MARKER = 'GEKKOAOT_UI_INSET_V348'
if MARKER not in text:
    edits = [
        ('extern "C" float gekkoaot_native_gx_ui_clip_x(float, float, float, float) noexcept;',
         'extern "C" float gekkoaot_native_gx_ui_clip_x(float, float, float, float) noexcept;\n'
         '// ' + MARKER + '\n'
         'extern "C" bool gekkoaot_native_gx_ui_inset(float, float) noexcept;\n'
         'extern "C" bool gekkoaot_native_gx_ui_full_view(float, float) noexcept;\n'
         'extern "C" float gekkoaot_native_gx_ui_canvas_x(float) noexcept;'),
        ('''  const bool gekkoaotApplyWideX = !gekkoaotTextureProducer && (''',
         '''  // An inset viewport is already moved and narrowed by the UI transform.
  const bool gekkoaotInset = !gekkoaotOffscreen && gekkoaot_native_gx_widescreen_x_scale() != 1.f &&
      gekkoaot_native_gx_ui_inset(gekkoaotView.left, gekkoaotView.width);
  const bool gekkoaotApplyWideX = !gekkoaotTextureProducer && !gekkoaotInset && ('''),
        ('''  const float scale = gekkoaot_native_gx_widescreen_x_scale();
  if (scale == 1.f || g_gxState.projType != GX_ORTHOGRAPHIC ||''',
         '''  const float scale = gekkoaot_native_gx_widescreen_x_scale();
  const bool offscreen = gfx::is_offscreen();
  const bool insetView = scale != 1.f && !offscreen &&
      gekkoaot_native_gx_ui_inset(view.left, view.width);
  // The render viewport is rewritten only while an inset is drawn; the next
  // non-inset draw restores the plain mapping. gfx::set_viewport() itself
  // skips redundant commands, so a restarted pass is re-synchronized here.
  static bool insetApplied = false;
  if (insetView || insetApplied) {
    auto target = map_logical_viewport(view);
    if (insetView && view.width > 0.f) {
      const float sx = target.width / view.width;
      const float left = gekkoaot_native_gx_ui_canvas_x(view.left);
      const float right = gekkoaot_native_gx_ui_canvas_x(view.left + view.width);
      target.left = left * sx;
      target.width = (right - left) * sx;
    }
    insetApplied = insetView;
    set_render_viewport(target);
  }
  // A small scissor inside a full-width view frames HUD content too, even for
  // draws that keep the retail width (screen fills, EFB-copy samples, 3D).
  if (insetView || (scale != 1.f && !offscreen &&
                    gekkoaot_native_gx_ui_inset(float(clip.x), float(clip.width)) &&
                    gekkoaot_native_gx_ui_full_view(view.left, view.width) &&
                    !gekkoaot_native_gx_texture_viewport(view.left, view.top, view.width, view.height))) {
    const auto logicalSize = logical_fb_size();
    const auto targetSize = gfx::get_render_target_size();
    if (logicalSize.x && logicalSize.y && targetSize.x && targetSize.y) {
      const float sx = float(targetSize.x) / float(logicalSize.x);
      const float sy = float(targetSize.y) / float(logicalSize.y);
      const float left = gekkoaot_native_gx_ui_canvas_x(float(clip.x)) * sx;
      const float right = gekkoaot_native_gx_ui_canvas_x(float(clip.x) + float(clip.width)) * sx;
      const s32 x = std::clamp(s32(std::floor(left)), 0, s32(targetSize.x));
      const s32 y = std::clamp(s32(std::floor(float(clip.y) * sy)), 0, s32(targetSize.y));
      const s32 endX = std::clamp(s32(std::ceil(right)), x, s32(targetSize.x));
      const s32 endY = std::clamp(s32(std::ceil((float(clip.y) + float(clip.height)) * sy)), y, s32(targetSize.y));
      set_render_scissor({.x=x, .y=y, .width=endX-x, .height=endY-y});
      return;
    }
  }
  if (scale == 1.f || g_gxState.projType != GX_ORTHOGRAPHIC ||'''),
    ]
    for before, after in edits:
        if text.count(before) != 1:
            raise RuntimeError(f'shader_info.cpp: UI inset anchor changed: {before[:100]}')
        text = text.replace(before, after)
    path.write_text(text)
