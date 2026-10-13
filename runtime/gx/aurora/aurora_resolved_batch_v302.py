#!/usr/bin/env python3
"""Fuse adjacent native GPU commands after resolving GX state (after v301).

Repeated SDK state writes can leave dirty flags even when the final native GPU
state is identical. Compare the resolved resources and uniform bytes instead.
"""
from pathlib import Path
import shutil
import sys
root = Path(sys.argv[1])
marker = 'GEKKOAOT_RESOLVED_BATCH_V302'
shutil.copyfile(Path(__file__).resolve().parents[1] / 'native_index_batch.h',
                root / 'lib/gfx/gekkoaot_native_index_batch.h')
def replace(s, old, new):
    if s.count(old) != 1:
        raise SystemExit('resolved native batch v302: missing/ambiguous anchor: ' + old[:100])
    return s.replace(old, new)
p = root / 'lib/gfx/recording.hpp'
s = p.read_text()
if marker not in s:
    s = replace(s, 'bool native_snapshot_atlas_enabled() noexcept;', '''bool native_snapshot_atlas_enabled() noexcept;
// GEKKOAOT_RESOLVED_BATCH_V302: recording-thread immutable uniform equality
// and ordered native index-span concatenation, within the current frame.
bool same_uniform_snapshot(Range a, Range b) noexcept;
bool append_native_index_span(Range previous, Range incoming, uint32_t base, uint32_t vertices) noexcept;''')
    p.write_text(s)

p = root / 'lib/gfx/recording.cpp'
s = p.read_text()
if marker not in s:
    s = replace(s, '#include "gekkoaot_native_snapshot_atlas.h"', '''#include "gekkoaot_native_snapshot_atlas.h"
#include "gekkoaot_native_index_batch.h"
// GEKKOAOT_RESOLVED_BATCH_V302''')
    s = replace(s, 'bool native_snapshot_atlas_enabled() noexcept {', '''bool same_uniform_snapshot(Range a, Range b) noexcept {
  if (!g_recorder.active() || !a.size || a.size != b.size) return false;
  const auto& bytes = current_frame_packet().uniforms;
  if (a.offset > bytes.size() || a.size > bytes.size() - a.offset ||
      b.offset > bytes.size() || b.size > bytes.size() - b.offset) return false;
  return a.offset == b.offset || std::memcmp(bytes.data() + a.offset, bytes.data() + b.offset, a.size) == 0;
}
bool append_native_index_span(Range previous, Range incoming, uint32_t base, uint32_t vertices) noexcept {
  if (!g_recorder.active() || !previous.size || !incoming.size ||
      uint64_t(previous.offset) + previous.size != incoming.offset) return false;
  auto& bytes = current_frame_packet().indices;
  if (incoming.offset > bytes.size() || incoming.size > bytes.size() - incoming.offset) return false;
  return GekkoAOT::GX::RebaseNativeIndexSpan(bytes.data() + incoming.offset, incoming.size, base, vertices);
}

bool native_snapshot_atlas_enabled() noexcept {''')
    p.write_text(s)
p = root / 'lib/gx/command_processor.cpp'
s = p.read_text()
if marker not in s:
    s = replace(s, 'static void push_gx_draw(GXPrimitive prim, GXVtxFmt fmt, u16 vtxCount, gfx::Range vertRange, gfx::Range idxRange,', '''// GEKKOAOT_RESOLVED_BATCH_V302: fuse only adjacent ordinary native draws.
// Scissor/viewport/copy/callback commands make get_last_draw_command() reject.
static bool resolved_native_batch_enabled() noexcept {
  static const bool enabled = [] {
    const char* value = std::getenv("GEKKOAOT_NATIVE_GX_RESOLVED_BATCH");
    return value && value[0] != '0';
  }();
  return enabled;
}
static bool append_resolved_native_draw(GXPrimitive prim, u16 vertices, gfx::Range vertexRange,
                                       gfx::Range indexRange, u32 indices,
                                       const DrawImmediateData& immediate) noexcept {
  if (!resolved_native_batch_enabled() || sDrawCache.lineMode != 0 || vertices < 3 ||
      (prim == GX_QUADS && vertices % 4u != 0) ||
      (prim == GX_TRIANGLES && vertices % 3u != 0)) return false;
  auto* previous = gfx::get_last_draw_command<DrawData>();
  if (!previous || previous->instanceCount != 1 || previous->logicDestination ||
      previous->vtxCount + uint32_t(vertices) > 65535u ||
      previous->pipeline != sDrawCache.pipelineRef ||
      previous->bindGroups.textureBindGroup != sDrawCache.bindGroups.textureBindGroup ||
      previous->dstAlpha != g_gxState.dstAlpha ||
      previous->immediateData.currentPnMtx != immediate.currentPnMtx ||
      previous->immediateData.fogRangeBase != immediate.fogRangeBase ||
      uint64_t(previous->immediateData.vtxStart) +
          uint64_t(previous->vtxCount) * sDrawCache.config.shaderConfig.vtxStride != vertexRange.offset)
    return false;
  for (int i = GX_VA_POS; i <= GX_VA_TEX7; ++i) {
    if (g_gxState.vtxDesc[i] != GX_INDEX8 && g_gxState.vtxDesc[i] != GX_INDEX16) continue;
    if (previous->immediateData.arrayStart[i - GX_VA_POS] != immediate.arrayStart[i - GX_VA_POS]) return false;
  }
  if (!gfx::same_uniform_snapshot(previous->uniformRange, sDrawCache.uniformRange)) return false;
  if (indices && previous->indexCount) {
    if (!gfx::append_native_index_span(previous->idxRange, indexRange, previous->vtxCount, vertices)) return false;
    previous->idxRange.size += indexRange.size;
    previous->indexCount += indices;
  } else if (indices || previous->indexCount || prim != GX_TRIANGLES || previous->vtxCount % 3u != 0) {
    return false;
  }
  previous->vtxCount += vertices;
  gfx::detail::increment_merged_draw_count();
  return true;
}

static void push_gx_draw(GXPrimitive prim, GXVtxFmt fmt, u16 vtxCount, gfx::Range vertRange, gfx::Range idxRange,''')
    s = replace(s, '''  const auto targetLayoutKey = gfx::get_render_target_layout().key;''', '''  const bool resourceVisibilityChanged = (state.dirty & DirtyTextures) != 0 ||
      cache.bindGeneration != texture::current_bind_generation();
  const auto targetLayoutKey = gfx::get_render_target_layout().key;''')
    s = replace(s, '''  if (!logic) {
    // GEKKOAOT_GX_HOTPATH_V300:''', '''  if (!logic) {
    if (!resourceVisibilityChanged &&
        append_resolved_native_draw(prim, vtxCount, vertRange, idxRange, numIndices, immediates)) return;
    // GEKKOAOT_GX_HOTPATH_V300:''')
    p.write_text(s)

