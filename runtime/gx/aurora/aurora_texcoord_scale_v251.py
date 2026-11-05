"""Apply GX SU scale to ordinary texture samples, including integer movie UVs."""
from pathlib import Path
import sys

path = Path(sys.argv[1]) / 'lib/gx/shader.cpp'
text = path.read_text()
marker = 'GEKKOAOT_SU_SAMPLE_SCALE_V251'
if marker not in text:
    before = '''      // No indirect texturing
      uvIn = fmt::format("tex{0}_uv", underlying(stage.texCoordId));'''
    after = '''      // GEKKOAOT_SU_SAMPLE_SCALE_V251
      // XF coordinates enter SU in normalized units, or in texel units when
      // GXSetTexCoordScaleManually selects scale=1 (retail Y/U/V movies).
      // SU multiplies by its per-coordinate scale before texture lookup;
      // WebGPU needs the resulting texels divided by this map's dimensions.
      uvIn = fmt::format("tex{0}_uv * ubuf.texcoord_scale[{0}].xy / ubuf.tex{1}_size_bias.xy",
                         underlying(stage.texCoordId), underlying(stage.texMapId));'''
    if text.count(before) != 1:
        raise RuntimeError(f'{path}: ordinary sample anchor changed')
    path.write_text(text.replace(before, after))
