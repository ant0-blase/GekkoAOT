"""Install BP59 offset decoding in the pinned Aurora source, idempotently."""
from pathlib import Path
import sys
root=Path(sys.argv[1])

def patch(relative, edits, marker):
    path=root/relative
    text=path.read_text()
    if marker in text:return
    for before,after in edits:
        if text.count(before)!=1:raise RuntimeError(f'{relative}: expected unique BP59 anchor: {before[:70]}')
        text=text.replace(before,after)
    path.write_text(text)

patch('lib/gx/gx.hpp',[("  u32 currentPnMtx;", "  u32 currentPnMtx;\n  // GEKKOAOT_SCISSOR_OFFSET_V247\n  s32 scissorOffsetX = 342;\n  s32 scissorOffsetY = 342;")],'GEKKOAOT_SCISSOR_OFFSET_V247')
patch('lib/gx/regs.cpp',[
('static_cast<s32>(reg_get(scis0, 11, 0)) - 342','static_cast<s32>(reg_get(scis0, 11, 0)) - g_gxState.scissorOffsetY'),
('static_cast<s32>(reg_get(scis0, 11, 12)) - 342','static_cast<s32>(reg_get(scis0, 11, 12)) - g_gxState.scissorOffsetX'),
('static_cast<s32>(reg_get(scis1, 11, 0)) - 342','static_cast<s32>(reg_get(scis1, 11, 0)) - g_gxState.scissorOffsetY'),
('static_cast<s32>(reg_get(scis1, 11, 12)) - 342','static_cast<s32>(reg_get(scis1, 11, 12)) - g_gxState.scissorOffsetX'),
('// Line/point size (0x22)', '''// GEKKOAOT_SCISSOR_OFFSET_V247: BP59 offsets are measured in pixel pairs.
// Apply to both raster coordinates and clipping; EFB copy rectangles already
// use EFB coordinates and must keep their original origin.
void bp_scissor_offset(u8, u32 value) noexcept {
  const s32 x = static_cast<s32>(reg_get(value, 10, 0)) * 2;
  const s32 y = static_cast<s32>(reg_get(value, 10, 10)) * 2;
  auto view = g_gxState.logicalViewport;
  view.left += static_cast<float>(g_gxState.scissorOffsetX - x);
  view.top += static_cast<float>(g_gxState.scissorOffsetY - y);
  g_gxState.scissorOffsetX = x;
  g_gxState.scissorOffsetY = y;
  set_logical_viewport(view);
  bp_scissor(0, 0);
}

// Line/point size (0x22)'''),
('  regs[0x21] = {bp_scissor};','  regs[0x21] = {bp_scissor};\n  regs[0x59] = {bp_scissor_offset};'),
('ox - 342.0f - width / 2.0f','ox - static_cast<float>(g_gxState.scissorOffsetX) - width / 2.0f'),
('oy - 342.0f - height / 2.0f','oy - static_cast<float>(g_gxState.scissorOffsetY) - height / 2.0f')
],'GEKKOAOT_SCISSOR_OFFSET_V247')
