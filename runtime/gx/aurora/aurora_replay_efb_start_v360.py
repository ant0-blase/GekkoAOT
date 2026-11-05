"""Replay frames that start from the previous frame's EFB (v360).

A GXCopyDisp without clear (Medal of Honor: Rising Sun clears by drawing)
makes the next recording LOAD the EFB colour and depth. Replaying such a frame
from whatever the EFB holds afterwards would depth-test against its own output.
When the replay is enabled, the first pass of a loading frame first copies the
EFB aside inside that frame's own command buffer; every replay copies it back
before re-encoding. The depth buffer gains copy usage for this (Depth32Float
supports texture copies).
"""
from pathlib import Path
import sys

root = Path(sys.argv[1])
MARKER = 'GEKKOAOT_REPLAY_EFB_START_V360'


def patch(relative, edits):
    path = root / relative
    text = path.read_text()
    if MARKER in text:
        return
    for before, after in edits:
        if text.count(before) != 1:
            raise RuntimeError(f'{relative}: replay EFB start anchor changed: {before[:100]}')
        text = text.replace(before, after)
    path.write_text(text)


patch('lib/webgpu/gpu.cpp', [
    ('''      .label = "Depth texture",
      .usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::TextureBinding,''',
     '''      .label = "Depth texture",
      // ''' + MARKER + ''': frame replays restore a loaded depth buffer.
      .usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::TextureBinding |
               wgpu::TextureUsage::CopySrc | wgpu::TextureUsage::CopyDst,'''),
])

