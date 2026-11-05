"""Protect every covered HUD texel, including translucent glyph edges, from DOF.

Only derived scene metadata changes. Original GX color, alpha and depth remain
untouched. The shared draw reason is valid outside the diagnostic coverage view.
"""
from pathlib import Path
import sys

root=Path(sys.argv[1])
shader=root/'lib/gx/shader.cpp'
s=shader.read_text()
marker='GEKKOAOT_LENS_INTERFACE_V1'
if marker not in s:
    old='"let metadata_write=select(0.0,1.0,"+replace+");\\n"'
    new='"let lens_interface=(ubuf.enhanced_view.y==4u || ubuf.enhanced_view.y==11u) && gekkoaot_alpha>0.001;\\n"\n' \
        '      "let metadata_write=select(0.0,1.0,("+replace+") || lens_interface);\\n"'
    assert s.count(old)==1
    shader.write_text('// '+marker+'\n'+s.replace(old,new))

# Old pipeline-cache entries contain the previous fragment metadata policy.
p=root/'lib/gx/pipeline.hpp'
s=p.read_text()
if marker not in s:
    old='GXPipelineConfigVersion = 27;'
    assert s.count(old)==1
    p.write_text('// '+marker+'\n'+s.replace(old,'GXPipelineConfigVersion = 28;'))

# Additive sunlight/flare quads contribute colour to the existing scene. Their
# rectangular raster bounds are not an opaque HUD surface: keep the receiver
# beneath them, including where a flare texture has nonzero alpha everywhere.
# Alpha-over glyphs still invalidate metadata at every covered texel.
marker2='GEKKOAOT_LENS_CONTRIBUTION_V2'
s=shader.read_text()
if marker2 not in s:
    old='"let lens_interface=(ubuf.enhanced_view.y==4u || ubuf.enhanced_view.y==11u) && gekkoaot_alpha>0.001;\\n"'
    new='"let lens_interface=(ubuf.enhanced_view.y==4u || ubuf.enhanced_view.y==11u) && gekkoaot_alpha>0.001 && "+std::string(mode==2u || mode==3u || mode==5u ? "false" : "true")+";\\n"'
    assert s.count(old)==1
    shader.write_text('// '+marker2+'\n'+s.replace(old,new))
p=root/'lib/gx/pipeline.hpp'
s=p.read_text()
if marker2 not in s:
    assert s.count('GXPipelineConfigVersion = 28;')==1
    p.write_text('// '+marker2+'\n'+s.replace('GXPipelineConfigVersion = 28;', 'GXPipelineConfigVersion = 29;'))
