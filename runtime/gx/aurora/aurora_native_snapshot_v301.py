#!/usr/bin/env python3
"""Immutable native array snapshots and semantic draw batching for pinned Aurora.

Applied after v300. FIFO/custom display lists retain their existing decoding;
once decoded, native resource identity and GPU state determine batching.
"""
from pathlib import Path
import shutil
import sys

root = Path(sys.argv[1])
marker = 'GEKKOAOT_NATIVE_SNAPSHOT_V301'
shutil.copyfile(Path(__file__).resolve().parents[1] / 'native_snapshot_atlas.h',
                root / 'lib/gfx/gekkoaot_native_snapshot_atlas.h')

def replace(source, before, after):
    if source.count(before) != 1:
        raise SystemExit('native snapshot v301: missing/ambiguous pinned anchor: ' + before[:100])
    return source.replace(before, after)

p = root / 'lib/gfx/recording.hpp'
s = p.read_text()
if marker not in s:
    s = replace(s, 'Range push_storage(const uint8_t* data, size_t length);', '''Range push_storage(const uint8_t* data, size_t length);
// GEKKOAOT_NATIVE_SNAPSHOT_V301: immutable per-frame/segment native resources.
bool native_snapshot_atlas_enabled() noexcept;
Range push_storage_snapshot(const uint8_t* data, size_t length, bool fullArray = false);''')
    p.write_text(s)

p = root / 'lib/gfx/recording.cpp'
s = p.read_text()
if marker not in s:
    s = replace(s, '#include "recording.hpp"', '''#include "recording.hpp"
#include "gekkoaot_native_snapshot_atlas.h"
#include <cstdio>
#include <cstdlib>
// GEKKOAOT_NATIVE_SNAPSHOT_V301''')
    s = replace(s, '''  auto& last = current_render_passes()[g_recorder.currentRenderPass].commands.back();''', '''  auto& commands = current_render_passes()[g_recorder.currentRenderPass].commands;
  if (commands.empty()) return nullptr;
  auto& last = commands.back();''')
    s = replace(s, 'Range push_texture_data(const uint8_t* data, u32 bytesPerRow, u32 rowsPerImage) {', '''bool native_snapshot_atlas_enabled() noexcept {
  static const bool enabled = [] {
    const char* value = std::getenv("GEKKOAOT_NATIVE_GX_SNAPSHOT_ATLAS");
    return !value || value[0] != '0';
  }();
  return enabled;
}

Range push_storage_snapshot(const uint8_t* data, size_t length, bool fullArray) {
  if (!native_snapshot_atlas_enabled() || !g_recorder.active()) return push_storage(data, length);
  auto& frame = current_frame_packet();
  auto& storage = current_storage(frame);
  // Recording is serialized. Compare a cacheable CPU shadow, never mapped
  // GPU staging; both immutable snapshots contain the exact same bytes.
  static GekkoAOT::GX::NativeSnapshotAtlas compactAtlas, fullAtlas(1024u * 1024u);
  auto& atlas = fullArray ? fullAtlas : compactAtlas;
  static uint64_t hits = 0, misses = 0, reusedBytes = 0, lastReport = UINT64_MAX;
  atlas.Scope(frame.frameId, frame.storageSegment);
  if (auto offset = atlas.Find(data, length, storage.size())) {
    ++hits;
    reusedBytes += length;
    return {*offset, static_cast<uint32_t>(length)};
  }
  ++misses;
  const auto snapshot = atlas.Capture(data, length);
  const auto range = push_storage(snapshot ? snapshot.bytes : data, length);
  if (range.size == length && snapshot) atlas.Remember(data, length, range.offset, snapshot);
  static const bool profile = [] {
    const char* value = std::getenv("GEKKOAOT_RUNTIME_PROFILE");
    return value && value[0] != '0';
  }();
  if (profile && (lastReport == UINT64_MAX || frame.frameId - lastReport >= 120)) {
    lastReport = frame.frameId;
    std::fprintf(stderr, "GEKKOAOT_NATIVE_SNAPSHOT_V301 hits=%llu misses=%llu reused_bytes=%llu frame=%llu\\n",
                 static_cast<unsigned long long>(hits), static_cast<unsigned long long>(misses),
                 static_cast<unsigned long long>(reusedBytes), static_cast<unsigned long long>(frame.frameId));
  }
  return range;
}

Range push_texture_data(const uint8_t* data, u32 bytesPerRow, u32 rowsPerImage) {''')
    p.write_text(s)

