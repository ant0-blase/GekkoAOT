// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "graphics_settings.h"
namespace GekkoAOT::GX {
// Image presets leave display size, aspect policy, FOV, HUD and pacing alone.
inline bool ApplyGraphicsPreset(GraphicsSettings& s,std::string_view name) {
  const bool remake=name=="remastered_remake";
  const bool remastered=name=="remastered" || name=="remastered_vivid" || name=="remastered_cinematic" ||
                        name=="remastered_performance" || remake;
  if(name!="neutral" && name!="balanced" && name!="cinematic" && name!="crisp" && !remastered)return false;
  s.post=name!="neutral";
  s.brightness=s.contrast=s.saturation=s.gamma=1;
  s.exposure=s.temperature=s.vibrance=s.tint=0;
  s.sharpen=s.bloom=s.vignette=s.grain=s.tone=s.local_contrast=s.cinematic=0;
  s.enhanced=name!="neutral";s.ao_quality=name=="neutral"?0:name=="cinematic"?3:2;
  s.fragment_lighting=name!="neutral";
  s.material_enhancement=remastered?1u:0u;
  // Curved meshes are an explicit choice, in no preset: on strip-built models
  // they cost six times the triangles for no rounder silhouette.
  s.geometry_refinement=0;
  s.pom_quality=remastered?(name=="remastered_performance"?0u:name=="remastered_cinematic" || remake?2u:1u):0u;
  s.material_normal_strength=name=="remastered_cinematic" || remake?1.f:.8f;s.material_relief=.02f;
  s.material_compare=0;s.displacement=0;
  s.surface_detail=remake?.6f:0.f;s.surface_specular=remake?.8f:0.f;
  // Texture resolution remains the user's independent preference, except in
  // Remake: Remastered works from the original disc textures with
  // reconstruction off, Remake is meant to change how the game looks.

  s.contact_shadows=name!="neutral";s.ao_strength=.7f;s.ao_radius=1;
  s.shadow_strength=.45f;s.shadow_distance=8;s.shadow_thickness=.25f;s.shadow_softness=.2f;
  s.light_x=-.5f;s.light_y=.7f;s.light_z=.3f;
  // Environment light, sun cascades, GI and reflections: the Remastered
  // presets only. All use the game's own sun (Auto), neutral daylight unless
  // stated, and enough sky light that faces turned from the sun keep about
  // 80% of their original light (no dark or tinted scenes).
  s.environment=remastered?1u:0u;s.sun_preset=0;s.environment_strength=.4f;
  s.sun_intensity=1;s.sun_temperature=5800;s.sun_azimuth=35;s.sun_elevation=55;s.sky_intensity=.8f;
  s.sun_shadows=remastered?2u:0u;s.gi=s.ssr=remastered?1u:0u;s.gi_strength=.35f;s.ssr_strength=1;
  if(remastered){
    // Natural: faithful colors, clean daylight and shadows.
    s.ao_quality=2;s.ao_strength=.8f;s.shadow_strength=.5f;
    s.sharpen=.2f;s.bloom=.06f;s.vignette=.03f;s.tone=.12f;s.local_contrast=.05f;s.vibrance=.1f;
  }
  if(name=="remastered_vivid"){
    // Brighter and more colorful: stronger bounce light and reflections.
    s.environment_strength=.45f;s.sky_intensity=.9f;s.gi_strength=.5f;s.ssr_strength=1.2f;
    s.saturation=1.1f;s.vibrance=.3f;s.contrast=1.04f;s.exposure=.1f;
    s.bloom=.1f;s.tone=.1f;s.local_contrast=.07f;s.sharpen=.22f;
  }
  if(name=="remastered_cinematic"){
    // Warm (not orange) light, deeper shadows, filmic tone.
    s.ao_quality=3;s.ao_strength=1;s.shadow_strength=.6f;
    s.sun_preset=1;s.sun_temperature=4700;s.sun_intensity=1.1f;s.sun_elevation=42;
    s.environment_strength=.5f;s.sky_intensity=.65f;
    s.tone=.22f;s.contrast=1.06f;s.local_contrast=.08f;s.bloom=.12f;s.vignette=.06f;s.grain=.005f;
    s.cinematic=.1f;s.sharpen=.18f;s.vibrance=.08f;
  }
  if(remake){
    // The Cinematic light on Real-ESRGAN textures. Inference runs once per
    // texture and the result stays on disk, so play only pays for sampling.
    s.ao_quality=3;s.ao_strength=1;s.shadow_strength=.6f;
    s.sun_preset=1;s.sun_temperature=4700;s.sun_intensity=1.1f;s.sun_elevation=42;
    s.environment_strength=.5f;s.sky_intensity=.65f;
    s.tone=.22f;s.contrast=1.06f;s.local_contrast=.08f;s.bloom=.12f;s.vignette=.06f;s.grain=.005f;
    s.cinematic=.1f;s.sharpen=.18f;s.vibrance=.08f;
    s.texture_scale=4;s.texture_quality=1;s.texture_backend=2;
    // FXAA costs nothing measurable here; 4x MSAA takes a frame to about 16.5 ms
    // on an integrated GPU, so it stays a manual choice.
    s.fxaa=1;
  }
  if(name=="remastered_performance"){
    // The Natural look at a lower GPU cost.
    s.ao_quality=1;s.sun_shadows=1;s.gi=s.ssr=0;
  }
  if(name=="balanced"){s.sharpen=.2f;s.bloom=.06f;s.vignette=.03f;s.tone=.1f;s.local_contrast=.05f;}
  if(name=="cinematic"){
    s.sharpen=.18f;s.bloom=.12f;s.vignette=.06f;s.grain=.005f;
    s.tone=.22f;s.local_contrast=.08f;s.cinematic=.1f;s.ao_strength=1;s.shadow_strength=.6f;
  }
  if(name=="crisp"){s.sharpen=.35f;s.bloom=.03f;s.tone=.05f;s.local_contrast=.08f;}
  return true;
}
inline bool SceneMatchesPreset(const GraphicsSettings& s,std::string_view name) {
  auto expected=s;if(!ApplyGraphicsPreset(expected,name))return false;
  return s.displacement==expected.displacement && s.material_enhancement==expected.material_enhancement && s.geometry_refinement==expected.geometry_refinement &&
    s.pom_quality==expected.pom_quality && s.material_normal_strength==expected.material_normal_strength &&
    s.material_relief==expected.material_relief && s.surface_detail==expected.surface_detail && s.surface_specular==expected.surface_specular && s.enhanced==expected.enhanced && s.fragment_lighting==expected.fragment_lighting && s.ao_quality==expected.ao_quality &&
    s.contact_shadows==expected.contact_shadows && s.ao_strength==expected.ao_strength &&
    s.ao_radius==expected.ao_radius && s.shadow_strength==expected.shadow_strength &&
    s.shadow_distance==expected.shadow_distance && s.shadow_thickness==expected.shadow_thickness &&
    s.shadow_softness==expected.shadow_softness && s.light_x==expected.light_x &&
    s.light_y==expected.light_y && s.light_z==expected.light_z && s.environment==expected.environment &&
    s.sun_shadows==expected.sun_shadows && s.gi==expected.gi && s.ssr==expected.ssr &&
    s.environment_strength==expected.environment_strength && s.gi_strength==expected.gi_strength &&
    s.ssr_strength==expected.ssr_strength && s.sun_preset==expected.sun_preset && s.sky_intensity==expected.sky_intensity &&
    s.sun_intensity==expected.sun_intensity && s.sun_temperature==expected.sun_temperature;
}
}
