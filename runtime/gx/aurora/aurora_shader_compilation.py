"""Pinned Aurora overlay: startup cache warmup and lossless asynchronous compilation.
Recording queues pipelines; encoding waits only for the exact required pipeline.
"""
from pathlib import Path
import sys
root = Path(sys.argv[1])
marker = 'GEKKOAOT_SHADER_COMPILATION_V1'
updates = {}
def edit(name, changes):
    p = root / name
    s = p.read_text()
    if marker in s:
        return
    for old, new in changes:
        if s.count(old) != 1:
            raise RuntimeError(f'{name}: shader compilation anchor changed: {old[:100]} ({s.count(old)})')
        s = s.replace(old, new)
    updates[p] = '// ' + marker + '\n' + s
edit('lib/gfx/pipeline_cache.hpp', [
    ('void rebuild_pipeline_cache();', 'void rebuild_pipeline_cache();\nvoid compile_startup_shaders();'),
])
edit('lib/gfx/pipeline_cache.cpp', [
    ('  const auto path = io::fs_path_to_string(io::fs_path_from_string(g_config.cachePath) / "pipeline_cache.db");', r'''  auto directory=io::fs_path_from_string(g_config.cachePath);
  const char* game=std::getenv("GEKKOAOT_GAME");
  if(!game || !*game)game=std::getenv("GEKKOAOT_ISO");
  if(game && *game){
    uint64_t id=14695981039346656037ull;
    for(const auto* p=reinterpret_cast<const unsigned char*>(game);*p;++p){id^=*p;id*=1099511628211ull;}
    char name[24];std::snprintf(name,sizeof(name),"%016llx",static_cast<unsigned long long>(id));
    directory/="shader-games";directory/=name;
    std::error_code error;std::filesystem::create_directories(directory,error);
    if(error){Log.error("Failed to create game shader cache: {}",error.message());pipeline_cache_abort();return false;}
  }
  const auto path = io::fs_path_to_string(directory / "pipeline_cache.db");
  Log.info("GEKKOAOT_SHADER_CACHE path={}",path);'''),
    ('static std::atomic_bool g_gpuCachePrunePending = false;', '''static std::atomic_bool g_gpuCachePrunePending = false;
static std::atomic_uint64_t g_deferredPipelines = 0, g_requiredPipelineWaits = 0;
static bool async_compilation() {
  static const bool enabled=[] {
    const char* value=std::getenv("GEKKOAOT_ASYNC_SHADER_COMPILATION");
    return !(value && std::string_view(value)=="0");
  }();
  return enabled;
}'''),
    ('  Normal,     // async skip draw', '  Normal,     // async recording; encoding waits for the required pipeline'),
    ('      g_pendingPipelines.insert(runtimeKey);', '      if(priority==PipelinePriority::Normal)++g_deferredPipelines;\n      g_pendingPipelines.insert(runtimeKey);'),
    ('return resolve_pipeline(ShaderType::Clear, config, layout, PipelinePriority::Normal);', 'return resolve_pipeline(ShaderType::Clear, config, layout, async_compilation()?PipelinePriority::Normal:PipelinePriority::Blocking);'),
    ('''  // GekkoAOT v141: a retail GX draw is ordered work, not optional UI work.
  // Wait only when this exact pipeline has not been compiled yet.
  return resolve_pipeline(ShaderType::GX, config, layout, PipelinePriority::Blocking);''', '''  // The immutable packet records the pipeline reference immediately. The render
  // worker waits for this exact pipeline before encoding its draw (never skips).
  return resolve_pipeline(ShaderType::GX, config, layout, async_compilation()?PipelinePriority::Normal:PipelinePriority::Blocking);'''),
    ('return resolve_pipeline(ShaderType::Rml, config, {}, PipelinePriority::Blocking);', 'return resolve_pipeline(ShaderType::Rml, config, {}, async_compilation()?PipelinePriority::Normal:PipelinePriority::Blocking);'),
    ('  rebuild_pipeline_cache();\n  if (!g_pipelineCacheBroken && loadedCount > 0) {', '  // Warmup is scheduled after the final scene attachment layout is selected.\n  if (!g_pipelineCacheBroken && loadedCount > 0) {'),
    ('void initialize_pipeline_cache() {', '''void compile_startup_shaders() {
  const char* enabled=std::getenv("GEKKOAOT_STARTUP_SHADER_COMPILATION");
  if(enabled && std::string_view(enabled)=="0")return;
  const auto start=std::chrono::steady_clock::now();
  // Layout is final only after the bridge has selected MSAA and normal capture.
  const auto scene=scene_render_target_layout();
  auto offscreen=scene;offscreen.colorAttachmentCount=1;offscreen.sampleCount=1;
  detail::finalize_render_target_layout(offscreen);
  std::vector<KnownPipeline> known;
  {std::lock_guard lock{g_pipelineMutex};
    for(const auto& p:g_knownPipelines|std::views::values)known.push_back(p);}
  std::ranges::sort(known,{},&KnownPipeline::firstFrameUsed);
  std::vector<PipelineRef> required;required.reserve(known.size()*2);
  for(const auto& p:known)std::visit([&](const auto& config){
    required.push_back(resolve_pipeline(p.type,config,scene,PipelinePriority::Normal));
    if(offscreen.key!=scene.key)required.push_back(resolve_pipeline(p.type,config,offscreen,PipelinePriority::Normal));
  },p.config);
  std::fprintf(stderr,"GEKKOAOT_SHADER_STARTUP begin known=%zu variants=%zu async=%u\\n",known.size(),required.size(),g_hasPipelineThread?1u:0u);
  // get_pipeline handles both the compilation worker and single-thread backends.
  // It preserves ordered work and fails explicitly if compilation is invalid.
  size_t completed=0;
  for(auto ref:required){CompiledPipeline pipeline;get_pipeline(ref,pipeline);
    if(++completed%64==0)std::fprintf(stderr,"GEKKOAOT_SHADER_STARTUP progress completed=%zu total=%zu\\n",completed,required.size());}
  const auto ms=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-start).count();
  g_pipelineLayoutKey=scene.key;
  std::fprintf(stderr,"GEKKOAOT_SHADER_STARTUP complete known=%zu variants=%zu elapsed_ms=%lld\\n",known.size(),required.size(),static_cast<long long>(ms));
}

void initialize_pipeline_cache() {
  g_deferredPipelines=0;g_requiredPipelineWaits=0;'''),
    ('void shutdown_pipeline_cache() {', '''void shutdown_pipeline_cache() {
  std::fprintf(stderr,"GEKKOAOT_SHADER_COMPILATION_STATS deferred=%llu required_waits=%llu skipped_draws=0 async=%u\\n",
    static_cast<unsigned long long>(g_deferredPipelines.load()),static_cast<unsigned long long>(g_requiredPipelineWaits.load()),async_compilation()?1u:0u);'''),
    ('''  std::lock_guard guard{g_pipelineMutex};
  const auto it = g_pipelines.find(ref);
  if (it == g_pipelines.end() || !it->second.main) {
    return false;
  }
  pipeline = it->second;''', '''  std::unique_lock guard{g_pipelineMutex};
  if(!g_pipelines.contains(ref) && g_pendingPipelines.contains(ref)) {
    ++g_requiredPipelineWaits;
    GekkoAOT::Perf::Scope latency(GekkoAOT::Perf::Metric::PipelineWait);
    GekkoAOT::Capture::Scope captureWait(GekkoAOT::Capture::Kind::PipelineWait);
    if(g_hasPipelineThread) {
      promote_pending_pipeline(ref,PipelinePriority::Blocking);
      g_pipelineQueueCv.notify_one();
      g_pipelineReadyCv.wait(guard,[=]{return g_pipelines.contains(ref)||g_pipelineThreadEnd;});
    } else {
      auto pending=take_pending_pipeline(ref);
      if(pending){auto result=pending->create();const auto count=result.pipeline_count();
        g_pipelines.try_emplace(ref,std::move(result));notify_pipeline_ready(true,count);}
    }
  }
  const auto it = g_pipelines.find(ref);
  if (it == g_pipelines.end() || !it->second.main) {
    if(g_pipelineThreadEnd)return false;
    std::fprintf(stderr,"GEKKOAOT_SHADER_COMPILATION_ERROR pipeline=%llu missing-or-invalid\\n",static_cast<unsigned long long>(ref));
    std::abort();
  }
  pipeline = it->second;'''),
])
# ShaderConfig padding now stores the enhancement coverage mode. Old cached
# configurations are invalid even though their byte length stayed unchanged.
edit('lib/gx/pipeline.hpp', [('GXPipelineConfigVersion = 16;', 'GXPipelineConfigVersion = 18;')])
for p,s in updates.items():
    p.write_text(s)
