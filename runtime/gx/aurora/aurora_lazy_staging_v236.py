#!/usr/bin/env python3
"""Compose lazy frame staging allocation without changing pool capacity or mapping."""
from pathlib import Path
import sys

if len(sys.argv) != 2:
    raise SystemExit('usage: aurora_lazy_staging_v236.py <aurora-source>')
path = Path(sys.argv[1]) / 'lib/gfx/frame.cpp'
source = path.read_text()
marker = 'GEKKOAOT_LAZY_STAGING_V236'
old_initialize = '''  for (size_t i = 0; i < g_stagingBuffers.size(); ++i) {
    const auto label = fmt::format("Staging Buffer {}", i);
    createBuffer(g_stagingBuffers[i], wgpu::BufferUsage::MapWrite | wgpu::BufferUsage::CopySrc, StagingBufferSize,
                 label.c_str());
  }
  for (auto& state : g_mappingStates) {
    state.store(BufferMapState::Unmapped, std::memory_order_release);
  }
  for (size_t slot = 0; slot < g_stagingBuffers.size(); ++slot) {
    map_staging_buffer(slot);
  }
'''
new_initialize = '''  // GEKKOAOT_LAZY_STAGING_V236: keep all five slots available, but create
  // a staging allocation only after its exclusive frontend lease is acquired.
  // Unused slots reserve no 152 MiB buffer and start no asynchronous mapping.
  g_stagingBuffers.fill({});
  for (auto& state : g_mappingStates) {
    state.store(BufferMapState::Unmapped, std::memory_order_release);
  }
'''
old_wait = '''bool wait_for_staging_buffer(size_t slot) {
  ZoneScopedN("Wait for buffer map");
  map_staging_buffer(slot);
'''
new_wait = '''bool wait_for_staging_buffer(size_t slot) {
  ZoneScopedN("Wait for buffer map");
  // GEKKOAOT_LAZY_STAGING_V236: the slot pool grants exclusive ownership
  // before this call. A worker sees a slot only after begin_frame publishes
  // its packet. Retained buffers use the unchanged MapAsync/remap lifecycle.
  if (!g_stagingBuffers[slot]) {
    const auto label = fmt::format("Staging Buffer {}", slot);
    const wgpu::BufferDescriptor descriptor{
        .label = label.c_str(),
        .usage = wgpu::BufferUsage::MapWrite | wgpu::BufferUsage::CopySrc,
        .size = StagingBufferSize,
    };
    g_stagingBuffers[slot] = g_device.CreateBuffer(&descriptor);
    size_t allocated = 0;
    for (const auto& buffer : g_stagingBuffers) allocated += bool(buffer);
    std::fprintf(stderr,
                 "GEKKOAOT_LAZY_STAGING_V236=1 allocated=%zu capacity=%zu bytes_per_buffer=%llu\\n",
                 allocated, g_stagingBuffers.size(), static_cast<unsigned long long>(StagingBufferSize));
  }
  map_staging_buffer(slot);
'''
if marker in source:
    if new_initialize not in source or new_wait not in source:
        raise SystemExit('lazy staging marker exists without the complete expected composition')
    print('GekkoAOT lazy staging v236 already composed')
else:
    for anchor in (old_initialize, old_wait):
        if source.count(anchor) != 1:
            raise SystemExit('lazy staging: expected exactly one unmodified initialization and acquisition anchor')
    source = source.replace(old_initialize, new_initialize).replace(old_wait, new_wait)
    if '#include <cstdio>\n' not in source:
        if source.count('#include <cstdint>\n') != 1:
            raise SystemExit('lazy staging: missing cstdint include anchor')
        source = source.replace('#include <cstdint>\n', '#include <cstdint>\n#include <cstdio>\n')
    path.write_text(source)
    print('GekkoAOT lazy staging v236 composed: unchanged five-slot capacity and asynchronous mapping')
