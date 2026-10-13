#!/usr/bin/env python3
from pathlib import Path
import sys

if len(sys.argv) != 2:
    raise SystemExit("usage: aurora_storage_pool_v161g.py <aurora-source>")
root = Path(sys.argv[1])
marker = "GEKKOAOT_STORAGE_POOL_V161G"

def load(rel):
    p = root / rel
    if not p.exists():
        raise SystemExit(f"missing Aurora source: {p}")
    return p, p.read_text(encoding="utf-8")

def replace_once(text, old, new, label):
    if new in text or marker in text and old not in text:
        return text, False
    n = text.count(old)
    if n != 1:
        raise SystemExit(f"{label}: expected one anchor, found {n}")
    return text.replace(old, new, 1), True

changed = False

# Pool identity travels with each per-frame overflow lease.
p, s = load("lib/gfx/frame_packet.hpp")
if marker not in s:
    old = '''struct StorageStagingSegment {
  wgpu::Buffer buffer;
  ByteBuffer data;
  bool mapped = false;
};'''
    new = '''struct StorageStagingSegment {
  wgpu::Buffer buffer;
  ByteBuffer data;
  bool mapped = false;
  // GEKKOAOT_STORAGE_POOL_V161G: UINT32_MAX means a non-pooled/legacy segment.
  uint32_t poolIndex = UINT32_MAX;
};'''
    s, c = replace_once(s, old, new, "frame_packet storage segment")
    if c:
        p.write_text(s, encoding="utf-8"); changed = True

# Expose pool acquisition/recycle to the recorder and end-frame submission path.
p, s = load("lib/gfx/frame.hpp")
if marker not in s:
    old = '''const wgpu::Buffer& staging_buffer(size_t slot);
'''
    new = '''const wgpu::Buffer& staging_buffer(size_t slot);
// GEKKOAOT_STORAGE_POOL_V161G: persistent overflow staging pool. Buffers are
// unmapped for submission and asynchronously remapped before reuse.
StorageStagingSegment acquire_storage_overflow_segment();
void recycle_storage_overflow_segment(uint32_t poolIndex);
'''
    s, c = replace_once(s, old, new, "frame pool declarations")
    if c:
        p.write_text(s, encoding="utf-8"); changed = True

