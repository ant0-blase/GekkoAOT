"""Reconstruct transient images directly in the normal GX sample path."""
from pathlib import Path
import sys
root=Path(sys.argv[1]);marker='GEKKOAOT_DYNAMIC_TEXTURE_V1'
p=root/'lib/gx/shader.cpp';s=p.read_text()
if marker not in s:
 a='#include "../gfx/gekkoaot_material_shaders.hpp"';assert s.count(a)==1
 s=s.replace(a,a+'\n#include "../gfx/dynamic_texture_shaders.h"')
 a='  if(info.materialEnhancement){uniBufAttrs+="\\n    material_params:vec4f,";texBindings+=gfx::hd::MaterialShader;}'
 assert s.count(a)==1;s=s.replace(a,'  texBindings+=gfx::hd::DynamicTextureShader;\n'+a)
 a='var sampled{0} = textureSampleBias(tex{1}, tex{1}_samp, {2}, ubuf.tex{1}_size_bias.z);'
 assert s.count(a)==1;s=s.replace(a,'var sampled{0} = hd_dynamic_sample(tex{1}, tex{1}_samp, {2}, ubuf.tex{1}_size_bias.z, ubuf.tex{1}_size_bias.w);')
 p.write_text('// '+marker+'\n'+s)
p=root/'lib/gx/shader_info.cpp';s=p.read_text()
if marker not in s:
 a='return {width, height, tex.texObj.lod_bias() + vpBias, 0.0f};';assert s.count(a)==1
 s=s.replace(a,'return {width, height, tex.texObj.lod_bias() + vpBias, tex.ref && !tex.ref->isReplacement ? float(gfx::hd::reconstruction_scale()) : 0.f};')
 p.write_text('// '+marker+'\n'+s)
p=root/'lib/gx/pipeline.hpp';s=p.read_text()
if marker not in s:
 a='GXPipelineConfigVersion = 25;';assert s.count(a)==1;s=s.replace(a,'GXPipelineConfigVersion = 26;');p.write_text('// '+marker+'\n'+s)
p=root/'lib/gx/texture.cpp';s=p.read_text()
if marker not in s:
 a='if (entry.handle && matches_source(entry, obj) &&';assert s.count(a)==2
 s=s.replace(a,'if (entry.handle && !gfx::hd::needs_observation(obj) && matches_source(entry, obj) &&')
 a='      if (matches) {';assert s.count(a)==1;s=s.replace(a,'      if (matches && !gfx::hd::needs_observation(obj)) {')
 p.write_text('// '+marker+'\n'+s)

# V2: cover transient color even when the draw has a material/POM binding.
p=root/'lib/gx/shader.cpp';s=p.read_text()
if 'GEKKOAOT_DYNAMIC_TEXTURE_V2' not in s:
 a='var sampled{}=textureSampleGrad(tex0,tex0_samp,material_uv,'
 b='material_dx*exp2(ubuf.tex0_size_bias.z),material_dy*exp2(ubuf.tex0_size_bias.z));'
 assert s.count(a)==1 and s.count(b)==1
 s=s.replace(a,'var sampled{}=hd_dynamic_sample_grad(tex0,tex0_samp,material_uv,').replace(b,'material_dx*exp2(ubuf.tex0_size_bias.z),material_dy*exp2(ubuf.tex0_size_bias.z),ubuf.tex0_size_bias.w);')
 p.write_text('// GEKKOAOT_DYNAMIC_TEXTURE_V2\n'+s)
p=root/'lib/gx/pipeline.hpp';s=p.read_text()
if 'GEKKOAOT_DYNAMIC_TEXTURE_V2' not in s:
 assert s.count('GXPipelineConfigVersion = 26;')==1
 p.write_text('// GEKKOAOT_DYNAMIC_TEXTURE_V2\n'+s.replace('GXPipelineConfigVersion = 26;','GXPipelineConfigVersion = 27;'))
