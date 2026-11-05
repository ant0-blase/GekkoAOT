"""Versioned EFB-copy producer/consumer provenance; no guest rendering changes."""
from pathlib import Path
import sys

root=Path(sys.argv[1]);marker='GEKKOAOT_SCENE_RESOURCE_HISTORY_V1';updates={}


def edit(name, changes):
    path=root/name;source=path.read_text()
    if marker in source:return
    for old,new in changes:
        if source.count(old)!=1:raise RuntimeError(f'{name}: resource history anchor changed: {old[:100]}')
        source=source.replace(old,new)
    updates[path]='// '+marker+'\n'+source


edit('lib/gfx/frame_packet.hpp',[
    ('#pragma once','#pragma once\n#include "gekkoaot_scene_resource_history.h"'),
    ('  std::array<uint32_t,12> enhancedReasons{};',
     '  uint64_t provenancePass=0,provenanceColorParent=0,provenanceCopyResource=0;\n'
     '  uint32_t provenanceCopyRevision=0,provenanceUseCount=0,provenanceUseOverflow=0;\n'
     '  std::array<GekkoAOT::GX::SceneTextureUse,128> provenanceUses{};\n'
     '  std::array<uint32_t,12> enhancedReasons{};')])
edit('lib/gfx/recording.cpp',[
    ('void enhanced::record_classification(unsigned reason,unsigned composition) noexcept {',
     'void enhanced::record_texture_use(const GekkoAOT::GX::SceneTextureUse& use) noexcept {\n'
     '  if(!g_recorder.active()||g_recorder.currentRenderPass==UINT32_MAX)return;\n'
     '  auto& pass=current_render_passes()[g_recorder.currentRenderPass];\n'
     '  for(unsigned i=0;i<pass.provenanceUseCount;++i){auto& old=pass.provenanceUses[i];\n'
     '    if(old.resource==use.resource && old.revision==use.revision && old.producer==use.producer){\n'
     '      ++old.draws;old.maps|=use.maps;return;}\n'
     '  }\n'
     '  if(pass.provenanceUseCount<pass.provenanceUses.size())pass.provenanceUses[pass.provenanceUseCount++]=use;\n'
     '  else ++pass.provenanceUseOverflow;\n'
     '}\n'
     'void enhanced::set_copy_provenance(uint64_t resource,uint32_t revision) noexcept {\n'
     '  if(!g_recorder.active()||g_recorder.currentRenderPass==UINT32_MAX)return;\n'
     '  auto& pass=current_render_passes()[g_recorder.currentRenderPass];\n'
     '  pass.provenanceCopyResource=resource;pass.provenanceCopyRevision=revision;\n'
     '}\n'
     'void enhanced::record_classification(unsigned reason,unsigned composition) noexcept {')])
edit('lib/dolphin/gx/GXFrameBuffer.cpp',[
    ('#include "../../gfx/recording.hpp"','#include "../../gfx/recording.hpp"\n#include "../../gfx/gekkoaot_enhanced_renderer.hpp"'),
    ('  gfx::resolve_pass_into(handle.handle, rect, clearColor, clearAlpha, clearDepth, g_gxState.clearColor,',
     '  gfx::enhanced::note_copy(handle.handle,handle.revision+1);\n'
     '  gfx::resolve_pass_into(handle.handle, rect, clearColor, clearAlpha, clearDepth, g_gxState.clearColor,')])
for path,source in updates.items():path.write_text(source)
