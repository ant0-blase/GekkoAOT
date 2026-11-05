"""Expose validated original-texture height fields to asynchronous mesh jobs."""
from pathlib import Path
import sys
p=Path(sys.argv[1])/'lib/gx/texture.cpp';s=p.read_text();marker='GEKKOAOT_DISPLACEMENT_V1'
if marker not in s:
 a='static void resolve_material_for_draw(const ShaderInfo& info) {'
 b='''// GEKKOAOT_DISPLACEMENT_V1
std::shared_ptr<const GekkoAOT::GX::Mesh::HeightField> displacement_height_for_draw(){
  if(g_gxState.numTexGens!=1 || g_gxState.numIndStages!=0 ||
     g_gxState.vtxDesc[GX_VA_PNMTXIDX]!=GX_NONE ||
     g_gxState.vtxDesc[GX_VA_TEX0MTXIDX]!=GX_NONE ||
     g_gxState.tevStages[0].texMapId!=GX_TEXMAP0 || g_gxState.tevStages[0].texCoordId!=GX_TEXCOORD0 ||
     g_gxState.tcgs[0].src!=GX_TG_TEX0 || g_gxState.tcgs[0].mtx!=GX_IDENTITY ||
     g_gxState.tcgs[0].postMtx!=GX_PTIDENTITY)return {};
  const auto& obj=g_gxState.loadedTextures[0];
  if(!obj.has_data() || g_gxState.copyTextures.contains(obj.data))return {};
  const GXTlutObj_* palette=nullptr;
  if(is_palette_format(obj.format())){
    if(obj.tlut>=g_gxState.loadedTluts.size())return {};
    palette=&g_gxState.loadedTluts[obj.tlut];if(!palette->data)return {};
  }
  const auto keys=hash_texture_source(obj,palette,true);if(!keys.sourceKey)return {};
  if(gfx::texture_replacement::find_pointer_replacement(obj) || gfx::texture_replacement::find_source_replacement(obj,*keys.sourceKey))return {};
  return gfx::hd::request_height(obj,*keys.sourceKey,palette);
}
'''+a
 assert s.count(a)==1;s=s.replace(a,b);p.write_text(s)
