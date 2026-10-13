#!/usr/bin/env python3
"""GX recording hot path for the pinned Aurora (GekkoAOT v300).

Three per-draw costs measured on retail frames with 8k-13k draws per pass:
  * every GX draw built a DrawData on the stack and copied it four times
    (DrawData -> DrawCommand payload -> Command::Data -> vector slot); the
    first wide copy stalled on store forwarding.  Build it in its slot.
  * EFB passes reserved 2048 (or 0) commands and doubled from there, copying
    the whole command list ~14 times per frame.  Reserve the recent peak.
  * get_render_target_layout() re-hashed the attachment layout every draw.
    Its key covers only count/semantic/format/depth/samples: reuse it while
    those inputs are unchanged (sizes are still refreshed every call).
"""
from pathlib import Path
import sys

root = Path(sys.argv[1])
marker = 'GEKKOAOT_GX_HOTPATH_V300'


def replace(source, before, after, count=1):
    if source.count(before) != count:
        raise SystemExit('GX hot path v300: pinned Aurora anchor missing or ambiguous: ' + before[:80])
    return source.replace(before, after)


# --- recording.hpp: in-place GX draw entry point ---------------------------
p = root / 'lib/gfx/recording.hpp'
s = p.read_text()
if marker not in s:
    s = replace(s, '''template <typename DrawData>
void push_draw_command(DrawData data);''', '''template <typename DrawData>
void push_draw_command(DrawData data);
// GEKKOAOT_GX_HOTPATH_V300: append a GX draw command and return its payload
// storage for in-place construction of gx::DrawData (nullptr outside a pass).
void* emplace_gx_draw_payload() noexcept;''')
    p.write_text(s)

# --- frame_packet.hpp: default-constructible Command for in-place emplace ---
p = root / 'lib/gfx/frame_packet.hpp'
s = p.read_text()
if marker not in s:
    s = replace(s, '''  union Data {
    Viewport setViewport;''', '''  union Data {
    // GEKKOAOT_GX_HOTPATH_V300: a default member initializer gives the union
    // (and Command) a default constructor; it remains an aggregate.
    Viewport setViewport{};''')
    p.write_text(s)

