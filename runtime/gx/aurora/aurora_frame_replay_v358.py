"""Geometry replay for host frame-rate unlock (v358).

The FPS unlock re-renders the last guest frame at host refresh times with
extrapolated matrices instead of generating images. Three hooks make that
possible:

* build_uniform() reports every GX uniform block (its offset in the frame's
  uniform buffer, its bytes and where the projection starts) to the bridge,
  which keys it by the draw that built it.
* The render worker keeps the encoded ops of the last submitted frame. Until
  the next frame is submitted, the shared vertex/index/storage buffers still
  hold that frame's data, so only uniforms change between replays.
* gekkoaot_replay_submit() rewrites the patched uniform runs and re-encodes
  the retained passes and texture copies. CPU peek snapshots, texture uploads
  and encoder tasks are never replayed: the guest must not observe replays.
"""
from pathlib import Path
import sys

root = Path(sys.argv[1])
MARKER = 'GEKKOAOT_FRAME_REPLAY_V358'


def patch(relative, edits):
    path = root / relative
    text = path.read_text()
    if MARKER in text:
        return
    for before, after in edits:
        if text.count(before) != 1:
            raise RuntimeError(f'{relative}: frame replay anchor changed: {before[:100]}')
        text = text.replace(before, after)
    path.write_text(text)


patch('lib/gx/shader_info.cpp', [
    ('''extern "C" void gekkoaot_native_gx_widescreen_diag(int, float, float, float, float,
    int, int, int, int, bool, bool, float) noexcept;
''',
     '''extern "C" void gekkoaot_native_gx_widescreen_diag(int, float, float, float, float,
    int, int, int, int, bool, bool, float) noexcept;
// ''' + MARKER + '''
extern "C" bool gekkoaot_native_gx_replay_capture() noexcept;
extern "C" void gekkoaot_native_gx_replay_uniform(std::uint32_t, const std::uint8_t*, std::uint32_t,
                                                  std::uint32_t) noexcept;
static std::uint32_t g_gekkoaotReplayProjOffset = 0;
'''),
    ('''  buf.append(proj);

  for (const auto& row : g_gxState.xfPosRows) buf.append(row);''',
     '''  g_gekkoaotReplayProjOffset = static_cast<std::uint32_t>(buf.size());
  buf.append(proj);

  for (const auto& row : g_gxState.xfPosRows) buf.append(row);'''),
    ('''  fill_uniform(buf, info);
  return gfx::push_uniform(buf.data(), buf.size());''',
     '''  fill_uniform(buf, info);
  const auto range = gfx::push_uniform(buf.data(), buf.size());
  if (range.size != 0 && gekkoaot_native_gx_replay_capture())
    gekkoaot_native_gx_replay_uniform(range.offset, buf.data(), range.size, g_gekkoaotReplayProjOffset);
  return range;'''),
])

patch('lib/gx/command_processor.cpp', [
    ('#include "regs.hpp"\n',
     '#include "regs.hpp"\n'
     '// ' + MARKER + ': CPU-animated vertex arrays of frames captured for replay.\n'
     'extern "C" bool gekkoaot_native_gx_replay_capture() noexcept;\n'
     'extern "C" void gekkoaot_native_gx_replay_array(std::uint32_t, const std::uint8_t*, std::uint32_t, std::uint32_t,\n'
     '                                                std::uint32_t, std::uint32_t, std::uint32_t, bool) noexcept;\n'),
    ('    immediates.arrayStart[i - GX_VA_POS] = array.cachedRange.offset;\n  }\n',
     '    immediates.arrayStart[i - GX_VA_POS] = array.cachedRange.offset;\n  }\n'
     '  if (gekkoaot_native_gx_replay_capture()) {\n'
     '    for (const int attr : {int(GX_VA_POS), int(GX_VA_NRM)}) {\n'
     '      if (state.vtxDesc[attr] != GX_INDEX8 && state.vtxDesc[attr] != GX_INDEX16) continue;\n'
     '      const auto& array = state.arrays[attr];\n'
     '      const auto& format = cache.config.shaderConfig.attrs[attr];\n'
     '      if (array.data != nullptr && array.cachedRange.size != 0 && !format.nbt3)\n'
     '        gekkoaot_native_gx_replay_array(std::uint32_t(attr), static_cast<const std::uint8_t*>(array.data),\n'
     '                                        array.cachedRange.offset, array.cachedRange.size, format.stride,\n'
     '                                        format.compType, format.cnt, array.le);\n'
     '    }\n'
     '  }\n'),
])

