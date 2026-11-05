#!/usr/bin/env python3
import pathlib
import sys

MARKER = "GEKKOAOT_AURORA_PIPELINE_MULTISTATE_V161"


def fail(msg: str) -> None:
    print(f"[gekkoaot-v161] {msg}", file=sys.stderr)
    raise SystemExit(1)


def find_matching_brace(text: str, open_brace: int) -> int:
    depth = 0
    in_string = False
    in_char = False
    escape = False
    line_comment = False
    block_comment = False
    i = open_brace
    while i < len(text):
        c = text[i]
        n = text[i + 1] if i + 1 < len(text) else ""
        if line_comment:
            if c == "\n":
                line_comment = False
            i += 1
            continue
        if block_comment:
            if c == "*" and n == "/":
                block_comment = False
                i += 2
                continue
            i += 1
            continue
        if in_string:
            if escape:
                escape = False
            elif c == "\\":
                escape = True
            elif c == '"':
                in_string = False
            i += 1
            continue
        if in_char:
            if escape:
                escape = False
            elif c == "\\":
                escape = True
            elif c == "'":
                in_char = False
            i += 1
            continue
        if c == "/" and n == "/":
            line_comment = True
            i += 2
            continue
        if c == "/" and n == "*":
            block_comment = True
            i += 2
            continue
        if c == '"':
            in_string = True
        elif c == "'":
            in_char = True
        elif c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0:
                return i
        i += 1
    return -1