p, s = load("lib/gfx/frame.cpp")
if marker not in s:
    # cstdlib for getenv/strtoul.
    if "#include <cstdlib>\n" not in s:
        anchor = "#include <cstdint>\n"
        if anchor not in s:
            raise SystemExit("frame.cpp: cstdint include anchor missing")
        s = s.replace(anchor, anchor + "#include <cstdlib>\n", 1)

    old_globals = '''std::array<FramePacket, FrameSlotCount> g_framePackets;
uint64_t g_nextFrameId = 1;
render_worker::FrameSlotPool g_frameSlots{FrameSlotCount};
render_worker::FrameSlotPool g_stagingSlots{StagingBufferCount};
'''
    new_globals = '''std::array<FramePacket, FrameSlotCount> g_framePackets;
uint64_t g_nextFrameId = 1;
render_worker::FrameSlotPool g_frameSlots{FrameSlotCount};
render_worker::FrameSlotPool g_stagingSlots{StagingBufferCount};

// GEKKOAOT_STORAGE_POOL_V161G
// storage-staging-v4 used CreateBuffer(mappedAtCreation=true) for every
// overflow segment. Draw-heavy games can overflow the 64 MiB segment several
// times per frame, making Dawn buffer creation dominate the CPU. Retain those
// CopySrc buffers and remap them asynchronously after submission instead.
struct StorageOverflowPoolEntry {
  wgpu::Buffer buffer;
  std::atomic<BufferMapState> mapState{BufferMapState::Unmapped};
  bool inUse = false; // protected by g_storageOverflowPoolMutex
};
std::deque<StorageOverflowPoolEntry> g_storageOverflowPool;
std::mutex g_storageOverflowPoolMutex;

size_t storage_overflow_pool_limit() {
  static const size_t limit = [] {
    constexpr size_t kDefault = 16;
    constexpr size_t kMax = 64;
    const char* env = std::getenv("GEKKOAOT_AURORA_STORAGE_POOL_SEGMENTS");
    if (env == nullptr || *env == '\\0') {
      return kDefault;
    }
    char* end = nullptr;
    const unsigned long value = std::strtoul(env, &end, 10);
    if (end == env || *end != '\\0' || value == 0) {
      return kDefault;
    }
    return static_cast<size_t>(value > kMax ? kMax : value);
  }();
  return limit;
}
'''
    s, c = replace_once(s, old_globals, new_globals, "frame pool globals")
    changed |= c

    old_detail = '''const wgpu::Buffer& staging_buffer(size_t slot) { return g_stagingBuffers[slot]; }
'''
    new_detail = '''const wgpu::Buffer& staging_buffer(size_t slot) { return g_stagingBuffers[slot]; }

StorageStagingSegment acquire_storage_overflow_segment() {
  ZoneScopedN("Acquire storage overflow segment");
  for (;;) {
    {
      std::lock_guard lock{g_storageOverflowPoolMutex};
      for (size_t i = 0; i < g_storageOverflowPool.size(); ++i) {
        auto& entry = g_storageOverflowPool[i];
        if (entry.inUse || entry.mapState.load(std::memory_order_acquire) != BufferMapState::Mapped) {
          continue;
        }
        auto* mapped = static_cast<uint8_t*>(entry.buffer.GetMappedRange(0, StorageBufferSize));
        if (mapped == nullptr) {
          entry.mapState.store(BufferMapState::Unmapped, std::memory_order_release);
          continue;
        }
        entry.inUse = true;
        StorageStagingSegment segment;
        segment.buffer = entry.buffer;
        segment.data = ByteBuffer{mapped, static_cast<size_t>(StorageBufferSize)};
        segment.mapped = true;
        segment.poolIndex = static_cast<uint32_t>(i);
        return segment;
      }

      if (g_storageOverflowPool.size() < storage_overflow_pool_limit()) {
        const wgpu::BufferDescriptor descriptor{
            .label = "GekkoAOT Pooled Overflow Storage Staging",
            .usage = wgpu::BufferUsage::MapWrite | wgpu::BufferUsage::CopySrc,
            .size = StorageBufferSize,
            .mappedAtCreation = true,
        };
        auto buffer = g_device.CreateBuffer(&descriptor);
        auto* mapped = static_cast<uint8_t*>(buffer.GetMappedRange(0, StorageBufferSize));
        AURORA_ASSERT(mapped != nullptr, "failed to map newly-created pooled overflow storage buffer");

        const size_t index = g_storageOverflowPool.size();
        g_storageOverflowPool.emplace_back();
        auto& entry = g_storageOverflowPool.back();
        entry.buffer = buffer;
        entry.mapState.store(BufferMapState::Mapped, std::memory_order_release);
        entry.inUse = true;

        Log.info("GEKKOAOT_STORAGE_POOL_V161G=1 action=grow index={} pool={} limit={} segment_mib={}", index,
                 g_storageOverflowPool.size(), storage_overflow_pool_limit(), StorageBufferSize / (1024u * 1024u));

        StorageStagingSegment segment;
        segment.buffer = std::move(buffer);
        segment.data = ByteBuffer{mapped, static_cast<size_t>(StorageBufferSize)};
        segment.mapped = true;
        segment.poolIndex = static_cast<uint32_t>(index);
        return segment;
      }
    }

    // All pooled segments are still in flight/remapping. Let Dawn make GPU/map
    // progress instead of allocating an unbounded stream of new buffers.
    wait_for_gpu_progress(std::chrono::microseconds{50});
  }
}

void recycle_storage_overflow_segment(uint32_t poolIndex) {
  if (poolIndex == UINT32_MAX) {
    return;
  }

  wgpu::Buffer buffer;
  {
    std::lock_guard lock{g_storageOverflowPoolMutex};
    if (poolIndex >= g_storageOverflowPool.size()) {
      Log.error("GEKKOAOT_STORAGE_POOL_V161G recycle invalid index={}", poolIndex);
      return;
    }
    auto& entry = g_storageOverflowPool[poolIndex];
    entry.mapState.store(BufferMapState::Mapping, std::memory_order_release);
    buffer = entry.buffer;
  }

  buffer.MapAsync(
      wgpu::MapMode::Write, 0, StorageBufferSize, wgpu::CallbackMode::AllowSpontaneous,
      [poolIndex](wgpu::MapAsyncStatus status, wgpu::StringView message) {
        std::lock_guard lock{g_storageOverflowPoolMutex};
        if (poolIndex >= g_storageOverflowPool.size()) {
          return;
        }
        auto& entry = g_storageOverflowPool[poolIndex];
        if (status == wgpu::MapAsyncStatus::Success) {
          entry.mapState.store(BufferMapState::Mapped, std::memory_order_release);
          entry.inUse = false;
          return;
        }
        Log.warn("GEKKOAOT_STORAGE_POOL_V161G remap {}: {}", magic_enum::enum_name(status), message);
        entry.mapState.store(BufferMapState::Unmapped, std::memory_order_release);
        entry.inUse = false;
      });
}
'''
    s, c = replace_once(s, old_detail, new_detail, "frame pool functions")
    changed |= c

    old_end = '''    (void)storageOverflow;
    g_frameSlots.release(frameSlot);
'''
    new_end = '''    for (const auto& segment : storageOverflow) {
      recycle_storage_overflow_segment(segment.poolIndex);
    }
    g_frameSlots.release(frameSlot);
'''
    s, c = replace_once(s, old_end, new_end, "frame pool recycle")
    changed |= c

    p.write_text(s, encoding="utf-8")

