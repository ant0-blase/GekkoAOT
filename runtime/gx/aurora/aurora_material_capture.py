"""Capture GX reflection evidence beside octahedral normals, without another MRT.

GX does not expose PBR materials. Only a sampled specular color channel or a
sampled normal-based environment texgen marks a reflective receiver. Bright
diffuse textures alone provide no such evidence. Original GX output is intact.
"""
from pathlib import Path
import sys

root = Path(sys.argv[1])
marker = 'GEKKOAOT_MATERIAL_CAPTURE_V1'
updates = {}

def edit(name, changes):
    path = root / name
    source = path.read_text()
    if marker in source:
        return
    for change in changes:
        old, new = change[:2]
        count = change[2] if len(change) > 2 else 1
        if source.count(old) != count:
            raise RuntimeError(f'{name}: material capture anchor changed: {old[:100]}')
        source = source.replace(old, new)
    updates[path] = '// ' + marker + '\n' + source

edit('lib/gx/pipeline.hpp', [
    ('GXPipelineConfigVersion = 22;', 'GXPipelineConfigVersion = 23;')])
edit('lib/gx/shader.cpp', [
    ('    std::string code="let capture_valid=',
     '    unsigned material=1; // no GX reflection evidence (diffuse/unknown)\n'
     '    for (unsigned i=0;i<info.sampledColorChannels.size();++i)\n'
     '      if(info.sampledColorChannels[i] && config.colorChannels[i].lightingEnabled &&\n'
     '         config.colorChannels[i].attnFn==GX_AF_SPEC) material=2;\n'
     '    for (unsigned i=0;i<info.sampledTexCoords.size();++i)\n'
     '      if(info.sampledTexCoords[i] && config.tcgs[i].src==GX_TG_NRM &&\n'
     '         (config.tcgs[i].type==GX_TG_MTX2x4 || config.tcgs[i].type==GX_TG_MTX3x4)) material=3;\n'
     '    std::string code="let capture_valid='),
    ('    code+=mode==3?',
     '    code+="let gx_normal=out.normal.xyz*2.-1.;\\n"\n'
     '      "var oct=vec2f(.5);\\n"\n'
     '      "if(dot(gx_normal,gx_normal)>.25){\\n"\n'
     '      " let v=gx_normal/(abs(gx_normal.x)+abs(gx_normal.y)+abs(gx_normal.z));\\n"\n'
     '      " oct=v.xy; if(v.z<0.){oct=(1.-abs(oct.yx))*select(vec2f(-1.),vec2f(1.),oct>=vec2f(0.));}\\n"\n'
     '      " oct=oct*.5+.5;\\n}\\n"\n'
     '      "let material_code=("+std::to_string(material)+".+select(0.,4.,dot(gx_normal,gx_normal)>.25))/7.;\\n"\n'
     '      "out.normal=vec4f(oct,material_code,out.normal.w);\\n";\n'
     '    code+=mode==3?')])

for path, source in updates.items():
    path.write_text(source)