patch('lib/gfx/encoding.cpp', [
    ('''std::atomic_bool g_gekkoaotReplayEnabled{false};
} // namespace
''',
     '''std::atomic_bool g_gekkoaotReplayEnabled{false};
} // namespace

// ''' + MARKER + ''': EFB state a loading frame started from (render worker only).
namespace {
struct GekkoaotReplayStart {
  uint64_t frameId = 0;
  bool color = false, depth = false;
  wgpu::Texture colorTexture, depthTexture;
};
GekkoaotReplayStart g_gekkoaotReplayStart;
bool g_gekkoaotReplayRestoreColor = false, g_gekkoaotReplayRestoreDepth = false;

bool gekkoaot_pass_loads_color(const RenderPass& pass) {
  const auto& color = pass.colorAttachments[SceneColorAttachmentIndex];
  return pass.colorAttachmentCount != 0 &&
         (color.loadOp == wgpu::LoadOp::Load || (color.loadOp == wgpu::LoadOp::Undefined && !color.clear));
}
bool gekkoaot_pass_loads_depth(const RenderPass& pass) {
  return pass.depthStencilView && pass.hasDepth &&
         (pass.depthLoadOp == wgpu::LoadOp::Load ||
          (pass.depthLoadOp == wgpu::LoadOp::Undefined && !pass.clearDepth));
}
wgpu::Texture gekkoaot_same_texture(wgpu::Texture& cached, const wgpu::Texture& like, const char* label) {
  if (!cached || cached.GetWidth() != like.GetWidth() || cached.GetHeight() != like.GetHeight() ||
      cached.GetFormat() != like.GetFormat() || cached.GetSampleCount() != like.GetSampleCount()) {
    const wgpu::TextureDescriptor descriptor{
        .label = label,
        // Multisampled textures must be render attachments (EFB with MSAA).
        .usage = wgpu::TextureUsage::CopySrc | wgpu::TextureUsage::CopyDst | wgpu::TextureUsage::RenderAttachment,
        .size = {like.GetWidth(), like.GetHeight(), 1},
        .format = like.GetFormat(),
        .sampleCount = like.GetSampleCount(),
    };
    cached = g_device.CreateTexture(&descriptor);
  }
  return cached;
}
void gekkoaot_copy_whole(const wgpu::CommandEncoder& cmd, const wgpu::Texture& src, const wgpu::Texture& dst) {
  const wgpu::TexelCopyTextureInfo from{.texture = src};
  const wgpu::TexelCopyTextureInfo to{.texture = dst};
  const wgpu::Extent3D size{src.GetWidth(), src.GetHeight(), 1};
  cmd.CopyTextureToTexture(&from, &to, &size);
}
} // namespace

// The first pass of a frame that loads the previous EFB: keep that EFB.
void gekkoaot_replay_snapshot_start(const wgpu::CommandEncoder& cmd, const FramePacket& frame, const FrameOp& op) {
  if (op.type != FrameOpType::RenderPass || op.renderPass == nullptr || op.index != 0 ||
      !g_gekkoaotReplayEnabled.load(std::memory_order_acquire)) {
    return;
  }
  const auto& pass = *op.renderPass;
  const bool color = gekkoaot_pass_loads_color(pass), depth = gekkoaot_pass_loads_depth(pass);
  g_gekkoaotReplayStart.frameId = 0;
  if (!color && !depth) {
    return;
  }
  const auto& colorSource = webgpu::g_frameBuffer.texture;
  const auto& depthSource = webgpu::g_depthBuffer.texture;
  const auto size = pass.colorAttachments[SceneColorAttachmentIndex].size;
  if ((color && (!colorSource || colorSource.GetWidth() != size.width || colorSource.GetHeight() != size.height)) ||
      (depth && (!depthSource || depthSource.GetWidth() != size.width || depthSource.GetHeight() != size.height)) ||
      webgpu::g_graphicsConfig.normalBuffer) {
    return;
  }
  if (color) gekkoaot_copy_whole(cmd, colorSource, gekkoaot_same_texture(g_gekkoaotReplayStart.colorTexture, colorSource, "GekkoAOT replay start color"));
  if (depth) gekkoaot_copy_whole(cmd, depthSource, gekkoaot_same_texture(g_gekkoaotReplayStart.depthTexture, depthSource, "GekkoAOT replay start depth"));
  g_gekkoaotReplayStart.frameId = frame.frameId;
  g_gekkoaotReplayStart.color = color;
  g_gekkoaotReplayStart.depth = depth;
}'''),
    # snapshot hook in encode_op
    ('''void encode_op(wgpu::CommandEncoder& cmd, FramePacket& frame, const FrameOp& op) {
  copy_staging_to_high_water(cmd, frame, op);''',
     '''void gekkoaot_replay_snapshot_start(const wgpu::CommandEncoder& cmd, const FramePacket& frame, const FrameOp& op);

void encode_op(wgpu::CommandEncoder& cmd, FramePacket& frame, const FrameOp& op) {
  copy_staging_to_high_water(cmd, frame, op);
  // ''' + MARKER + '''
  gekkoaot_replay_snapshot_start(cmd, frame, op);'''),
    # retain: loading frames need their start snapshot
    ('''  g_gekkoaotReplay.frameId = frame.frameId;
  // Moving the deques keeps element addresses''',
     '''  // ''' + MARKER + ''': a frame starting from the previous EFB replays only
  // with the snapshot its first pass took.
  g_gekkoaotReplayRestoreColor = g_gekkoaotReplayRestoreDepth = false;
  if (!frame.renderPasses.empty()) {
    const auto& first = frame.renderPasses.front();
    const bool color = gekkoaot_pass_loads_color(first), depth = gekkoaot_pass_loads_depth(first);
    if (color || depth) {
      if (g_gekkoaotReplayStart.frameId != frame.frameId || (color && !g_gekkoaotReplayStart.color) ||
          (depth && !g_gekkoaotReplayStart.depth)) {
        return;
      }
      g_gekkoaotReplayRestoreColor = color;
      g_gekkoaotReplayRestoreDepth = depth;
    }
  }
  g_gekkoaotReplay.frameId = frame.frameId;
  // Moving the deques keeps element addresses'''),
    # submit: restore before re-encoding
    ('''  FramePacket scratch;
  for (const auto& op : g_gekkoaotReplay.ops) {''',
     '''  // ''' + MARKER + '''
  if (g_gekkoaotReplayRestoreColor) {
    if (!webgpu::g_frameBuffer.texture || !g_gekkoaotReplayStart.colorTexture ||
        webgpu::g_frameBuffer.texture.GetWidth() != g_gekkoaotReplayStart.colorTexture.GetWidth() ||
        webgpu::g_frameBuffer.texture.GetHeight() != g_gekkoaotReplayStart.colorTexture.GetHeight()) {
      return false;
    }
    gekkoaot_copy_whole(cmd, g_gekkoaotReplayStart.colorTexture, webgpu::g_frameBuffer.texture);
  }
  if (g_gekkoaotReplayRestoreDepth) {
    if (!webgpu::g_depthBuffer.texture || !g_gekkoaotReplayStart.depthTexture ||
        webgpu::g_depthBuffer.texture.GetWidth() != g_gekkoaotReplayStart.depthTexture.GetWidth() ||
        webgpu::g_depthBuffer.texture.GetHeight() != g_gekkoaotReplayStart.depthTexture.GetHeight()) {
      return false;
    }
    gekkoaot_copy_whole(cmd, g_gekkoaotReplayStart.depthTexture, webgpu::g_depthBuffer.texture);
  }
  FramePacket scratch;
  for (const auto& op : g_gekkoaotReplay.ops) {'''),
])
