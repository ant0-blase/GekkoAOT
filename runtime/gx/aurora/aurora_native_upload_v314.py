#!/usr/bin/env python3
"""Use one cacheable native upload arena instead of per-resource CPU shadows.

The mapped Dawn buffers remain the GPU transfer sources. Only the serialized
recorder accesses the native arena; the render worker never retains its data.
Flush each storage segment before rotation/end, retaining all old GPU ranges.
"""
from pathlib import Path
import shutil
import sys

root = Path(sys.argv[1])
shutil.copyfile(Path(__file__).resolve().parents[1] / 'native_resource_table.h',
                root / 'lib/gfx/gekkoaot_native_resource_table.h')
marker = 'GEKKOAOT_NATIVE_UPLOAD_V314'

def replace(s, before, after):
    if s.count(before) != 1:
        raise SystemExit('native upload v314: missing/ambiguous anchor: ' + before[:100])
    return s.replace(before, after)

p = root / 'lib/gfx/recording.cpp'
s = p.read_text()
if marker not in s:
    s = replace(s, '#include "gekkoaot_native_snapshot_atlas.h"', '''#include "gekkoaot_native_snapshot_atlas.h"
#include "gekkoaot_native_resource_table.h"
// GEKKOAOT_NATIVE_UPLOAD_V314''')
    s = replace(s, 'FrameRecorder g_recorder;', '''FrameRecorder g_recorder;
// Ordinary cacheable host memory, reused by the serialized recording thread.
// The GPU worker references only the mapped staging buffers, never this arena.
ByteBuffer g_nativeStorage;
GekkoAOT::GX::NativeResourceTable g_nativeCompactResources, g_nativeFullResources;
bool native_upload_enabled() noexcept {
  static const bool enabled = [] {
    const char* value = std::getenv("GEKKOAOT_NATIVE_GX_UPLOAD_ARENA");
    return value && value[0] != '0';
  }();
  return enabled;
}
uint64_t g_nativeUploadTransfers = 0, g_nativeUploadBytes = 0;
void flush_native_storage(FramePacket& frame) {
  if (!native_upload_enabled()) return;
  auto& target = frame.storageSegment == 0 ? frame.storage :
      frame.storageOverflow.at(frame.storageSegment - 1u).data;
  // Only committed bytes are transferred. Mapped buffer ownership, lifetime,
  // unmapping/submission and the encoder's high-water marks are unchanged.
  target.clear();
  if (!g_nativeStorage.empty()) {
    target.append(g_nativeStorage.data(), g_nativeStorage.size());
    ++g_nativeUploadTransfers;
    g_nativeUploadBytes += g_nativeStorage.size();
  }
}''')
    s = replace(s, 'ByteBuffer& current_storage(FramePacket& frame) {', '''ByteBuffer& current_storage(FramePacket& frame) {
  if (native_upload_enabled()) return g_nativeStorage;''')
    s = replace(s, 'const ByteBuffer& current_storage(const FramePacket& frame) {', '''const ByteBuffer& current_storage(const FramePacket& frame) {
  if (native_upload_enabled()) return g_nativeStorage;''')
    s = replace(s, '  g_recorder.frameSlot = frameSlot;\n  g_passSnapshotPools', '''  g_recorder.frameSlot = frameSlot;
  if (native_upload_enabled()) g_nativeStorage.clear();
  g_passSnapshotPools''')
    s = replace(s, '  size_t storageBytes = frame.storage.size();', '''  flush_native_storage(frame);
  size_t storageBytes = frame.storage.size();''')
    s = replace(s, '  g_recorder.currentRenderPass = UINT32_MAX;\n  g_offscreenCache.clear();', '''  g_recorder.currentRenderPass = UINT32_MAX;
  g_offscreenCache.clear();
  g_nativeStorage.release();
  g_nativeCompactResources = {};
  g_nativeFullResources = {};''')
    s = replace(s, '  frame.storageOverflow.emplace_back(std::move(segment));', '''  flush_native_storage(frame);
  if (native_upload_enabled()) g_nativeStorage.clear();
  frame.storageOverflow.emplace_back(std::move(segment));''')
    s = replace(s, '''  // Recording is serialized. Compare a cacheable CPU shadow, never mapped''', '''  if (native_upload_enabled()) {
    auto& resources = fullArray ? g_nativeFullResources : g_nativeCompactResources;
    resources.Scope(frame.frameId, frame.storageSegment);
    static uint64_t hits = 0, misses = 0, reusedBytes = 0, lastReport = UINT64_MAX;
    if (auto offset = resources.Find(data, length, {storage.data(), storage.size()})) {
      ++hits; reusedBytes += length;
      return {*offset, static_cast<uint32_t>(length)};
    }
    ++misses;
    const auto range = push_storage(data, length);
    if (range.size == length)
      resources.Remember(data, length, range.offset, {storage.data(), storage.size()});
    static const bool profile = [] {
      const char* value = std::getenv("GEKKOAOT_RUNTIME_PROFILE");
      return value && value[0] != '0';
    }();
    if (profile && (lastReport == UINT64_MAX || frame.frameId - lastReport >= 120)) {
      lastReport = frame.frameId;
      std::fprintf(stderr, "GEKKOAOT_NATIVE_UPLOAD_V314 hits=%llu misses=%llu reused_bytes=%llu transfers=%llu transferred_bytes=%llu arena_bytes=%zu shadow_bytes=0 frame=%llu\\n",
          static_cast<unsigned long long>(hits), static_cast<unsigned long long>(misses),
          static_cast<unsigned long long>(reusedBytes), static_cast<unsigned long long>(g_nativeUploadTransfers),
          static_cast<unsigned long long>(g_nativeUploadBytes), storage.size(),
          static_cast<unsigned long long>(frame.frameId));
    }
    return range;
  }
  // Recording is serialized. Compare a cacheable CPU shadow, never mapped''')
    p.write_text(s)
