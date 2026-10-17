#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Opt-in frame/wait telemetry, applied after the pinned Aurora overlays."""
from pathlib import Path
import sys
root=Path(sys.argv[1])
def edit(name, replacements):
 p=root/name;s=p.read_text();marker='GEKKOAOT_CAPTURE_PROFILE_V1'
 if marker in s:return
 for old,new,count in replacements:
  if s.count(old)!=count:raise SystemExit(f'{name}: expected {count} anchors, got {s.count(old)}: {old[:90]}')
  s=s.replace(old,new)
 p.write_text('// '+marker+'\n'+s)

edit('lib/gfx/render_worker.hpp',[
 ('  uint64_t frameId = 0;', '  uint64_t frameId = 0;\n  uint64_t captureFrame = 0;\n  uint64_t captureQueued = 0;',1),
])
edit('lib/gfx/render_worker.cpp',[
 ('#include "render_worker.hpp"','#include "render_worker.hpp"\n#include "../gekkoaot_capture.hpp"',1),
 ('    auto item = g_queue.pop_for(IdlePumpInterval, closed);', '    auto item = GekkoAOT::Capture::Invoke(GekkoAOT::Capture::Kind::RenderQueueEmpty, 0, [&] {\n      return g_queue.pop_for(IdlePumpInterval, closed);\n    });',1),
 ('    if (item->work) {','    GekkoAOT::Capture::FrameContext context(item->captureFrame);\n    if(item->captureQueued && GekkoAOT::Capture::Enabled())\n      GekkoAOT::Capture::Record(GekkoAOT::Capture::Kind::RenderQueueAge, item->captureQueued, GekkoAOT::Capture::Now(), item->captureFrame);\n    GekkoAOT::Capture::Scope work(GekkoAOT::Capture::Kind::RenderWork);\n    if (item->work) {',1),
 ('  g_pendingItems.fetch_add(1, std::memory_order_acq_rel);','  item.captureFrame=GekkoAOT::Capture::frame_id;\n  item.captureQueued=GekkoAOT::Capture::Enabled()?GekkoAOT::Capture::Now():0;\n  g_pendingItems.fetch_add(1, std::memory_order_acq_rel);',1),
 ('  m_notFull.wait(lock, [&] { return m_closed || m_items.size() < m_capacity; });','  { GekkoAOT::Capture::Scope wait(GekkoAOT::Capture::Kind::RenderQueueFull);\n    m_notFull.wait(lock, [&] { return m_closed || m_items.size() < m_capacity; });\n  }',1),
 ('  sync->cv.wait(lock, [&] { return sync->complete; });','  GekkoAOT::Capture::Scope wait(GekkoAOT::Capture::Kind::RenderFence);\n  sync->cv.wait(lock, [&] { return sync->complete; });',1),
])
edit('lib/gfx/frame.cpp',[
 ('#include "render_worker.hpp"','#include "render_worker.hpp"\n#include "../gekkoaot_capture.hpp"',1),
 ('  frame.frameId = g_nextFrameId++;','  frame.frameId = g_nextFrameId++;\n  GekkoAOT::Capture::frame_id=frame.frameId;\n  GekkoAOT::Capture::Mark(GekkoAOT::Capture::Kind::FrameBegin, frame.frameId);',1),
 ('  const uint64_t frameId = frame.frameId;','  const uint64_t frameId = frame.frameId;\n  GekkoAOT::Capture::Mark(GekkoAOT::Capture::Kind::FrameEnd, frameId);',1),
 ('void after_present() noexcept {','void after_present() noexcept {\n  GekkoAOT::Capture::Mark(GekkoAOT::Capture::Kind::WsiSuccess, GekkoAOT::Capture::frame_id);',1),
 ('  g_stagingBuffers[slot].MapAsync(', '  const auto captureFrame=GekkoAOT::Capture::frame_id;\n  g_stagingBuffers[slot].MapAsync(',1),
 ('      [slot, releaseSlotOnCompletion](wgpu::MapAsyncStatus status, wgpu::StringView message) {','      [slot, releaseSlotOnCompletion, captureFrame](wgpu::MapAsyncStatus status, wgpu::StringView message) {\n        if(status == wgpu::MapAsyncStatus::Success)\n          GekkoAOT::Capture::Mark(GekkoAOT::Capture::Kind::GpuStagingReady, captureFrame, slot);',1),
])
edit('lib/gfx/pipeline_cache.cpp',[
 ('#include "pipeline_cache.hpp"','#include "pipeline_cache.hpp"\n#include "../gekkoaot_capture.hpp"',1),
 ('    auto result = pending.create();','    auto result = GekkoAOT::Capture::Invoke(GekkoAOT::Capture::Kind::PipelineCompile, pending.hash, [&] { return pending.create(); });',1),
 ('    g_pipelineReadyCv.wait(lock, [=] { return g_pipelines.contains(runtimeKey) || g_pipelineThreadEnd; });','    GekkoAOT::Capture::Scope captureWait(GekkoAOT::Capture::Kind::PipelineWait);\n    g_pipelineReadyCv.wait(lock, [=] { return g_pipelines.contains(runtimeKey) || g_pipelineThreadEnd; });',1),
 ('        std::this_thread::sleep_for(std::chrono::microseconds(sleepUs));','        { GekkoAOT::Capture::Scope captureWait(GekkoAOT::Capture::Kind::PipelineThrottle);\n          std::this_thread::sleep_for(std::chrono::microseconds(sleepUs));\n        }',1),
])
# Every surface path is covered. after_present is called only after success;
# neither GetCurrentTexture nor Submit by itself counts as displayed content.
edit('lib/aurora.cpp',[
 ('#include "gfx/render_worker.hpp"','#include "gfx/render_worker.hpp"\n#include "gekkoaot_capture.hpp"',1),
 ('g_surface.GetCurrentTexture(&surfaceTexture);','{ GekkoAOT::Capture::Scope capture(GekkoAOT::Capture::Kind::SurfaceAcquire);\n      g_surface.GetCurrentTexture(&surfaceTexture); }',2),
 ('g_surface.GetCurrentTexture(&surface_texture);','{ GekkoAOT::Capture::Scope capture(GekkoAOT::Capture::Kind::SurfaceAcquire);\n      g_surface.GetCurrentTexture(&surface_texture); }',1),
 ('          status = g_surface.Present();','{ GekkoAOT::Capture::Scope capture(GekkoAOT::Capture::Kind::SurfacePresent);\n          status = g_surface.Present(); }',1),
 ('const auto present_status = g_surface.Present();','const auto present_status = GekkoAOT::Capture::Invoke(GekkoAOT::Capture::Kind::SurfacePresent, GekkoAOT::Capture::frame_id, [&] { return g_surface.Present(); });',2),
 ('g_queue.Submit(1, &buffer);','g_queue.Submit(1, &buffer);\n    GekkoAOT::Capture::Mark(GekkoAOT::Capture::Kind::GpuSubmit, GekkoAOT::Capture::frame_id);',2),
])

