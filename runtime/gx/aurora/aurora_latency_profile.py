#!/usr/bin/env python3
"""Install opt-in, allocation-free latency instrumentation in pinned Aurora."""
from pathlib import Path
import sys

root=Path(sys.argv[1])
def edit(name, replacements):
    path=root/name
    source=path.read_text()
    marker='GEKKOAOT_LATENCY_PROFILE_V1'
    if marker in source:return
    for old,new in replacements:
        if source.count(old)!=1:raise SystemExit(f'latency instrumentation: {name}: anchor count {source.count(old)}: {old[:70]}')
        source=source.replace(old,new,1)
    path.write_text('// GEKKOAOT_LATENCY_PROFILE_V1\n'+source)

edit('lib/gfx/frame.cpp',[
    ('#include "render_worker.hpp"','#include "render_worker.hpp"\n#include "../gekkoaot_latency.hpp"'),
    ('    update_ema(g_presentPeriodNs, nowNs - previousPresentNs);','    GekkoAOT::Perf::Record(GekkoAOT::Perf::Metric::PresentInterval, nowNs - previousPresentNs);\n    update_ema(g_presentPeriodNs, nowNs - previousPresentNs);'),
    ('  shutdown_pipeline_cache();','  shutdown_pipeline_cache();\n  GekkoAOT::Perf::Report("aurora");'),
])
edit('lib/gfx/pipeline_cache.cpp',[
    ('#include "pipeline_cache.hpp"','#include "pipeline_cache.hpp"\n#include "../gekkoaot_latency.hpp"'),
    ('    g_pipelineReadyCv.wait(lock, [=] { return g_pipelines.contains(runtimeKey) || g_pipelineThreadEnd; });','    GekkoAOT::Perf::Scope latency(GekkoAOT::Perf::Metric::PipelineWait);\n    g_pipelineReadyCv.wait(lock, [=] { return g_pipelines.contains(runtimeKey) || g_pipelineThreadEnd; });'),
    ('    auto result = pending.create();','    const auto compileStart=GekkoAOT::Perf::Enabled()?GekkoAOT::Perf::Now():0;\n    auto result = pending.create();\n    if(compileStart)GekkoAOT::Perf::Record(GekkoAOT::Perf::Metric::PipelineCompile,GekkoAOT::Perf::Now()-compileStart);'),
])
edit('lib/gfx/render_worker.hpp',[
    ('  uint64_t frameId = 0;','  uint64_t frameId = 0;\n  uint64_t queuedAtNs = 0;'),
])
edit('lib/gfx/render_worker.cpp',[
    ('#include "render_worker.hpp"','#include "render_worker.hpp"\n#include "../gekkoaot_latency.hpp"'),
    ('    if (item->work) {','    if(item->queuedAtNs)GekkoAOT::Perf::Record(GekkoAOT::Perf::Metric::RenderQueue,GekkoAOT::Perf::Now()-item->queuedAtNs);\n    if (item->work) {'),
    ('  g_pendingItems.fetch_add(1, std::memory_order_acq_rel);','  item.queuedAtNs=GekkoAOT::Perf::Enabled()?GekkoAOT::Perf::Now():0;\n  g_pendingItems.fetch_add(1, std::memory_order_acq_rel);'),
])
edit('lib/aurora.cpp',[
    ('#include "gfx/render_worker.hpp"','#include "gfx/render_worker.hpp"\n#include "gekkoaot_latency.hpp"'),
    ('void present_texture(const webgpu::TextureWithSampler& source) noexcept {','void present_texture(const webgpu::TextureWithSampler& source) noexcept {\n  GekkoAOT::Perf::Scope latency(GekkoAOT::Perf::Metric::PresentCall);'),
])
