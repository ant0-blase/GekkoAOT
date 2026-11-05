#!/usr/bin/env python3
"""Compose encoder-local native resource binding cache with the pinned Aurora."""
from pathlib import Path
import sys

root = Path(sys.argv[1])
marker = 'GEKKOAOT_NATIVE_BIND_GROUP_CACHE_V297'
header = Path(__file__).resolve().parent.parent/'native_bind_group_cache.h'
target = root/'lib/gx/gekkoaot_native_bind_group_cache.hpp'
text = header.read_text()
if not target.exists() or target.read_text() != text:
    target.write_text(text)

def replace(source, before, after):
    if source.count(before) != 1:
        raise SystemExit('Native bind cache: pinned Aurora anchor missing or ambiguous: '+before[:70])
    return source.replace(before, after, 1)

p = root/'lib/gx/pipeline.cpp'
s = p.read_text()
if marker not in s:
    s = replace(s, '#include "pipeline.hpp"',
                '#include "pipeline.hpp"\n#include "gekkoaot_native_bind_group_cache.hpp"')
    s = replace(s, 'namespace aurora::gx {\nnamespace {', '''namespace aurora::gx {
namespace {
// GEKKOAOT_NATIVE_BIND_GROUP_CACHE_V297: each render encoder owns its bindings.
// Custom encoders and begin-pass invalidate them, including uniform offsets.
thread_local GekkoAOT::GX::NativeBindGroupCache g_nativeBindings;
thread_local unsigned g_nativeBindingSuspensions = 0;
bool native_binding_cache_enabled() noexcept {
  static const bool enabled = [] {
    const auto* value = std::getenv("GEKKOAOT_NATIVE_GX_BIND_GROUP_CACHE");
    // Default on (docs/OPTIMIZATIONS.md); GEKKOAOT_NATIVE_GX_BIND_GROUP_CACHE=0 disables.
    const bool on = !(value && value[0] == '0' && value[1] == '\\0');
    std::fprintf(stderr, "GEKKOAOT_NATIVE_GX_BIND_GROUP_CACHE_V297=%u scope=encoder reset=pass+custom offsets=exact default=on\\n", on ? 1u : 0u);
    return on;
  }();
  return enabled;
}
bool bind_native_group(unsigned group, const wgpu::BindGroup& resource,
                       std::uint32_t offset = 0) noexcept {
  return !native_binding_cache_enabled() || g_nativeBindingSuspensions || g_nativeBindings.NeedsBind(
      group, reinterpret_cast<std::uintptr_t>(resource.Get()), offset);
}''')
    s = replace(s, 'void render(const DrawData& data, const wgpu::RenderPassEncoder& pass) {', '''void reset_native_bind_group_cache() noexcept { g_nativeBindings.Reset(); }

ScopedNativeBindGroupCacheSuspend::ScopedNativeBindGroupCacheSuspend() noexcept {
  ++g_nativeBindingSuspensions;
  g_nativeBindings.Reset();
}
ScopedNativeBindGroupCacheSuspend::~ScopedNativeBindGroupCacheSuspend() {
  --g_nativeBindingSuspensions;
  g_nativeBindings.Reset();
}

void render(const DrawData& data, const wgpu::RenderPassEncoder& pass) {''')
    s = replace(s, '''  pass.SetBindGroup(1, resources.uniformBindGroup, offsets.size(), offsets.data());
  if (data.bindGroups.textureBindGroup) {
    pass.SetBindGroup(2, gfx::find_bind_group(data.bindGroups.textureBindGroup));
  }
  pass.SetBindGroup(3, data.logicDestination ? gfx::find_bind_group(data.logicDestination)
                                           : g_emptyLogicBindGroup);''', '''  if (bind_native_group(1, resources.uniformBindGroup, data.uniformRange.offset))
    pass.SetBindGroup(1, resources.uniformBindGroup, offsets.size(), offsets.data());
  if (data.bindGroups.textureBindGroup) {
    const auto texture = gfx::find_bind_group(data.bindGroups.textureBindGroup);
    if (bind_native_group(2, texture)) pass.SetBindGroup(2, texture);
  }
  const auto logic = data.logicDestination ? gfx::find_bind_group(data.logicDestination)
                                          : g_emptyLogicBindGroup;
  if (bind_native_group(3, logic)) pass.SetBindGroup(3, logic);''')
    p.write_text(s)

p = root/'lib/gx/pipeline.hpp'
s = p.read_text()
if marker not in s:
    s = replace(s, 'void render(const DrawData& data, const wgpu::RenderPassEncoder& pass);',
                '''// GEKKOAOT_NATIVE_BIND_GROUP_CACHE_V297
void reset_native_bind_group_cache() noexcept;
class ScopedNativeBindGroupCacheSuspend {
public:
  ScopedNativeBindGroupCacheSuspend() noexcept;
  ~ScopedNativeBindGroupCacheSuspend();
  ScopedNativeBindGroupCacheSuspend(const ScopedNativeBindGroupCacheSuspend&) = delete;
  ScopedNativeBindGroupCacheSuspend& operator=(const ScopedNativeBindGroupCacheSuspend&) = delete;
};
void render(const DrawData& data, const wgpu::RenderPassEncoder& pass);''')
    p.write_text(s)

p = root/'lib/gfx/encoding.cpp'
s = p.read_text()
if marker not in s:
    s = replace(s, '  // Bind bind group for the whole pass',
                '  // GEKKOAOT_NATIVE_BIND_GROUP_CACHE_V297\n  gx::reset_native_bind_group_cache();\n\n  // Bind bind group for the whole pass')
    s = replace(s, '      render_custom_draw(cmd.data.customDraw, pass, passInfo);',
                '''      // Custom callbacks may bind arbitrary groups or re-enter GX.
      // Disable caching throughout the callback, then invalidate on return.
      const gx::ScopedNativeBindGroupCacheSuspend cacheSuspend;
      render_custom_draw(cmd.data.customDraw, pass, passInfo);''')
    p.write_text(s)
p = root/'lib/gfx/recording.cpp'
s = p.read_text()
if marker not in s:
    s = replace(s, '''  const T& data = inline_payload<T>(payload);
  if constexpr (std::is_invocable_v<decltype(Renderer), const T&, const wgpu::RenderPassEncoder&,
''', '''  const T& data = inline_payload<T>(payload);
  // GEKKOAOT_NATIVE_BIND_GROUP_CACHE_V297: only the known GX encoder may
  // retain bindings; clears/UI and future inline encoders are boundaries.
  if constexpr (std::is_same_v<T, gx::DrawData>) {
    static_assert(Renderer == gx::render);
    Renderer(data, pass);
    return;
  }
  const gx::ScopedNativeBindGroupCacheSuspend cacheSuspend;
  if constexpr (std::is_invocable_v<decltype(Renderer), const T&, const wgpu::RenderPassEncoder&,
''')
    p.write_text(s)
print('GEKKOAOT_AURORA_NATIVE_BIND_GROUP_CACHE_V297=1 reset=pass+custom+non-GX-draw key=native-handle+dynamic-offset default=off')