patch('lib/gfx/recording.hpp', [
    ('void gekkoaot_capture_peek_snapshot() noexcept;',
     'void gekkoaot_capture_peek_snapshot() noexcept;\n'
     '// ' + MARKER + ': id of the frame being recorded (0 outside a recording).\n'
     'uint64_t gekkoaot_recording_frame_id() noexcept;'),
])

patch('lib/gfx/recording.cpp', [
    ('void gekkoaot_preserve_efb_on_next_begin(bool preserveColor, bool preserveDepth) noexcept {',
     '''// ''' + MARKER + '''
uint64_t gekkoaot_recording_frame_id() noexcept {
  return g_recorder.active() ? current_frame_packet().frameId : 0u;
}

void gekkoaot_preserve_efb_on_next_begin(bool preserveColor, bool preserveDepth) noexcept {'''),
])

patch('lib/gfx/frame.hpp', [
    ('''std::optional<RegisteredDrawType> find_runtime_draw_type(DrawTypeId id);''',
     '''std::optional<RegisteredDrawType> find_runtime_draw_type(DrawTypeId id);

// ''' + MARKER + ''': render-worker-only frame replay.
struct GekkoaotReplayWrite {
  uint32_t offset = 0;
  uint32_t size = 0;
  uint32_t source = 0;
};
void gekkoaot_replay_set_enabled(bool enabled) noexcept;
void gekkoaot_replay_retain(FramePacket& frame);
// Writes the uniform runs (offset-sorted) and re-encodes the retained frame.
bool gekkoaot_replay_submit(uint64_t frameId, const uint8_t* bytes, const GekkoaotReplayWrite* writes,
                            size_t count);
// Rewrites a vertex-array range of the retained frame's storage (blended
// vertices before a replay, the frame's own bytes after it).
bool gekkoaot_replay_write_storage(uint64_t frameId, uint32_t offset, const uint8_t* bytes, uint32_t size);'''),
])

patch('lib/gfx/frame_packet.hpp', [
    ('''  bool captureDepthSnapshot = false;''',
     '''  bool captureDepthSnapshot = false;
  // ''' + MARKER + ''': this pass was the displayed one when a replay retained it.
  bool gekkoaotReplayDisplayed = false;'''),
])

patch('lib/gfx/frame.cpp', [
    ('''    auto storageOverflow = std::move(packet.storageOverflow);''',
     '''    // ''' + MARKER + '''
    gekkoaot_replay_retain(packet);
    auto storageOverflow = std::move(packet.storageOverflow);'''),
])

