"""Explain enhanced scene-capture coverage per pixel (GEKKOAOT_ENHANCED_VIEW=coverage).

A draw that replaces scene metadata without becoming a receiver stores its
rejection code instead of depth. Normal rendering is unchanged: the code is
written only when the per-draw uniform enables the coverage view.
Applied after the capture/contribution overlays; every anchor is preflighted.
"""
from pathlib import Path
import sys

root = Path(sys.argv[1])
marker = 'GEKKOAOT_CAPTURE_DIAGNOSTICS_V1'
updates = {}


def edit(name, changes):
    path = root / name
    source = path.read_text()
    if marker in source:
        return
    for old, new in changes:
        if source.count(old) != 1:
            raise RuntimeError(f'{name}: capture diagnostics anchor changed: {old[:100]} ({source.count(old)})')
        source = source.replace(old, new)
    updates[path] = '// ' + marker + '\n' + source


edit('lib/gx/shader_info.cpp', [(
    'buf.append(Vec4<uint32_t>{gfx::enhanced::current_view_id(),0,0,0});',
    'buf.append(Vec4<uint32_t>{gfx::enhanced::current_view_id(),gfx::enhanced::diagnostic_reason(),\n'
    '    gfx::enhanced::diagnostic_coverage(),gfx::enhanced::shadow_suppression()});')])
edit('lib/gx/shader.cpp', [(
    '  if(config.enhancementMask)fragmentFn += "\\n    let gekkoaot_alpha=prev.a;";\n',
    '  // Game shadows under sun cascades: dark alpha-over fragments fade with\n'
    '  // the per-draw suppression (0 for HUD, offscreen and other blends),\n'
    '  // BEFORE their alpha is recorded: the faded decal must not keep hiding\n'
    '  // the receivers underneath from the enhanced composition.\n'
    '  if(config.enhancementMask && config.enhancedComposition==1)fragmentFn +=\n'
    '    "\\n    if(ubuf.enhanced_view.w>0u && prev.a<0.95){prev.a*=1.0-f32(ubuf.enhanced_view.w)*0.001*"\n'
    '    "(1.0-smoothstep(0.1,0.35,dot(prev.rgb,vec3f(0.299,0.587,0.114))));}";\n'
    '  if(config.enhancementMask && config.enhancedComposition==4)fragmentFn +=\n'
    '    "\\n    if(ubuf.enhanced_view.w>0u && dot(prev.rgb,vec3f(0.299,0.587,0.114))<0.3){discard;}";\n'
    '  if(config.enhancementMask)fragmentFn += "\\n    let gekkoaot_alpha=prev.a;";\n'),(
    '      "out.normal.w=metadata_write;\\n";\n',
    '      "out.normal.w=metadata_write;\\n";\n'
    '    // Coverage view only: keep the reason a pixel is not a receiver. A\n'
    '    // translucent fragment of a receiver draw is shown although it normally\n'
    '    // keeps the receiver underneath it.\n'
    '    code+=std::string("if(!capture_valid && ubuf.enhanced_view.z>0u){\\n")+\n'
    '      (config.enhancedEligible?(config.enhancedAlphaBlend?\n'
    '        "  let reason=select(select(15u,14u,gekkoaot_alpha<0.98),13u,ubuf.enhanced_view.x==0u);\\n":\n'
    '        "  let reason=select(15u,13u,ubuf.enhanced_view.x==0u);\\n"):\n'
    '        "  let reason=ubuf.enhanced_view.y;\\n")+\n'
    '      "  if(reason>0u && (metadata_write>0.5 || reason==14u)){out.capture=vec4f(f32(reason),"\n'
    '      "f32(u32(round(clamp(gekkoaot_alpha,0.0,1.0)*1023.0))),0.0,1.0);out.normal.w=1.0;}\\n}\\n";\n')])
for path, source in updates.items():
    path.write_text(source)
