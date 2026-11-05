"""GX draw sub-phase probes (v389, diagnostic).

Marks the stages of Aurora's draw_prim/push_gx_draw for the bridge's
GEKKOAOT_GX_PHASE_PROFILE cycle accounting (gekkoaot_gx_phase_enter/leave,
defined in bridge.cpp). Disabled profiling costs one predictable branch per
probe.
"""
from pathlib import Path
import sys

root = Path(sys.argv[1])
MARKER = 'GEKKOAOT_GX_PHASE_PROBE_V389'
path = root / 'lib/gx/command_processor.cpp'
text = path.read_text()
if MARKER not in text:
    def rep(old, new):
        global text
        if text.count(old) != 1:
            raise RuntimeError('command_processor.cpp: phase probe anchor changed: ' + old[:60])
        text = text.replace(old, new)
    rep('static void push_gx_draw(GXPrimitive prim, GXVtxFmt fmt, u16 vtxCount, gfx::Range vertRange, gfx::Range idxRange,\n',
        '''// ''' + MARKER + ''': stage marks for the bridge's cycle accounting.
extern "C" unsigned gekkoaot_gx_phase_enter(unsigned phase);
extern "C" void gekkoaot_gx_phase_leave(unsigned saved);
namespace {
struct GxPhaseProbe {
  unsigned saved;
  explicit GxPhaseProbe(unsigned phase) : saved(gekkoaot_gx_phase_enter(phase)) {}
  ~GxPhaseProbe() { gekkoaot_gx_phase_leave(saved); }
};
} // namespace
static void push_gx_draw(GXPrimitive prim, GXVtxFmt fmt, u16 vtxCount, gfx::Range vertRange, gfx::Range idxRange,
''')
    rep('''  if (!pipelineValid) {
    const bool hadPipeline = cache.hasPipeline;''', '''  std::optional<GxPhaseProbe> probe(std::in_place, 13u);
  if (!pipelineValid) {
    const bool hadPipeline = cache.hasPipeline;''')
    rep('''  const size_t storageAlignment = gfx::storage_alignment();''', '''  probe.reset(); probe.emplace(14u);
  const size_t storageAlignment = gfx::storage_alignment();''')
    rep('''  if (gfx::enhanced::alias_bindings_stale()) texture::invalidate_bindings();''', '''  probe.reset(); probe.emplace(15u);
  if (gfx::enhanced::alias_bindings_stale()) texture::invalidate_bindings();''')
    rep('''  const bool uniformValid = (state.dirty & DirtyUniform) == 0 && cache.uniformRange.size != 0;''', '''  probe.reset(); probe.emplace(16u);
  const bool uniformValid = (state.dirty & DirtyUniform) == 0 && cache.uniformRange.size != 0;''')
    rep('''  state.dirty &= ~DirtyImmediates;

  uint32_t instanceCount = 1;''', '''  state.dirty &= ~DirtyImmediates;
  probe.reset(); probe.emplace(17u);

  uint32_t instanceCount = 1;''')
    rep('''  const auto vertexData = reader.take(totalVtxBytes);
  if(!gfx::geometry::canonical_active()){''', '''  const auto vertexData = reader.take(totalVtxBytes);
  std::optional<GxPhaseProbe> probe(std::in_place, 10u);
  if(!gfx::geometry::canonical_active()){''')
    rep('''  // Raw vertices were inspected before classifying/merging the draw.
  if(auto mesh=gfx::geometry::request(''', '''  probe.reset(); probe.emplace(11u);
  // Raw vertices were inspected before classifying/merging the draw.
  if(auto mesh=gfx::geometry::request(''')
    rep('''  gfx::Range vertRange = gfx::push_verts(vertexData.data(), vertexData.size(), canMerge ? 0 : 4);
''', '''  probe.reset(); probe.emplace(12u);
  gfx::Range vertRange = gfx::push_verts(vertexData.data(), vertexData.size(), canMerge ? 0 : 4);
  probe.reset();
''')
    rep('''    cache.bindGroups = build_bind_groups(cache.shaderInfo);''', '''    std::optional<GxPhaseProbe> groups(std::in_place, 21u);
    cache.bindGroups = build_bind_groups(cache.shaderInfo);
    groups.reset();''')
    if '#include <optional>\n' not in text:
        text = '#include <optional>\n' + text
    path.write_text(text)