# Comparisons/rebasing must NEVER read write-combined GPU mappings. Uniforms
# retain a cacheable CPU shadow; only the most recent native index upload needs
# a shadow because concatenation touches the incoming span, never old indices.
p = root / 'lib/gfx/recording.cpp'
s = p.read_text()
if 'GEKKOAOT_RESOLVED_CPU_SHADOW_V302' not in s:
    s = replace(s, 'FrameRecorder g_recorder;', '''FrameRecorder g_recorder;
// GEKKOAOT_RESOLVED_CPU_SHADOW_V302
std::vector<uint8_t> g_nativeUniformShadow, g_nativeIndexShadow;
uint64_t g_nativeUniformFrame = UINT64_MAX, g_nativeIndexFrame = UINT64_MAX;
Range g_nativeIndexRange{};
bool native_resolved_shadow_enabled() noexcept {
  static const bool enabled = [] {
    const char* value = std::getenv("GEKKOAOT_NATIVE_GX_RESOLVED_BATCH");
    return value && value[0] != '0';
  }();
  return enabled;
}''')
    s = replace(s, '''  return push(indices, data, length, alignment);''', '''  if (native_resolved_shadow_enabled()) {
    g_nativeIndexShadow.assign(data, data + length);
    g_nativeIndexRange = push(indices, g_nativeIndexShadow.data(), length, alignment);
    g_nativeIndexFrame = current_frame_packet().frameId;
    return g_nativeIndexRange;
  }
  return push(indices, data, length, alignment);''')
    s = replace(s, '''  return push(current_frame_packet().uniforms, data, length, resources().limits.minUniformBufferOffsetAlignment);''', '''  auto& frame = current_frame_packet();
  const auto alignment = resources().limits.minUniformBufferOffsetAlignment;
  if (!native_resolved_shadow_enabled()) return push(frame.uniforms, data, length, alignment);
  if (g_nativeUniformFrame != frame.frameId) {
    g_nativeUniformShadow.clear();
    g_nativeUniformFrame = frame.frameId;
  }
  const auto offset = alignment ? AURORA_ALIGN(frame.uniforms.size(), alignment) : frame.uniforms.size();
  if (offset > UniformBufferSize || length > UniformBufferSize - offset)
    return push(frame.uniforms, data, length, alignment);
  g_nativeUniformShadow.resize(offset + length);
  std::memcpy(g_nativeUniformShadow.data() + offset, data, length);
  return push(frame.uniforms, g_nativeUniformShadow.data() + offset, length, alignment);''')
    s = replace(s, '''  const auto& bytes = current_frame_packet().uniforms;''', '''  if (g_nativeUniformFrame != current_frame_packet().frameId) return false;
  const auto& bytes = g_nativeUniformShadow;''')
    s = replace(s, '''  return GekkoAOT::GX::RebaseNativeIndexSpan(bytes.data() + incoming.offset, incoming.size, base, vertices);''', '''  if (g_nativeIndexFrame != current_frame_packet().frameId ||
      g_nativeIndexRange.offset != incoming.offset || g_nativeIndexRange.size != incoming.size ||
      g_nativeIndexShadow.size() != incoming.size) return false;
  if (!GekkoAOT::GX::RebaseNativeIndexSpan(g_nativeIndexShadow.data(), incoming.size, base, vertices)) return false;
  std::memcpy(bytes.data() + incoming.offset, g_nativeIndexShadow.data(), incoming.size);
  return true;''')
    p.write_text(s)