# v312: expose bounded CPU-shadow reuse separately from GPU range reuse.
# Upgrade an already installed v301 overlay without replacing other patches.
p = root / 'lib/gfx/recording.cpp'
s = p.read_text()
if 'GEKKOAOT_NATIVE_SNAPSHOT_V312' not in s:
    anchor = '''                 static_cast<unsigned long long>(reusedBytes), static_cast<unsigned long long>(frame.frameId));'''
    s = replace(s, anchor, anchor + '''
    std::fprintf(stderr, "GEKKOAOT_NATIVE_SNAPSHOT_V312 recycled=%llu budget_declines=%llu shadow_bytes=%zu gpu_ranges=immutable\\n",
                 static_cast<unsigned long long>(compactAtlas.RecycledSnapshots() + fullAtlas.RecycledSnapshots()),
                 static_cast<unsigned long long>(compactAtlas.BudgetDeclines() + fullAtlas.BudgetDeclines()),
                 compactAtlas.ShadowBytes() + fullAtlas.ShadowBytes());''')
    p.write_text(s)

p = root / 'lib/gx/command_processor.cpp'
s = p.read_text()
if marker not in s:
    s = replace(s, '''static void draw_prim(GXPrimitive prim, GXVtxFmt fmt, u16 vtxCount, ByteReader& reader) noexcept {''', '''// GEKKOAOT_NATIVE_SNAPSHOT_V301: an indexed draw remains a visibility
// boundary. It can share a native draw only AFTER every used array has an
// immutable snapshot, with identical native bases and all other GPU state.
static bool native_semantic_batch_enabled() noexcept {
  static const bool enabled = [] {
    const char* value = std::getenv("GEKKOAOT_NATIVE_GX_SEMANTIC_BATCH");
    return !value || value[0] != '0';
  }();
  return enabled;
}
static bool same_native_immediates(const DrawData& previous) noexcept {
  if (previous.immediateData.currentPnMtx != g_gxState.currentPnMtx) return false;
  for (int i = GX_VA_POS; i <= GX_VA_TEX7; ++i) {
    if (g_gxState.vtxDesc[i] != GX_INDEX8 && g_gxState.vtxDesc[i] != GX_INDEX16) continue;
    const auto& range = g_gxState.arrays[i].cachedRange;
    if (!range.size || range.offset != previous.immediateData.arrayStart[i - GX_VA_POS]) return false;
  }
  return previous.immediateData.fogRangeBase == sDrawCache.fogRange.offset / sizeof(u32);
}

static void draw_prim(GXPrimitive prim, GXVtxFmt fmt, u16 vtxCount, ByteReader& reader) noexcept {''')
    s = replace(s, '''  const bool cleanState = g_gxState.dirty == 0 && fmt == sDrawCache.lastDrawFmt && sDrawCache.lineMode == 0 &&
                          prim != GX_LINES && prim != GX_LINESTRIP && prim != GX_POINTS;
  auto* lastDraw = cleanState ? gfx::get_last_draw_command<DrawData>() : nullptr;
  const bool canMerge = lastDraw != nullptr && lastDraw->instanceCount == 1 &&
                        !(g_gxState.blendMode == GX_BM_LOGIC && shader_logic_op(g_gxState.blendOp));''', '''  const bool nativeBatch = native_semantic_batch_enabled();
  const bool cleanState = (g_gxState.dirty == 0 || (nativeBatch && g_gxState.dirty == DirtyImmediates)) &&
                          fmt == sDrawCache.lastDrawFmt && sDrawCache.lineMode == 0 &&
                          prim != GX_LINES && prim != GX_LINESTRIP && prim != GX_POINTS;
  auto* lastDraw = cleanState ? gfx::get_last_draw_command<DrawData>() : nullptr;
  const bool canMerge = lastDraw != nullptr && lastDraw->instanceCount == 1 &&
                        lastDraw->vtxCount + uint32_t(vtxCount) <= 65535u &&
                        (prim != GX_TRIANGLES || vtxCount % 3u == 0) &&
                        (prim != GX_QUADS || vtxCount % 4u == 0) &&
                        (lastDraw->indexCount != 0 || lastDraw->vtxCount % 3u == 0) &&
                        (g_gxState.dirty == 0 || same_native_immediates(*lastDraw)) &&
                        !(g_gxState.blendMode == GX_BM_LOGIC && shader_logic_op(g_gxState.blendOp));''')
    s = replace(s, '''    gfx::detail::increment_merged_draw_count();
    return;''', '''    g_gxState.dirty &= ~DirtyImmediates;
    gfx::detail::increment_merged_draw_count();
    return;''')
    p.write_text(s)