path = root / 'lib/gx/texture.cpp'
text = path.read_text()
if MARKER not in text:
    def rep2(old, new):
        global text
        if text.count(old) != 1:
            raise RuntimeError('texture.cpp: phase probe anchor changed: ' + old[:60])
        text = text.replace(old, new)
    rep2('''void resolve_sampled_textures(const ShaderInfo& info) noexcept {
  ZoneScoped;
  apply_pending_invalidations();
''', '''// ''' + MARKER + ''': stage marks for the bridge's cycle accounting.
extern "C" unsigned gekkoaot_gx_phase_enter(unsigned phase);
extern "C" void gekkoaot_gx_phase_leave(unsigned saved);
namespace {
struct GxPhaseProbe {
  unsigned saved;
  explicit GxPhaseProbe(unsigned phase) : saved(gekkoaot_gx_phase_enter(phase)) {}
  ~GxPhaseProbe() { gekkoaot_gx_phase_leave(saved); }
};
} // namespace
void resolve_sampled_textures(const ShaderInfo& info) noexcept {
  ZoneScoped;
  {
    GxPhaseProbe inval(18u);
    apply_pending_invalidations();
  }
''')
    rep2('''    gfx::TextureHandle handle;
    const auto copyIt = g_gxState.copyTextures.find(obj.data);''', '''    std::optional<GxPhaseProbe> slow(std::in_place, 22u);
    gfx::TextureHandle handle;
    const auto copyIt = g_gxState.copyTextures.find(obj.data);''')
    rep2('''    } else if (obj.has_data()) {
      handle = texture::resolve_static_texture(obj);
    }
''', '''    } else if (obj.has_data()) {
      slow.reset(); slow.emplace(19u);
      handle = texture::resolve_static_texture(obj);
    }
''')
    rep2('''    textureBind = gfx::TextureBind{obj, std::move(handle), s_bindGeneration};
  }
  resolve_material_for_draw(info);''', '''    textureBind = gfx::TextureBind{obj, std::move(handle), s_bindGeneration};
  }
  GxPhaseProbe material(20u);
  resolve_material_for_draw(info);''')
    if '#include <optional>\n' not in text:
        text = '#include <optional>\n' + text
    path.write_text(text)

path = root / 'lib/gx/texture.cpp'
text = path.read_text()
TOTALS = 'GEKKOAOT_TEXTURE_TOTALS_V389'
if TOTALS not in text:
    old = '''  TracyPlot("aurora: textureReplacementPublishBytes", static_cast<int64_t>(s_stats.publishBytes));

  s_stats = {};'''
    if text.count(old) != 1:
        raise RuntimeError('texture.cpp: end_frame stats anchor changed')
    text = text.replace(old, '''  TracyPlot("aurora: textureReplacementPublishBytes", static_cast<int64_t>(s_stats.publishBytes));

  // ''' + TOTALS + ''': running totals for the bridge's phase report.
  s_totalStats[0] += s_stats.objectHits;
  s_totalStats[1] += s_stats.contentHits;
  s_totalStats[2] += s_stats.misses;
  s_totalStats[3] += s_stats.hashedBytes;
  s_stats = {};''')
    old = 'const TextureStats& texture_stats() noexcept { return s_stats; }\n'
    if text.count(old) != 1:
        raise RuntimeError('texture.cpp: texture_stats anchor changed')
    text = text.replace(old, old + '''} // namespace aurora::gx
extern "C" void gekkoaot_texture_totals_v389(uint64_t* out) {
  for (int i = 0; i < 4; ++i) out[i] = aurora::gx::s_totalStats[i];
}
namespace aurora::gx {
''')
    old = 'TextureStats s_stats;\n'
    if text.count(old) != 1:
        raise RuntimeError('texture.cpp: s_stats anchor changed')
    text = text.replace(old, old + 'uint64_t s_totalStats[4] = {};\n')
    path.write_text(text)