p, s = load("lib/gfx/recording.cpp")
if marker not in s:
    old_alloc = '''  const wgpu::BufferDescriptor descriptor{
      .label = "GekkoAOT Overflow Storage Staging",
      .usage = wgpu::BufferUsage::MapWrite | wgpu::BufferUsage::CopySrc,
      .size = StorageBufferSize,
      .mappedAtCreation = true,
  };
  StorageStagingSegment segment;
  segment.buffer = webgpu::g_device.CreateBuffer(&descriptor);
  auto* mapped = static_cast<uint8_t*>(segment.buffer.GetMappedRange(0, StorageBufferSize));
  if (mapped == nullptr) {
    Log.error("GEKKOAOT_AURORA_STORAGE_V4 phase=rollover-rejected reason=map-failed required={}", requiredEnd);
    return false;
  }
  segment.data = ByteBuffer{mapped, static_cast<size_t>(StorageBufferSize)};
  segment.mapped = true;
'''
    new_alloc = '''  // GEKKOAOT_STORAGE_POOL_V161G: acquire a persistent, already-mapped
  // overflow segment. It is unmapped before submit and MapAsync-remapped for
  // later frames, eliminating per-rollover Dawn CreateBuffer/MapAtCreation.
  StorageStagingSegment segment = acquire_storage_overflow_segment();
  if (!segment.mapped || !segment.buffer || segment.data.data() == nullptr) {
    Log.error("GEKKOAOT_AURORA_STORAGE_V4 phase=rollover-rejected reason=pool-acquire-failed required={}",
              requiredEnd);
    return false;
  }
'''
    s, c = replace_once(s, old_alloc, new_alloc, "recording pooled allocation")
    if c:
        p.write_text(s, encoding="utf-8"); changed = True

print("GEKKOAOT_AURORA_STORAGE_POOL_V161G=1 mode=persistent-remap pool-default=16 pool-max=64 segment-mib=64" +
      (" action=installed" if changed else " action=already-installed"))
