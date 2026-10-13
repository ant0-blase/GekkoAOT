#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Release producer-thread native handle caches before the device/window dies.

Render-worker TLS is destroyed when that worker joins. Offscreen encoding can
also populate the producer's TLS; its native handles must not outlive Aurora.
"""
import sys
from pathlib import Path

root=Path(sys.argv[1])
marker='GEKKOAOT_NATIVE_CACHE_LIFETIME_V317'
cpp=root/'lib/gx/pipeline.cpp'
hpp=root/'lib/gx/pipeline.hpp'
gx=root/'lib/gx/gx.cpp'
frame=root/'lib/gfx/frame.cpp'
processor=root/'lib/gx/command_processor.cpp'
processor_hpp=root/'lib/gx/command_processor.hpp'
changes={}

def replace_once(text,anchor,replacement):
    if text.count(anchor)!=1:
        raise RuntimeError('Pinned Aurora cache lifetime anchor changed: '+anchor[:80])
    return text.replace(anchor,replacement,1)

text=cpp.read_text()
if marker not in text:
    anchor='void reset_native_bind_group_cache() noexcept { g_nativeBindings.Reset(); }'
    changes[cpp]=replace_once(text,anchor,anchor+'''

// GEKKOAOT_NATIVE_CACHE_LIFETIME_V317
void clear_native_render_caches() noexcept {
  t_gekkoaotPipelines.fill({});
  t_gekkoaotBindGroups.fill({});
  t_gekkoaotDrawsSinceTrim = 0;
  g_nativeBindings.Reset();
}
''')
text=hpp.read_text()
if marker not in text:
    changes[hpp]=replace_once(text,'void reset_native_bind_group_cache() noexcept;',
        'void reset_native_bind_group_cache() noexcept;\n// '+marker+'\nvoid clear_native_render_caches() noexcept;')
text=gx.read_text()
if marker not in text:
    changes[gx]=replace_once(text,'void shutdown() noexcept {\n  sLogicDestination = {};',
        '''void shutdown() noexcept {
  // GEKKOAOT_NATIVE_CACHE_LIFETIME_V317: gfx::shutdown joined the worker;
  // release the producer's retained handles before destroying the device.
  clear_native_render_caches();
  sLogicDestination = {};''')
text=changes.get(gx,text)
if 'GEKKOAOT_NATIVE_GX_STATE_REINIT_V317' not in text:
    text=replace_once(text,'void initialize() noexcept {\n  {',
        '''void initialize() noexcept {
  // GEKKOAOT_NATIVE_GX_STATE_REINIT_V317: cached BP/CP/XF words and texture
  // bindings belong to a renderer lifetime, never to the next device.
  g_gxState = GXState{};
  {''')
    changes[gx]=text
text=changes.get(gx,text)
if 'GEKKOAOT_NATIVE_DERIVED_STATE_REINIT_V317' not in text:
    text=replace_once(text,'#include "pipeline.hpp"',
        '#include "pipeline.hpp"\n#include "command_processor.hpp"')
    text=replace_once(text,'  g_gxState = GXState{};',
        '  g_gxState = GXState{};\n  // GEKKOAOT_NATIVE_DERIVED_STATE_REINIT_V317\n  fifo::reset_renderer_draw_cache();')
    changes[gx]=text
if 'GEKKOAOT_NATIVE_GX_RESOURCE_RELEASE_V317' not in text:
    changes[gx]=replace_once(text,'  g_emptyLogicBindGroup = {};',
        '''  g_emptyLogicBindGroup = {};
  // GEKKOAOT_NATIVE_GX_RESOURCE_RELEASE_V317: these default GX resources
  // also own the device. Release them before webgpu::shutdown, not at exit.
  g_emptyTextureBindGroup = {};
  sPipelineLayout = {};
  sEmptyTextureView = {};
  sEmptyTexture = {};
  sEmptySampler = {};''')
text=frame.read_text()
if 'GEKKOAOT_NATIVE_STORAGE_RELEASE_V317' not in text:
    text=replace_once(text,'std::mutex g_storageOverflowPoolMutex;',
        'std::mutex g_storageOverflowPoolMutex;\nuint64_t g_storageOverflowGeneration = 1; // guarded by pool mutex')
    text=replace_once(text,'''  wgpu::Buffer buffer;
  {
    std::lock_guard lock{g_storageOverflowPoolMutex};''',
        '''  wgpu::Buffer buffer;
  uint64_t generation;
  {
    std::lock_guard lock{g_storageOverflowPoolMutex};
    generation = g_storageOverflowGeneration;''')
    text=replace_once(text,'''      [poolIndex](wgpu::MapAsyncStatus status, wgpu::StringView message) {
        std::lock_guard lock{g_storageOverflowPoolMutex};
        if (poolIndex >= g_storageOverflowPool.size()) {''',
        '''      [poolIndex,generation](wgpu::MapAsyncStatus status, wgpu::StringView message) {
        std::lock_guard lock{g_storageOverflowPoolMutex};
        if (generation != g_storageOverflowGeneration || poolIndex >= g_storageOverflowPool.size()) {''')
    text=replace_once(text,'''  for (auto& packet : g_framePackets) {
    packet = {};
  }
  shutdown_recording();''',
        '''  for (auto& packet : g_framePackets) {
    packet = {};
  }
  // GEKKOAOT_NATIVE_STORAGE_RELEASE_V317: MapAsync can call back while a
  // buffer is released. Detach under the lock, release outside it, and reject
  // callbacks belonging to the previous renderer lifetime after a restart.
  std::deque<StorageOverflowPoolEntry> retiredStorage;
  {
    std::lock_guard lock{g_storageOverflowPoolMutex};
    ++g_storageOverflowGeneration;
    retiredStorage.swap(g_storageOverflowPool);
  }
  retiredStorage.clear();
  shutdown_recording();''')
    changes[frame]=text
text=processor.read_text()
if 'GEKKOAOT_NATIVE_DERIVED_STATE_REINIT_V317' not in text:
    changes[processor]=replace_once(text,'void clear_draw_cache() noexcept {',
        '''// GEKKOAOT_NATIVE_DERIVED_STATE_REINIT_V317: pipeline references name
// resources owned by this device; retaining them can silently skip new draws.
void reset_renderer_draw_cache() noexcept {
  sDrawCache = {};
  sPipelineStateCache.fill({});
  sPipelineStateCacheHits = 0;
  sPipelineStateCacheMisses = 0;
  sFogRangeLuts.clear();
}

void clear_draw_cache() noexcept {''')
text=processor_hpp.read_text()
if 'reset_renderer_draw_cache' not in text:
    changes[processor_hpp]=replace_once(text,'void clear_draw_cache() noexcept;',
        'void clear_draw_cache() noexcept;\nvoid reset_renderer_draw_cache() noexcept;')
# Validate every anchor before writing any prepared dependency file.
for path,text in changes.items():path.write_text(text)