# --- recording.cpp ----------------------------------------------------------
p = root / 'lib/gfx/recording.cpp'
s = p.read_text()
if marker not in s:
    s = replace(s, '''FrameRecorder g_recorder;
''', '''FrameRecorder g_recorder;

// GEKKOAOT_GX_HOTPATH_V300: command-list capacity follows the largest recent
// pass instead of doubling from 2048 (or 0) on every heavy GX frame.
size_t g_gekkoaotPassCommandHint = 2048;
void gekkoaot_reserve_pass_commands(RenderPass& pass) {
  pass.commands.reserve(g_gekkoaotPassCommandHint);
}
void gekkoaot_note_pass_commands(size_t count) {
  constexpr size_t kMaxHint = 32768;
  const size_t want = std::min(kMaxHint, count + count / 8 + 64);
  if (want > g_gekkoaotPassCommandHint) {
    g_gekkoaotPassCommandHint = want;
  } else {
    g_gekkoaotPassCommandHint =
        std::max<size_t>(2048, (g_gekkoaotPassCommandHint * 15 + want) / 16);
  }
}
''')
    s = replace(s, '''  auto& pass = frame.renderPasses[passIndex];
  if (pass.sealed) {
    return;
  }
  pass.sealed = true;''', '''  auto& pass = frame.renderPasses[passIndex];
  if (pass.sealed) {
    return;
  }
  pass.sealed = true;
  gekkoaot_note_pass_commands(pass.commands.size());''')
    s = replace(s, '''  current_render_passes().emplace_back();
  auto& pass = current_render_passes()[0];
  pass.label = pass_label("EFB");''', '''  current_render_passes().emplace_back();
  auto& pass = current_render_passes()[0];
  gekkoaot_reserve_pass_commands(pass);
  pass.label = pass_label("EFB");''')
    s = replace(s, '  newPass.commands.reserve(2048);\n',
                '  gekkoaot_reserve_pass_commands(newPass);\n', count=3)
    s = replace(s, '''RenderTargetLayout get_render_target_layout() noexcept {
  CHECK(g_recorder.currentRenderPass != UINT32_MAX, "get_render_target_layout called outside of a frame");
  return current_render_passes()[g_recorder.currentRenderPass].target_layout();
}''', '''RenderTargetLayout get_render_target_layout() noexcept {
  CHECK(g_recorder.currentRenderPass != UINT32_MAX, "get_render_target_layout called outside of a frame");
  // GEKKOAOT_GX_HOTPATH_V300: identical fields to RenderPass::target_layout().
  // finalize_render_target_layout() hashes count/semantic/format/depth/samples
  // only, so the previous key is reused while those inputs are unchanged.
  const auto& pass = current_render_passes()[g_recorder.currentRenderPass];
  RenderTargetLayout layout{
      .colorAttachmentCount = pass.colorAttachmentCount,
      .depthStencilFormat =
          pass.hasDepth || pass.hasStencil ? pass.depthStencilFormat : wgpu::TextureFormat::Undefined,
      .sampleCount = pass.msaaSamples,
  };
  for (uint32_t i = 0; i < pass.colorAttachmentCount; ++i) {
    layout.colorAttachments[i] = {
        .semantic = pass.colorAttachments[i].semantic,
        .format = pass.colorAttachments[i].format,
        .width = pass.colorAttachments[i].size.width,
        .height = pass.colorAttachments[i].size.height,
    };
  }
  static thread_local RenderTargetLayout cached{};
  static thread_local bool cachedValid = false;
  bool same = cachedValid && cached.colorAttachmentCount == layout.colorAttachmentCount &&
              cached.depthStencilFormat == layout.depthStencilFormat &&
              cached.sampleCount == layout.sampleCount;
  for (uint32_t i = 0; same && i < layout.colorAttachmentCount; ++i) {
    same = cached.colorAttachments[i].semantic == layout.colorAttachments[i].semantic &&
           cached.colorAttachments[i].format == layout.colorAttachments[i].format;
  }
  if (same) {
    layout.key = cached.key;
    return layout;
  }
  finalize_render_target_layout(layout);
  cached = layout;
  cachedValid = true;
  return layout;
}''')
    s = replace(s, '''template <>
void push_draw_command(gx::DrawData data) {
  push_draw_command(make_draw_command<gx::render>(data));
}''', '''template <>
void push_draw_command(gx::DrawData data) {
  push_draw_command(make_draw_command<gx::render>(data));
}

// GEKKOAOT_GX_HOTPATH_V300: same command as push_draw_command(gx::DrawData),
// built in its final vector slot (get_last_draw_command() and merging see the
// identical encoder/payload layout).
void* emplace_gx_draw_payload() noexcept {
  if (g_recorder.currentRenderPass == UINT32_MAX)
    UNLIKELY {
      Log.warn("Dropping command {}", magic_enum::enum_name(CommandType::Draw));
      return nullptr;
    }
  auto& renderPass = current_render_passes()[g_recorder.currentRenderPass];
  AURORA_ASSERT(!renderPass.sealed, "Attempted to append command {} to sealed render pass {}",
                magic_enum::enum_name(CommandType::Draw), g_recorder.currentRenderPass);
  renderPass.hasDraws = true;
  auto& command = renderPass.commands.emplace_back();
  command.type = CommandType::Draw;
#ifdef AURORA_GFX_DEBUG_GROUPS
  command.debugGroupStack = g_recorder.debugGroupStack;
#endif
  command.data.draw.encoder = encode_draw<gx::render, gx::DrawData>;
  ++g_recorder.drawCallCount;
  return command.data.draw.payload.data();
}''')
    p.write_text(s)

