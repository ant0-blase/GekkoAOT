"""GX logic OR/AND as fixed-function blending (v369).

WebGPU has no logic ops: Aurora reads the destination in the shader and, to
let overlapping triangles see each other, snapshots the whole framebuffer and
splits the render pass before EVERY triangle of a logic draw. Star Fox
Adventures ORs thousands of shadow-mask triangles per frame (ThornTail
Hollow: ~8000 full-surface copies, 3000 render passes, 4 fps at 1920x1200).

On mask values -- 0, 255, or one value drawn over itself -- s|d is max(s,d)
and s&d is min(s,d) exactly, which the blender computes per fragment in
order with no destination read. Other bit patterns are approximated (as
backends without logic ops do); every other logic op keeps the exact shader
path.
"""
from pathlib import Path
import sys

root = Path(sys.argv[1])
MARKER = 'GEKKOAOT_LOGIC_BLEND_V369'


def patch(relative, edits):
    path = root / relative
    text = path.read_text()
    if MARKER in text:
        return
    for before, after in edits:
        if text.count(before) != 1:
            raise RuntimeError(f'{relative}: logic blend anchor changed: {before[:100]}')
        text = text.replace(before, after)
    path.write_text(text)


patch('lib/gx/gx.hpp', [
    ('''constexpr bool shader_logic_op(GXLogicOp op) noexcept {
  return op != GX_LO_CLEAR && op != GX_LO_COPY && op != GX_LO_NOOP;
}''',
     '''// ''' + MARKER + ''': OR and AND blend as max/min (exact on masks).
constexpr bool shader_logic_op(GXLogicOp op) noexcept {
  return op != GX_LO_CLEAR && op != GX_LO_COPY && op != GX_LO_NOOP && op != GX_LO_OR && op != GX_LO_AND;
}'''),
])

patch('lib/gx/gx.cpp', [
    ('''    case GX_LO_NOOP:
      colorBlendComponent = {
          .operation = wgpu::BlendOperation::Add,
          .srcFactor = wgpu::BlendFactor::Zero,
          .dstFactor = wgpu::BlendFactor::One,
      };
      break;
    }''',
     '''    case GX_LO_NOOP:
      colorBlendComponent = {
          .operation = wgpu::BlendOperation::Add,
          .srcFactor = wgpu::BlendFactor::Zero,
          .dstFactor = wgpu::BlendFactor::One,
      };
      break;
    // ''' + MARKER + ''': s|d = max(s,d) and s&d = min(s,d) on mask values.
    case GX_LO_OR:
      colorBlendComponent = {
          .operation = wgpu::BlendOperation::Max,
          .srcFactor = wgpu::BlendFactor::One,
          .dstFactor = wgpu::BlendFactor::One,
      };
      break;
    case GX_LO_AND:
      colorBlendComponent = {
          .operation = wgpu::BlendOperation::Min,
          .srcFactor = wgpu::BlendFactor::One,
          .dstFactor = wgpu::BlendFactor::One,
      };
      break;
    }'''),
])