def main() -> None:
    if len(sys.argv) != 2:
        fail("usage: aurora_pipeline_multistate_v161.py <Aurora source root>")
    root = pathlib.Path(sys.argv[1])
    path = root / "lib/gx/command_processor.cpp"
    if not path.is_file():
        fail(f"missing {path}")

    text = path.read_text(encoding="utf-8")
    if MARKER in text:
        print(f"{MARKER}=1 status=already-installed")
        return

    if '#include "../gfx/hash.hpp"' not in text:
        needle = '#include "../gfx/depth_peek.hpp"\n'
        if needle not in text:
            fail("cannot find depth_peek include anchor")
        text = text.replace(needle, needle + '#include "../gfx/hash.hpp"\n', 1)

    if '#include <cstdlib>' not in text:
        needle = '#include <cstdint>\n'
        if needle not in text:
            fail("cannot find <cstdint> include anchor")
        text = text.replace(needle, needle + '#include <cstdlib>\n', 1)

    anchor = "DrawCache sDrawCache;\n"
    if anchor not in text:
        fail("cannot find DrawCache anchor")

    cache_code = r'''

// GEKKOAOT_AURORA_PIPELINE_MULTISTATE_V161
// Direct-mapped cache of derived GX pipeline state. A hash collision is only
// a miss: the complete 64-bit semantic key and target layout are checked
// before any PipelineConfig/ShaderInfo/PipelineRef is reused.
struct PipelineStateCacheEntry {
  uint64_t key = 0;
  uint64_t targetLayoutKey = 0;
  PipelineConfig config{};
  ShaderInfo shaderInfo{};
  gfx::PipelineRef pipelineRef{};
  bool valid = false;
};
constexpr size_t PipelineStateCacheSlots = 64;
static_assert((PipelineStateCacheSlots & (PipelineStateCacheSlots - 1u)) == 0u);
std::array<PipelineStateCacheEntry, PipelineStateCacheSlots> sPipelineStateCache{};
uint64_t sPipelineStateCacheHits = 0;
uint64_t sPipelineStateCacheMisses = 0;

bool pipeline_state_cache_enabled() noexcept {
  static const bool enabled = [] {
    const char* value = std::getenv("GEKKOAOT_AURORA_PIPELINE_STATE_CACHE");
    return value == nullptr || !(value[0] == '0' && value[1] == '\0');
  }();
  return enabled;
}

uint64_t pipeline_state_key(GXPrimitive prim, GXVtxFmt fmt, uint64_t targetLayoutKey) noexcept {
  const auto& state = g_gxState;
  aurora::Hasher hash{0x4745584b4f563136ull};
  const u8 lineMode = prim == GX_LINES ? 1u : prim == GX_LINESTRIP ? 2u : prim == GX_POINTS ? 3u : 0u;
  hash.update(&targetLayoutKey, sizeof(targetLayoutKey));
  hash.update(&fmt, sizeof(fmt));
  hash.update(&lineMode, sizeof(lineMode));
  hash.update(&state.vtxDesc, sizeof(state.vtxDesc));
  hash.update(&state.vtxFmts[fmt], sizeof(state.vtxFmts[fmt]));
  for (int i = GX_VA_POS; i <= GX_VA_TEX7; ++i) {
    hash.update(&state.arrays[i].stride, sizeof(state.arrays[i].stride));
    hash.update(&state.arrays[i].le, sizeof(state.arrays[i].le));
  }
  hash.update(&state.fog.type, sizeof(state.fog.type));
  hash.update(&state.fog.rangeEnabled, sizeof(state.fog.rangeEnabled));
  hash.update(&state.tevSwapTable, sizeof(state.tevSwapTable));
  hash.update(&state.numTevStages, sizeof(state.numTevStages));
  if (state.numTevStages != 0)
    hash.update(state.tevStages.data(), sizeof(state.tevStages[0]) * state.numTevStages);
  hash.update(&state.zTexType, sizeof(state.zTexType));
  hash.update(&state.zTexOp, sizeof(state.zTexOp));
  hash.update(&state.numIndStages, sizeof(state.numIndStages));
  if (state.numIndStages != 0)
    hash.update(state.indStages.data(), sizeof(state.indStages[0]) * state.numIndStages);
  hash.update(&state.colorChannelConfig, sizeof(state.colorChannelConfig));
  hash.update(&state.numTexGens, sizeof(state.numTexGens));
  if (state.numTexGens != 0)
    hash.update(state.tcgs.data(), sizeof(state.tcgs[0]) * state.numTexGens);
  hash.update(&state.alphaCompare, sizeof(state.alphaCompare));
  hash.update(&state.cullMode, sizeof(state.cullMode));
  hash.update(&state.frontOffset, sizeof(state.frontOffset));
  hash.update(&state.frontScale, sizeof(state.frontScale));
  hash.update(&state.backOffset, sizeof(state.backOffset));
  hash.update(&state.backScale, sizeof(state.backScale));
  hash.update(&state.clamp, sizeof(state.clamp));
  hash.update(&state.depthFunc, sizeof(state.depthFunc));
  hash.update(&state.blendMode, sizeof(state.blendMode));
  hash.update(&state.blendFacSrc, sizeof(state.blendFacSrc));
  hash.update(&state.blendFacDst, sizeof(state.blendFacDst));
  hash.update(&state.blendOp, sizeof(state.blendOp));
  hash.update(&state.dstAlpha, sizeof(state.dstAlpha));
  hash.update(&state.depthCompare, sizeof(state.depthCompare));
  hash.update(&state.depthUpdate, sizeof(state.depthUpdate));
  hash.update(&state.alphaUpdate, sizeof(state.alphaUpdate));
  hash.update(&state.colorUpdate, sizeof(state.colorUpdate));
  return hash.digest();
}
'''
    text = text.replace(anchor, anchor + cache_code, 1)

    # Replace only the pipeline-invalid rebuild block. This is deliberately
    # structural rather than line/context based so it composes with the other
    # GekkoAOT Aurora patches already installed by the controller.
    start = text.find("  if (!pipelineValid) {")
    if start < 0:
        fail("cannot find pipelineValid rebuild block")
    open_brace = text.find("{", start)
    end = find_matching_brace(text, open_brace)
    if end < 0:
        fail("cannot match pipelineValid rebuild braces")

    replacement = r'''  if (!pipelineValid) {
    const bool hadPipeline = cache.hasPipeline;
    const auto prevSampledTextures = cache.shaderInfo.sampledTextures;
    const auto prevSampledIndTextures = cache.shaderInfo.sampledIndTextures;

    bool stateCacheHit = false;
    uint64_t stateKey = 0;
    PipelineStateCacheEntry* stateEntry = nullptr;
    if (pipeline_state_cache_enabled()) {
      stateKey = pipeline_state_key(prim, fmt, targetLayoutKey);
      stateEntry = &sPipelineStateCache[stateKey & (PipelineStateCacheSlots - 1u)];
      if (stateEntry->valid && stateEntry->key == stateKey && stateEntry->targetLayoutKey == targetLayoutKey) {
        cache.config = stateEntry->config;
        cache.shaderInfo = stateEntry->shaderInfo;
        cache.pipelineRef = stateEntry->pipelineRef;
        stateCacheHit = true;
        ++sPipelineStateCacheHits;
      }
    }

    if (!stateCacheHit) {
      populate_pipeline_config(cache.config, prim, fmt);
      cache.shaderInfo = build_shader_info(cache.config.shaderConfig);
      cache.pipelineRef = gfx::pipeline_ref(cache.config);
      ++sPipelineStateCacheMisses;
      if (stateEntry != nullptr) {
        stateEntry->key = stateKey;
        stateEntry->targetLayoutKey = targetLayoutKey;
        stateEntry->config = cache.config;
        stateEntry->shaderInfo = cache.shaderInfo;
        stateEntry->pipelineRef = cache.pipelineRef;
        stateEntry->valid = true;
      }
    }

    cache.targetLayoutKey = targetLayoutKey;
    cache.fmt = fmt;
    cache.lineMode = lineMode;
    cache.hasPipeline = true;
    state.dirty = (state.dirty & ~DirtyPipeline) | DirtyUniform;
    if (!hadPipeline || prevSampledTextures != cache.shaderInfo.sampledTextures ||
        prevSampledIndTextures != cache.shaderInfo.sampledIndTextures) {
      cache.bindGeneration = 0;
    }
  }'''
    text = text[:start] + replacement + text[end + 1:]
    path.write_text(text, encoding="utf-8")
    print(f"{MARKER}=1 slots=64 cache=config+shaderinfo+pipeline-ref optout=GEKKOAOT_AURORA_PIPELINE_STATE_CACHE=0")


if __name__ == "__main__":
    main()