# --- pipeline.cpp: render-worker handle caches, one index binding per pass ---
p = root / 'lib/gx/pipeline.cpp'
s = p.read_text()
if marker not in s:
    s = replace(s, '''      group, reinterpret_cast<std::uintptr_t>(resource.Get()), offset);
}''', '''      group, reinterpret_cast<std::uintptr_t>(resource.Get()), offset);
}
// GEKKOAOT_GX_HOTPATH_V300: GX draws select their indices with firstIndex, so
// the whole index buffer is bound once per encoder (same reset rules).
bool bind_native_index_buffer(const wgpu::Buffer& buffer) noexcept {
  return !native_binding_cache_enabled() || g_nativeBindingSuspensions ||
         g_nativeBindings.NeedsIndexBuffer(reinterpret_cast<std::uintptr_t>(buffer.Get()));
}
// Render-worker-local handle caches: get_pipeline()/find_bind_group() take a
// mutex and copy (AddRef/Release) native handles on every draw. Slots own a
// reference, so a cached handle can never dangle; they are dropped
// periodically so retired resources are released.
struct GekkoAOTPipelineSlot {
  gfx::PipelineRef ref = 0;
  gfx::CompiledPipeline pipeline;
};
struct GekkoAOTBindGroupSlot {
  gfx::BindGroupRef id = 0;
  wgpu::BindGroup group;
};
thread_local std::array<GekkoAOTPipelineSlot, 64> t_gekkoaotPipelines;
thread_local std::array<GekkoAOTBindGroupSlot, 256> t_gekkoaotBindGroups;
thread_local uint32_t t_gekkoaotDrawsSinceTrim = 0;
void gekkoaot_trim_render_caches() noexcept {
  if (++t_gekkoaotDrawsSinceTrim < (1u << 20)) return;
  t_gekkoaotDrawsSinceTrim = 0;
  t_gekkoaotPipelines.fill({});
  t_gekkoaotBindGroups.fill({});
}
const gfx::CompiledPipeline* gekkoaot_cached_pipeline(gfx::PipelineRef ref) {
  auto& slot = t_gekkoaotPipelines[static_cast<size_t>(ref ^ (ref >> 29)) & 63u];
  if (slot.ref == ref && slot.pipeline.main) return &slot.pipeline;
  gfx::CompiledPipeline pipeline;
  if (!gfx::get_pipeline(ref, pipeline)) return nullptr;
  slot.ref = ref;
  slot.pipeline = std::move(pipeline);
  return &slot.pipeline;
}
const wgpu::BindGroup& gekkoaot_cached_bind_group(gfx::BindGroupRef id) {
  auto& slot = t_gekkoaotBindGroups[static_cast<size_t>(id ^ (id >> 31)) & 255u];
  if (slot.id != id || !slot.group) {
    slot.group = gfx::find_bind_group(id);
    slot.id = id;
  }
  return slot.group;
}''')
    s = replace(s, '''void render(const DrawData& data, const wgpu::RenderPassEncoder& pass) {
  gfx::CompiledPipeline pipeline;
  if (!gfx::get_pipeline(data.pipeline, pipeline)) {
    return;
  }''', '''void render(const DrawData& data, const wgpu::RenderPassEncoder& pass) {
  // GEKKOAOT_GX_HOTPATH_V300: cached handles, no per-draw lock/refcount.
  gekkoaot_trim_render_caches();
  const auto* cachedPipeline = gekkoaot_cached_pipeline(data.pipeline);
  if (!cachedPipeline) {
    return;
  }
  const auto& pipeline = *cachedPipeline;''')
    s = replace(s, '''  if (data.bindGroups.textureBindGroup) {
    const auto texture = gfx::find_bind_group(data.bindGroups.textureBindGroup);
    if (bind_native_group(2, texture)) pass.SetBindGroup(2, texture);
  }
  const auto logic = data.logicDestination ? gfx::find_bind_group(data.logicDestination)
                                          : g_emptyLogicBindGroup;
  if (bind_native_group(3, logic)) pass.SetBindGroup(3, logic);
  pass.SetIndexBuffer(resources.indexBuffer, wgpu::IndexFormat::Uint16, data.idxRange.offset, data.idxRange.size);''', '''  if (data.bindGroups.textureBindGroup) {
    const auto& texture = gekkoaot_cached_bind_group(data.bindGroups.textureBindGroup);
    if (bind_native_group(2, texture)) pass.SetBindGroup(2, texture);
  }
  const auto& logic = data.logicDestination ? gekkoaot_cached_bind_group(data.logicDestination)
                                           : g_emptyLogicBindGroup;
  if (bind_native_group(3, logic)) pass.SetBindGroup(3, logic);
  const bool indexed = data.indexCount != 0;
  if (indexed && bind_native_index_buffer(resources.indexBuffer))
    pass.SetIndexBuffer(resources.indexBuffer, wgpu::IndexFormat::Uint16, 0, wgpu::kWholeSize);''')
    s = replace(s, '''  const auto draw = [&] {
    if (data.indexCount == 0) {
      pass.Draw(data.vtxCount, data.instanceCount);
    } else {
      pass.DrawIndexed(data.indexCount, data.instanceCount);
    }
  };''', '''  const auto draw = [&] {
    if (!indexed) {
      pass.Draw(data.vtxCount, data.instanceCount);
    } else {
      pass.DrawIndexed(data.indexCount, data.instanceCount,
                       data.idxRange.offset / static_cast<uint32_t>(sizeof(uint16_t)));
    }
  };''')
    p.write_text(s)

