"""Enable Aurora's GX lighting equations per fragment on eligible enhanced draws.

No extra light is inferred from light slots. Original rendering stays vertex-lit.
All file anchors are checked before writing the composed Aurora checkout.
"""
from pathlib import Path
import sys

root = Path(sys.argv[1])
marker = 'GEKKOAOT_FRAGMENT_LIGHTING_V1'
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
            raise RuntimeError(f'{name}: fragment lighting anchor changed: {old[:100]}')
        source = source.replace(old, new)
    updates[path] = '// ' + marker + '\n' + source

edit('lib/gx/gx.hpp', [('  u8 pad1 : 1 = 0;', '  u8 fragmentLighting : 1 = false;')])
edit('lib/gx/gx.cpp', [
    ('  config.shaderConfig.enhancedEligible = gfx::enhanced::eligible();',
     '  config.shaderConfig.enhancedEligible = gfx::enhanced::eligible();\n'
     '  config.shaderConfig.fragmentLighting = gfx::enhanced::fragment_lighting() &&\n'
     '      config.shaderConfig.enhancedEligible && g_gxState.vtxDesc[GX_VA_NRM] != GX_NONE &&\n'
     '      primitive != GX_LINES && primitive != GX_LINESTRIP && primitive != GX_POINTS;')])
edit('lib/gx/command_processor.cpp', [
    ('  hash.update(&enhanced,sizeof(enhanced)); hash.update(&eligible,sizeof(eligible));',
     '  hash.update(&enhanced,sizeof(enhanced)); hash.update(&eligible,sizeof(eligible));\n'
     '  const bool fragmentLighting=gfx::enhanced::fragment_lighting();\n'
     '  hash.update(&fragmentLighting,sizeof(fragmentLighting));')])
edit('lib/gx/pipeline.hpp', [('GXPipelineConfigVersion = 20;', 'GXPipelineConfigVersion = 21;')])
edit('lib/gx/shader.cpp', [
    ('UsePerPixelLighting', 'config.fragmentLighting', 10),
    ('"in.mv_nrm"', '"fragment_normal"', 1),
    ('dot(ldir, in.mv_nrm)', 'dot(ldir, fragment_normal)', 2),
    ('    if (config.fragmentLighting) {\n      vtxOutAttrs += fmt::format("\\n    @location({}) mv_pos: vec3f,", vtxOutIdx++);',
     '    if (config.fragmentLighting) {\n'
     '      fragmentFnPre += "\\n    let fragment_normal = in.mv_nrm * inverseSqrt(max(dot(in.mv_nrm,in.mv_nrm),1e-20));";\n'
     '      vtxOutAttrs += fmt::format("\\n    @location({}) mv_pos: vec3f,", vtxOutIdx++);'),
    ('          ldir = ldir / dist;',
     '          ldir = ldir / {10};'),
    ('                     alpha ? "a"sv : ""sv);',
     '                     alpha ? "a"sv : ""sv, config.fragmentLighting ? "max(dist, 1e-10)"sv : "dist"sv);')])

for path, source in updates.items():
    path.write_text(source)