patch('lib/gfx/encoding.cpp', [
    ('#include <algorithm>\n#include <array>\n',
     '#include <algorithm>\n#include <array>\n#include <atomic>\n#include <cstdio>\n'),
    ('''} // namespace detail

void bind_pipeline(const wgpu::RenderPipeline& pipeline, const wgpu::RenderPassEncoder& pass) {''',
     '''
// ''' + MARKER + '''
namespace {
struct GekkoaotReplayFrame {
  uint64_t frameId = 0;
  RenderPassList renderPasses;
  std::deque<TextureCopy> textureCopies;
  std::deque<FrameOp> ops;
};
GekkoaotReplayFrame g_gekkoaotReplay; // render worker only
std::atomic_bool g_gekkoaotReplayEnabled{false};
} // namespace

void gekkoaot_replay_set_enabled(bool enabled) noexcept {
  g_gekkoaotReplayEnabled.store(enabled, std::memory_order_release);
}

void gekkoaot_replay_retain(FramePacket& frame) {
  g_gekkoaotReplay = {};
  if (!g_gekkoaotReplayEnabled.load(std::memory_order_acquire)) {
    return;
  }
  // Replays rely on the shared buffers still holding this frame's data: a
  // storage overflow overwrote the start of the frame. Encoder tasks
  // (bounding-box writes) feed state the guest can read back.
  if (!frame.storageOverflow.empty() || frame.storageSegment != 0 || !frame.encoderTasks.empty()) {
    static unsigned refusals = 0;
    if (refusals++ < 8u || refusals % 600u == 0u) {
      std::fprintf(stderr, "GEKKOAOT_REPLAY_RETAIN_REFUSED_V393 overflow=%zu segment=%u tasks=%zu count=%u\\n",
                   size_t(frame.storageOverflow.size()), unsigned(frame.storageSegment),
                   size_t(frame.encoderTasks.size()), refusals);
    }
    return;
  }
  g_gekkoaotReplay.frameId = frame.frameId;
  // Moving the deques keeps element addresses, so the ops' pass/copy
  // pointers stay valid.
  g_gekkoaotReplay.renderPasses = std::move(frame.renderPasses);
  g_gekkoaotReplay.textureCopies = std::move(frame.textureCopies);
  g_gekkoaotReplay.ops = std::move(frame.ops);
  for (auto& pass : g_gekkoaotReplay.renderPasses) {
    pass.gekkoaotReplayDisplayed = pass.captureDepthSnapshot;
    pass.captureDepthSnapshot = false;
  }
  for (auto& op : g_gekkoaotReplay.ops) {
    op.textureUploads.clear();
    op.storageSource = {};
  }
}

bool gekkoaot_replay_write_storage(uint64_t frameId, uint32_t offset, const uint8_t* bytes, uint32_t size) {
  if (frameId == 0 || g_gekkoaotReplay.frameId != frameId || g_gekkoaotReplay.ops.empty() || bytes == nullptr ||
      size == 0 || (offset & 3u) != 0 || (size & 3u) != 0) {
    return false;
  }
  const auto& storage = resources().storageBuffer;
  if (!storage || uint64_t(offset) + size > storage.GetSize()) {
    return false;
  }
  g_queue.WriteBuffer(storage, offset, bytes, size);
  return true;
}

bool gekkoaot_replay_submit(uint64_t frameId, const uint8_t* bytes, const GekkoaotReplayWrite* writes,
                            size_t count) {
  if (frameId == 0 || g_gekkoaotReplay.frameId != frameId || g_gekkoaotReplay.ops.empty()) {
    return false;
  }
  ZoneScopedN("GekkoAOT frame replay");
  auto& res = resources();
  const uint32_t alignment = std::max<uint32_t>(res.limits.minUniformBufferOffsetAlignment, 4u);
  // Blocks separated only by their alignment padding go out as one write.
  std::vector<uint8_t> run;
  uint32_t runOffset = 0;
  const auto flush = [&] {
    if (!run.empty()) {
      g_queue.WriteBuffer(res.uniformBuffer, runOffset, run.data(), run.size());
      run.clear();
    }
  };
  for (size_t i = 0; i < count; ++i) {
    const auto& write = writes[i];
    if (write.size == 0 || (write.offset & 3u) != 0 || (write.size & 3u) != 0 ||
        uint64_t(write.offset) + write.size > UniformBufferSize) {
      continue;
    }
    const uint32_t runEnd = runOffset + static_cast<uint32_t>(run.size());
    if (run.empty() || write.offset < runEnd || write.offset != AURORA_ALIGN(runEnd, alignment)) {
      flush();
      runOffset = write.offset;
    } else {
      run.resize(write.offset - runOffset, 0);
    }
    run.insert(run.end(), bytes + write.source, bytes + write.source + write.size);
  }
  flush();

  shadow::set_frame_replay(true); // the real frame's cascade atlas is reused
  constexpr wgpu::CommandEncoderDescriptor descriptor{.label = "GekkoAOT frame replay"};
  auto cmd = g_device.CreateCommandEncoder(&descriptor);
  FramePacket scratch;
  for (const auto& op : g_gekkoaotReplay.ops) {
    if (op.type == FrameOpType::RenderPass && op.renderPass != nullptr) {
      render(cmd, scratch, *op.renderPass, op.index);
    } else if (op.type == FrameOpType::TextureCopy && op.textureCopy != nullptr) {
      cmd.CopyTextureToTexture(&op.textureCopy->src, &op.textureCopy->dst, &op.textureCopy->size);
    }
  }
  shadow::set_frame_replay(false);
  const auto buffer = cmd.Finish();
  g_queue.Submit(1, &buffer);
  return true;
}
} // namespace detail

void bind_pipeline(const wgpu::RenderPipeline& pipeline, const wgpu::RenderPassEncoder& pass) {'''),
])
