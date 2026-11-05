"""GX SRTG texture coordinates consume XF lighting results, not raw colors."""
from pathlib import Path
import sys
root=Path(sys.argv[1]);marker='GEKKOAOT_COLOR_TEXGEN_V377'
p=root/'lib/gx/shader_info.cpp';s=p.read_text()
if marker not in s:
 a='  info.uniformSize += info.loadsTevReg.count() * sizeof(Vec4<float>);'
 b='''  // GEKKOAOT_COLOR_TEXGEN_V377: lighting may be read only by texgen.
  for(unsigned coord=0;coord<MaxTexCoord;++coord) {
    if(!info.sampledTexCoords.test(coord))continue;
    const auto source=config.tcgs[coord].src;
    if(source==GX_TG_COLOR0 || source==GX_TG_COLOR1)
      info.sampledColorChannels.set(source==GX_TG_COLOR0?0u:1u);
  }
'''+a
 assert s.count(a)==1;s=s.replace(a,b);p.write_text(s)
p=root/'lib/gx/shader.cpp';s=p.read_text()
if marker not in s:
 a='  for (int i = 0; i < info.sampledKColors.size(); ++i) {\n    if (info.sampledKColors.test(i)) {\n      uniBufAttrs +='
 assert s.count(a)==1
 b='''  // GEKKOAOT_COLOR_TEXGEN_V377: SRTG runs in XF, even when TEV
  // lighting is evaluated per fragment. Keep that original vertex result.
  std::array<bool,2> colorTexgen{};
  for(unsigned coord=0;coord<MaxTexCoord;++coord) {
    if(!info.sampledTexCoords.test(coord))continue;
    const auto source=config.tcgs[coord].src;
    if(source==GX_TG_COLOR0 || source==GX_TG_COLOR1)
      colorTexgen[source==GX_TG_COLOR0?0u:1u]=true;
  }
  if(config.fragmentLighting) {
    auto vertexConfig=config;vertexConfig.fragmentLighting=false;
    for(unsigned channel=0;channel<2;++channel)if(colorTexgen[channel]) {
      vtxXfrAttrs+=fmt::format("\\n    var texgen_cc{}: vec4f;",channel);
      auto lighting=lighting_func(vertexConfig,config.colorChannels[channel],channel,false);
      lighting+=lighting_func(vertexConfig,config.colorChannels[channel+GX_ALPHA0],channel,true);
      const auto from=fmt::format("out.cc{}",channel),to=fmt::format("texgen_cc{}",channel);
      for(std::size_t at=0;(at=lighting.find(from,at))!=std::string::npos;at+=to.size())lighting.replace(at,from.size(),to);
      vtxXfrAttrs+=lighting;
    }
  }
'''+a
 s=s.replace(a,b)
 for n in (0,1):
  a=f'vtxXfrAttrs += fmt::format("\\n    var tc{{}} = {{}};", i, vtx_attr(config, GX_VA_CLR{n}));'
  b=f'vtxXfrAttrs += fmt::format("\\n    var tc{{}} = {{}};", i, config.fragmentLighting ? "texgen_cc{n}" : "out.cc{n}");'
  assert s.count(a)==1;s=s.replace(a,b)
 s=s.replace('    if (tcg.normalize) {','    if (tcg.normalize && tcg.type != GX_TG_SRTG) {')
 s=s.replace('    if (tcg.postMtx == GX_PTIDENTITY) {','    if (tcg.postMtx == GX_PTIDENTITY || tcg.type == GX_TG_SRTG) {')
 p.write_text(s)
p=root/'lib/gx/pipeline.hpp' ;s=p.read_text()
if marker not in s:
 assert 'GXPipelineConfigVersion = 29;' in s
 p.write_text('// '+marker+'\n'+s.replace('GXPipelineConfigVersion = 29;','GXPipelineConfigVersion = 30;'))
