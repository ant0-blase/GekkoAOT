"""Route HD generation through Aurora's existing asynchronous replacement cache."""
from pathlib import Path
import sys
root=Path(sys.argv[1])
marker='GEKKOAOT_HD_RESOURCES_V1'

def edit(name,changes):
    p=root/name;s=p.read_text()
    if marker in s:return
    for old,new,count in changes:
        if s.count(old)!=count:
            raise RuntimeError(f'HD resources: {name}: missing/ambiguous anchor {old[:90]}')
        s=s.replace(old,new)
    p.write_text('// '+marker+'\n'+s)

edit('lib/gx/texture.cpp',[
    ('#include "texture.hpp"','#include "texture.hpp"\n#include "../gfx/gekkoaot_hd_resources.hpp"',1),
    ('gfx::texture_replacement::should_build_source_key()',
     '(gfx::texture_replacement::should_build_source_key() || gfx::hd::enabled())',2),
    ('    keys = hash_texture_source(obj, nullptr, buildSourceKey);\n    if (keys->sourceKey.has_value()) {\n'
     '      use_replacement(gfx::texture_replacement::find_source_replacement(obj, *keys->sourceKey), handle, replacementId);',
     '    keys = hash_texture_source(obj, nullptr, buildSourceKey);\n    if (keys->sourceKey.has_value()) {\n'
     '      if(!use_replacement(gfx::texture_replacement::find_source_replacement(obj, *keys->sourceKey), handle, replacementId))\n'
     '        use_replacement(gfx::hd::request(obj, *keys->sourceKey, nullptr), handle, replacementId);',1),
    ('      use_replacement(gfx::texture_replacement::find_source_replacement(obj, *keys->sourceKey), handle, replacementId);',
     '      if(!use_replacement(gfx::texture_replacement::find_source_replacement(obj, *keys->sourceKey), handle, replacementId))\n'
     '        use_replacement(gfx::hd::request(obj, *keys->sourceKey, &tlut), handle, replacementId);',1),
])
edit('lib/gfx/texture_replacement.cpp',[
    ('#include "texture_replacement.hpp"','#include "texture_replacement.hpp"\n#include "gekkoaot_hd_resources.hpp"',1),
    ('s_replacementCacheBytes > kReplacementCacheBudgetBytes','s_replacementCacheBytes > gfx::hd::replacement_budget()',1),
    ('void shutdown() noexcept {\n  clear_replacements();\n  stop_worker_pool();',
     'void shutdown() noexcept {\n  clear_replacements();\n  stop_worker_pool();\n  hd::report();hd::reset();',1),
    ('ReplacementRegistration register_virtual_replacement(std::string_view path, VirtualFileSource source,\n'
     '                                                     ReplacementOptions options) {',
     'static ReplacementRegistration register_virtual_replacement_impl(std::string_view path, VirtualFileSource source,\n'
     '                                                     ReplacementOptions options, bool invalidateCache) {',1),
    ('      .virtualReadState = std::make_shared<VirtualReadState>(),\n  });\n  ++s_sourceEntryCount;\n'
     '  const auto snapshot = snapshot_entry(replacementKey, entries.back());\n'
     '  queue_thumbnail_load(snapshot);\n  erase_cache_locked(replacementKey);\n  gx::clear_static_texture_cache();\n  return registration;\n}',
     '      .virtualReadState = std::make_shared<VirtualReadState>(),\n  });\n  ++s_sourceEntryCount;\n'
     '  const auto snapshot = snapshot_entry(replacementKey, entries.back());\n'
     '  queue_thumbnail_load(snapshot);\n  erase_cache_locked(replacementKey);\n  if(invalidateCache)gx::clear_static_texture_cache();\n  return registration;\n}\n'
     'ReplacementRegistration register_virtual_replacement(std::string_view path,VirtualFileSource source,ReplacementOptions options) {\n'
     '  return register_virtual_replacement_impl(path,source,options,true);\n}\n'
     'ReplacementRegistration register_generated_replacement(std::string_view path,VirtualFileSource source,ReplacementOptions options) {\n'
     '  return register_virtual_replacement_impl(path,source,options,false);\n}',1),
])

