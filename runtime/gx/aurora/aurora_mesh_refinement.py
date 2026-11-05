"""Lower validated refined meshes through the ordinary GX draw path."""
from pathlib import Path
import sys
root=Path(sys.argv[1]);marker='GEKKOAOT_MESH_REFINEMENT_V1'
p=root/'lib/gx/command_processor.cpp';s=p.read_text()
if marker not in s:
    a='#include "../gfx/gekkoaot_hd_resources.hpp"'
    assert s.count(a)==1
    s=s.replace(a,a+'\n#include "../gfx/gekkoaot_mesh_resources.hpp"')
    observer='  gfx::enhanced::begin_draw();\n  gfx::enhanced::observe_draw();\n  gfx::enhanced::observe_environment(vtxCount);'
    assert s.count(observer)==1
    s=s.replace(observer,'  if(!gfx::geometry::canonical_active()){\n'+observer+'\n  }')
    a='  const auto vertexData = reader.take(totalVtxBytes);\n  gfx::Range vertRange ='
    b='''  const auto vertexData = reader.take(totalVtxBytes);
  if(auto mesh=gfx::geometry::request(prim,fmt,vtxCount,{vertexData.data(),vertexData.size()})){
    const unsigned level=gfx::geometry::select_level(*mesh);
    if(level){
      const bool compare=gfx::geometry::comparison_enabled();
      if(compare){
        gfx::geometry::ComparisonScope originalHalf(1,true);
        ByteReader original{{vertexData.data(),vertexData.size()}};
        draw_prim(prim,fmt,vtxCount,original);
      }
      gfx::geometry::ComparisonScope refinedHalf(compare?2:0,false);
      gfx::geometry::FormatScope formatScope(*mesh);
      const auto& bytes=mesh->vertices[level-1];
      ByteReader refined{{bytes.data(),bytes.size()}};
      draw_prim(GX_TRIANGLES,GX_VTXFMT7,static_cast<u16>(mesh->counts[level-1]),refined);
      return;
    }
  }
  gfx::Range vertRange ='''
    assert s.count(a)==1
    s=s.replace(a,b)
    s=s.replace('      gfx::shadow::record_draw(*draw,cache.config);','      if(!gfx::geometry::comparison_original())gfx::shadow::record_draw(*draw,cache.config);')
    p.write_text('// '+marker+'\n'+s)
p=root/'lib/gx/gx.cpp';s=p.read_text()
if marker not in s:
    a='#include "../gfx/gekkoaot_hd_resources.hpp"';assert s.count(a)==1
    s=s.replace(a,a+'\n#include "../gfx/gekkoaot_mesh_resources.hpp"')
    a='  g_gxState.renderScissor = scissor;\n  gfx::set_scissor(scissor);';assert s.count(a)==1
    s=s.replace(a,'  const auto clipped=gfx::geometry::comparison_scissor(scissor);\n  g_gxState.renderScissor = clipped;\n  gfx::set_scissor(clipped);')
    a='void shutdown() noexcept {';assert s.count(a)==1
    s=s.replace(a,a+'\n  gfx::geometry::shutdown();')
    p.write_text('// '+marker+'\n'+s)
