"""Classify camera-centred background before pipeline/uniform selection."""
from pathlib import Path
import sys
p=Path(sys.argv[1])/'lib/gx/command_processor.cpp';s=p.read_text()
if 'GEKKOAOT_BACKGROUND_GEOMETRY_V1' not in s:
 old='''  if(!gfx::geometry::canonical_active()){
  gfx::enhanced::begin_draw();
  gfx::enhanced::observe_draw();
  gfx::enhanced::observe_environment(vtxCount);
  }
'''
 assert s.count(old)==1;s=s.replace(old,'')
 anchor='  const bool nativeBatch = native_semantic_batch_enabled();'
 assert s.count(anchor)==1
 s=s.replace(anchor,'''  const auto vertexData = reader.take(totalVtxBytes);
  if(!gfx::geometry::canonical_active()){
    gfx::enhanced::begin_draw();
    gfx::enhanced::observe_geometry(prim,fmt,vtxCount,{vertexData.data(),vertexData.size()});
    gfx::enhanced::observe_draw();
    gfx::enhanced::observe_environment(vtxCount);
  }
'''+anchor)
 # Original later read is removed; classification also precedes canMerge.
 anchor='  // Push raw vertex data to buffer. Merged draws must remain contiguous with the previous range.\n  const auto vertexData = reader.take(totalVtxBytes);'
 assert s.count(anchor)==1;s=s.replace(anchor,'  // Raw vertices were inspected before classifying/merging the draw.')
 anchor='      gfx::enhanced::begin_draw();\n      push_gx_draw('
 assert s.count(anchor)==1;s=s.replace(anchor,'      gfx::enhanced::begin_draw();\n      gfx::enhanced::observe_geometry(prim,fmt,vtxCount,{vertexData.data(),vertexData.size()});\n      push_gx_draw(')
 p.write_text('// GEKKOAOT_BACKGROUND_GEOMETRY_V1\n'+s)

# GEKKOAOT_BACKGROUND_GEOMETRY_V2: primitive topology is needed for emitters.
s=p.read_text()
s=s.replace('observe_geometry(fmt,vtxCount','observe_geometry(prim,fmt,vtxCount')
p.write_text(s)
