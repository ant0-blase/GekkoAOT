"""Keep small emission statistics alongside the existing decoded texture cache."""
from pathlib import Path
import sys
root=Path(sys.argv[1]);marker='GEKKOAOT_PARTICLE_RADIANCE_V1'
p=root/'lib/gfx/texture.hpp';s=p.read_text()
if marker not in s:
    anchor='  bool hasArbitraryMips = false;'
    assert s.count(anchor)==1
    s=s.replace(anchor,anchor+'\n  std::array<float,4> particleRadiance{};\n  bool particleRadianceKnown=false;')
    p.write_text('// '+marker+'\n'+s)

# Inspect a resolved binding without populating/changing the texture cache
# before Aurora has selected the draw's actual source.
p=root/'lib/gx/texture.hpp';s=p.read_text()
if 'cached_particle_texture' not in s:
    anchor='gfx::TextureHandle resolve_static_texture(const GXTexObj_& obj);'
    assert s.count(anchor)==1
    p.write_text(s.replace(anchor,anchor+'\ngfx::TextureHandle cached_particle_texture(const GXTexObj_&, const GXTlutObj_* = nullptr);'))
p=root/'lib/gx/texture.cpp';s=p.read_text()
if 'cached_particle_texture' not in s:
    anchor='void invalidate_bindings() noexcept {'
    assert s.count(anchor)==1
    code='''gfx::TextureHandle cached_particle_texture(const GXTexObj_& obj,const GXTlutObj_* tlut) {
  const auto it=s_textureObjectCaches.find(obj.texObjId);
  if(it==s_textureObjectCaches.end() || !matches_source(it->second,obj))return {};
  const auto& entry=it->second;
  if(tlut && (entry.tlutObjId!=tlut->tlutObjId || entry.tlutDataVersion!=tlut->tlutDataVersion))return {};
  return entry.handle;
}
'''
    p.write_text(s.replace(anchor,code+anchor))
s=p.read_text()
forward='static bool matches_source(const CachedTextureEntry&, const GXTexObj_&);'
if forward not in s:
    p.write_text(s.replace('gfx::TextureHandle cached_particle_texture(',forward+'\ngfx::TextureHandle cached_particle_texture(',1))
p=root/'lib/gfx/texture.cpp';s=p.read_text()
if marker not in s:
    s='#include "gekkoaot_particle_radiance.hpp"\n'+s
    anchor='  uint32_t offset = 0;'
    assert s.count(anchor)==2
    s=s.replace(anchor,'  enhanced::capture_particle_texture(ref,data);\n'+anchor)
    p.write_text('// '+marker+'\n'+s)
