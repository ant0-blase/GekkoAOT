"""Use actual shader texture dependencies, including merged and indirect draws."""
from pathlib import Path
import sys
root=Path(sys.argv[1]);marker='GEKKOAOT_DRAW_PROVENANCE_V1';updates={}
def edit(name,changes):
    path=root/name;source=path.read_text()
    if marker in source:return
    for old,new in changes:
        if source.count(old)!=1:raise RuntimeError(f'{name}: draw provenance anchor changed: {old[:100]}')
        source=source.replace(old,new)
    updates[path]='// '+marker+'\n'+source
edit('lib/gx/command_processor.cpp',[
    ('  auto& cache = sDrawCache;\n  gfx::enhanced::observe_draw();',
     '  auto& cache = sDrawCache;'),
    ('static void draw_prim(GXPrimitive prim, GXVtxFmt fmt, u16 vtxCount, ByteReader& reader) noexcept {\n  ZoneScoped;',
     'static void draw_prim(GXPrimitive prim, GXVtxFmt fmt, u16 vtxCount, ByteReader& reader) noexcept {\n  ZoneScoped;\n'
     '  gfx::enhanced::observe_draw();'),
    ('  // GEKKOAOT_UI_SCISSOR_V257\n  update_widescreen_scissor(cache.shaderInfo);',
     '  gfx::enhanced::record_draw_resources((cache.shaderInfo.sampledTextures | cache.shaderInfo.sampledIndTextures).to_ulong());\n'
     '  // GEKKOAOT_UI_SCISSOR_V257\n  update_widescreen_scissor(cache.shaderInfo);'),
    ('  if (canMerge) {\n    u32 numIndices = 0;',
     '  if (canMerge) {\n'
     '    gfx::enhanced::record_draw_resources((sDrawCache.shaderInfo.sampledTextures | sDrawCache.shaderInfo.sampledIndTextures).to_ulong());\n'
     '    u32 numIndices = 0;')])
for path,source in updates.items():path.write_text(source)
