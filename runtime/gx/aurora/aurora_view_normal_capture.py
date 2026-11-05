"""Keep Enhanced geometry normals in view space, independently of GX lighting space."""
from pathlib import Path
import sys
root=Path(sys.argv[1])
p=root/'lib/gx/shader.cpp'
s=p.read_text()
marker='GEKKOAOT_VIEW_NORMAL_CAPTURE_V1'
if marker not in s:
    anchor='  uniBufAttrs += "\\n    proj: mat4x4f,";'
    assert s.count(anchor)==1
    inject=r'''
  // GEKKOAOT_VIEW_NORMAL_CAPTURE_V1: GX normal matrices are for lighting and
  // may omit the camera rotation. Reconstruct a geometric normal using the
  // inverse transpose of the matching per-vertex position matrix instead.
  if (useNormalTarget) {
    vtxOutAttrs += fmt::format("\n    @location({}) capture_nrm: vec3f,", vtxOutIdx++);
    vtxXfrAttrsPre += fmt::format("\n    out.capture_nrm = gekkoaot_capture_normal({}, in_pnmtxidx);",
                                vtx_attr(config, GX_VA_NRM));
  }
'''
    s=s.replace(anchor,inject+anchor)
    anchor='fn gekkoaot_xf_nrm(v: vec4f, idx_raw: u32) -> vec3f {'
    assert s.count(anchor)==1
    helper=r'''
fn gekkoaot_capture_normal(n: vec3f, idx_raw: u32) -> vec3f {
    let i = idx_raw & 0x3fu;
    let a = ubuf.xf_pos_rows[i].xyz;
    let b = ubuf.xf_pos_rows[i + 1u].xyz;
    let c = ubuf.xf_pos_rows[i + 2u].xyz;
    let cof0 = cross(b, c);
    let det = dot(a, cof0);
    if (abs(det) < 1e-12) { return vec3f(0.); }
    let v = vec3f(dot(cof0, n), dot(cross(c, a), n), dot(cross(a, b), n)) / det;
    return v * inverseSqrt(max(dot(v, v), 1e-20));
}
'''
    s=s.replace(anchor,helper+anchor)
    for old,new in [('dot(in.mv_nrm, in.mv_nrm)','dot(in.capture_nrm, in.capture_nrm)'),('normalize(in.mv_nrm), nrm_len_sq','normalize(in.capture_nrm), nrm_len_sq')]:
        assert s.count(old)==2,old
        s=s.replace(old,new)
    p.write_text(s)
    p=root/'lib/gx/pipeline.hpp'
    s=p.read_text();assert 'GXPipelineConfigVersion = 23;' in s
    p.write_text(s.replace('GXPipelineConfigVersion = 23;','GXPipelineConfigVersion = 24;'))