# Add optional real queue-completion telemetry without waiting or changing PE.
p=root/'lib/aurora.cpp';s=p.read_text()
if 'GEKKOAOT_CAPTURE_GPU_COMPLETE_V1' not in s:
 s=s.replace('    GekkoAOT::Capture::Mark(GekkoAOT::Capture::Kind::GpuSubmit, GekkoAOT::Capture::frame_id);','')
 import re
 def submit(m):
  return m.group(0)+'''
    GekkoAOT::Capture::Mark(GekkoAOT::Capture::Kind::GpuSubmit, GekkoAOT::Capture::frame_id);
    if (GekkoAOT::Capture::Enabled()) {
      const auto captureFrame=GekkoAOT::Capture::frame_id;
      g_queue.OnSubmittedWorkDone(wgpu::CallbackMode::AllowSpontaneous,
        [captureFrame](wgpu::QueueWorkDoneStatus status, wgpu::StringView) {
          if(status==wgpu::QueueWorkDoneStatus::Success)
            GekkoAOT::Capture::Mark(GekkoAOT::Capture::Kind::GpuComplete,captureFrame);
        });
    }'''
 s,n=re.subn(r'g_queue.Submit\(1, &(?:buffer|command)\);',submit,s)
 if n!=3:raise SystemExit(f'GPU completion telemetry: expected 3 submits, got {n}')
 p.write_text('// GEKKOAOT_CAPTURE_GPU_COMPLETE_V1\n'+s)