# Generated DDS images already contain their full mip chain. Thumbnail jobs
# otherwise run the complete generator, then block a second worker waiting for
# that same image and temporarily bind a 64-pixel mip as its HD replacement.
p=root/'lib/gfx/texture_replacement.cpp';s=p.read_text()
if 'GEKKOAOT_GENERATED_STREAMING_V2' not in s:
 a='  queue_thumbnail_load(snapshot);\n  erase_cache_locked(replacementKey);\n  if(invalidateCache)'
 assert s.count(a)==1
 s=s.replace(a,'  if(invalidateCache)queue_thumbnail_load(snapshot);\n  erase_cache_locked(replacementKey);\n  if(invalidateCache)')
 a='        job = std::move(s_highPriorityJobs.front());\n        s_highPriorityJobs.pop_front();'
 assert s.count(a)==1
 s=s.replace(a,'''        // Small images and intensity/alpha font atlases must not wait behind
        // a screen-sized neural job. Preserve FIFO periodically to avoid
        // starving larger surfaces; user pack selection is unchanged.
        auto selected=s_highPriorityJobs.begin();
        static thread_local unsigned quickBurst=0;
        if(quickBurst<8){
          auto quick=std::find_if(s_highPriorityJobs.begin(),s_highPriorityJobs.end(),[](const auto& pending){
            const auto* key=std::get_if<TextureSourceKey>(&pending.entry.key);
            return key && pending.entry.virtualPath.starts_with("@gekkoaot-") &&
              (key->format<=3u || std::uint64_t(key->width)*key->height<=4096u);
          });
          if(quick!=s_highPriorityJobs.end()){selected=quick;++quickBurst;}else quickBurst=0;
        }else quickBurst=0;
        job=std::move(*selected);s_highPriorityJobs.erase(selected);''')
 p.write_text('// GEKKOAOT_GENERATED_STREAMING_V2\n'+s)

# Keep cached uploads and inexpensive resources runnable while one neural
# reconstruction uses the compute executor. Previously four workers could all
# block behind the same neural mutex, starving an already-cached sky/menu.
p=root/'lib/gfx/texture_replacement.cpp';s=p.read_text()
if 'GEKKOAOT_GENERATED_STREAMING_V3' not in s:
 anchor='void worker_main(std::stop_token token) {'
 assert s.count(anchor)==1
 s=s.replace(anchor,'''unsigned s_generatedComputing=0; // protected by s_jobMutex
bool expensive_generated(const LoadJob& job){
  return job.entry.kind==EntryKind::Virtual && job.entry.virtualPath.starts_with("@gekkoaot-") &&
    gfx::hd::generated_needs_compute(job.entry.source.userData);
}
void worker_main(std::stop_token token) {''')
 s=s.replace('    LoadJob job;\n    {\n      std::unique_lock lock{s_jobMutex};','    LoadJob job;bool computing=false;\n    {\n      std::unique_lock lock{s_jobMutex};')
 old='''        return token.stop_requested() ||
               (!s_workersPaused && (!s_highPriorityJobs.empty() || !s_lowPriorityJobs.empty()));'''
 new='''        return token.stop_requested() || (!s_workersPaused &&
          (!s_lowPriorityJobs.empty() || std::any_of(s_highPriorityJobs.begin(),s_highPriorityJobs.end(),
            [](const auto& pending){return !s_generatedComputing || !expensive_generated(pending);})));
'''.rstrip()
 assert s.count(old)==1;s=s.replace(old,new)
 start=s.index('      if (!s_highPriorityJobs.empty()) {',s.index('void worker_main'))
 end=s.index('\n    }\n\n    std::optional<gfx::ConvertedTexture>',start)
 s=s[:start]+'''      auto selected=std::find_if(s_highPriorityJobs.begin(),s_highPriorityJobs.end(),
        [](const auto& pending){return !expensive_generated(pending);});
      // A cold generator still gets every ninth turn under continuous cached
      // traffic. At most one worker waits on expensive inference at a time.
      static thread_local unsigned fastBurst=0;
      if(selected!=s_highPriorityJobs.end() && fastBurst++>=8 && !s_generatedComputing){selected=s_highPriorityJobs.begin();fastBurst=0;}
      if(selected==s_highPriorityJobs.end() && !s_generatedComputing && !s_highPriorityJobs.empty())selected=s_highPriorityJobs.begin();
      if(selected!=s_highPriorityJobs.end()){
        computing=expensive_generated(*selected);if(computing)++s_generatedComputing;
        job=std::move(*selected);s_highPriorityJobs.erase(selected);
      }else {
        job=std::move(s_lowPriorityJobs.front());s_lowPriorityJobs.pop_front();
      }'''+s[end:]
 anchor='''      std::lock_guard lock{s_jobMutex};
      if (!token.stop_requested()) {'''
 assert s.count(anchor)==1;s=s.replace(anchor,'''      std::lock_guard lock{s_jobMutex};
      if(computing)--s_generatedComputing;
      if (!token.stop_requested()) {''')
 p.write_text('// GEKKOAOT_GENERATED_STREAMING_V3\n'+s)