# --- command_processor.cpp: construct GX draws in place ---------------------
p = root / 'lib/gx/command_processor.cpp'
s = p.read_text()
if marker not in s:
    fields = '''      .pipeline = cache.pipelineRef,
      .bindGroups = cache.bindGroups,
      .idxRange = idxRange,
      .uniformRange = cache.uniformRange,
      .immediateData = immediates,
      .vtxCount = vtxCount,
      .indexCount = numIndices,
      .instanceCount = instanceCount,
      .dstAlpha = state.dstAlpha,
'''
    s = replace(s, '''  cache.lastDrawFmt = fmt;
  DrawData draw{
''' + fields + '''  };
  const bool logic = state.blendMode == GX_BM_LOGIC && shader_logic_op(state.blendOp) &&
                     (state.colorUpdate || state.alphaUpdate);
  if (!logic) {
    gfx::push_draw_command(draw);
    return;
  }
''', '''  cache.lastDrawFmt = fmt;
  const bool logic = state.blendMode == GX_BM_LOGIC && shader_logic_op(state.blendOp) &&
                     (state.colorUpdate || state.alphaUpdate);
  if (!logic) {
    // GEKKOAOT_GX_HOTPATH_V300: construct the payload directly in its slot.
    // vtxStart/currentPnMtx are stored straight into the slot: copying the
    // whole local DrawImmediateData right after those two stores stalled on
    // store forwarding. arrayStart/fogRangeBase were written much earlier.
    if (void* slot = gfx::emplace_gx_draw_payload()) {
      auto* draw = ::new (slot) DrawData{
''' + fields.replace('      .', '          .').replace('          .immediateData = immediates,\n', '') + '''      };
      draw->immediateData.vtxStart = vertRange.offset;
      draw->immediateData.currentPnMtx = state.currentPnMtx;
      draw->immediateData.fogRangeBase = immediates.fogRangeBase;
      // Element-wise from the array state (same values as the loop above,
      // nothing in between touches cachedRange): a wide copy of the local
      // array still waited on its eight just-issued 4-byte stores.
      for (int i = GX_VA_POS; i <= GX_VA_TEX7; ++i) {
        if (state.vtxDesc[i] == GX_INDEX8 || state.vtxDesc[i] == GX_INDEX16)
          draw->immediateData.arrayStart[i - GX_VA_POS] = state.arrays[i].cachedRange.offset;
      }
    }
    return;
  }
  DrawData draw{
''' + fields + '''  };
''')
    s = replace(s, '''    immediates.arrayStart[i - GX_VA_POS] = array.cachedRange.offset;
  }
''', '''    immediates.arrayStart[i - GX_VA_POS] = array.cachedRange.offset;
  }
#if defined(__GNUC__) || defined(__clang__)
  // GEKKOAOT_GX_HOTPATH_V300: `immediates` never escapes, so the compiler sank
  // these stores next to the payload copy below (32-byte load over 4-byte
  // stores = store-forwarding stall on every draw). Materialize them here.
  __asm__ volatile("" : : "r"(&immediates) : "memory");
#endif
''')
    if '#include <new>' not in s:
        s = replace(s, '#include <span>\n', '#include <new>\n#include <span>\n')
    p.write_text(s)
