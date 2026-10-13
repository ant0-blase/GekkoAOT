// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

namespace GekkoAOT::GX {
inline constexpr const char* MotionShader = R"WGSL(
@group(0) @binding(0) var previous: texture_2d<f32>;
@group(0) @binding(1) var current: texture_2d<f32>;
@group(0) @binding(2) var flow: texture_storage_2d<rgba16float, write>;

fn sample_image(image: texture_2d<f32>, p: vec2<i32>) -> vec3<f32> {
  let size = vec2<i32>(textureDimensions(image));
  return textureLoad(image, clamp(p, vec2<i32>(0), size - 1), 0).rgb;
}
fn error_at(p: vec2<i32>, offset: vec2<i32>) -> f32 {
  let taps = array<vec2<i32>, 5>(vec2<i32>(0), vec2<i32>(-3,-3),
      vec2<i32>(3,-3), vec2<i32>(-3,3), vec2<i32>(3,3));
  var cost = 0.0;
  for (var i = 0; i < 5; i++) {
    let a = sample_image(previous, p + taps[i] + offset);
    let b = sample_image(current, p + taps[i]);
    cost += dot(abs(a - b), vec3<f32>(0.299, 0.587, 0.114));
  }
  return cost / 5.0;
}
@compute @workgroup_size(8,8) fn motion(@builtin(global_invocation_id) id: vec3<u32>) {
  let size = textureDimensions(flow);
  if (any(id.xy >= size)) { return; }
  let p = vec2<i32>(id.xy * 8u + 4u);
  var best = vec2<i32>(0);
  let initial = error_at(p, best);
  var cost = initial;
  // Unchanged blocks need no search. This also keeps static HUD/text exact.
  if (initial > 0.002) {
    for (var y = -4; y <= 4; y++) {
      for (var x = -4; x <= 4; x++) {
        let offset = vec2<i32>(x,y) * 4;
        let candidate = error_at(p, offset);
        if (candidate < cost) { best = offset; cost = candidate; }
      }
    }
    let coarse = best;
    for (var y = -3; y <= 3; y++) {
      for (var x = -3; x <= 3; x++) {
        let offset = coarse + vec2<i32>(x,y);
        let candidate = error_at(p, offset);
        if (candidate < cost) { best = offset; cost = candidate; }
      }
    }
  }
  textureStore(flow, vec2<i32>(id.xy), vec4<f32>(vec2<f32>(best), cost, initial));
}
)WGSL";
} // namespace GekkoAOT::GX
