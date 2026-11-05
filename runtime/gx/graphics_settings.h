#pragma once
#ifndef GEKKOAOT_GX_GRAPHICS_SETTINGS_H_INCLUDED
#define GEKKOAOT_GX_GRAPHICS_SETTINGS_H_INCLUDED
// SPDX-License-Identifier: GPL-3.0-or-later
#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>
#if __has_include("gekkoaot_environment_lighting.h")
#include "gekkoaot_environment_lighting.h"
#else
#include "environment_lighting.h"
#endif

namespace GekkoAOT::GX {
// v359 performance HUD rows (GraphicsSettings::hud_items bit mask).
enum HudItem : std::uint32_t {
  HudFps = 1u << 0, HudFrameTime = 1u << 1, HudGameFps = 1u << 2, HudVps = 1u << 3,
  HudSpeed = 1u << 4, HudCpu = 1u << 5, HudGpu = 1u << 6, HudGraph = 1u << 7,
};
inline constexpr std::uint32_t HudDefaultItems =
    HudFps | HudFrameTime | HudGameFps | HudVps | HudSpeed | HudCpu | HudGpu | HudGraph;
inline constexpr struct { const char* key; const char* env; std::uint32_t bit; } HudItemKeys[] = {
  {"hud_fps", "GEKKOAOT_HUD_FPS", HudFps}, {"hud_frametime", "GEKKOAOT_HUD_FRAMETIME", HudFrameTime},
  {"hud_game_fps", "GEKKOAOT_HUD_GAME_FPS", HudGameFps}, {"hud_vps", "GEKKOAOT_HUD_VPS", HudVps},
  {"hud_speed", "GEKKOAOT_HUD_SPEED", HudSpeed}, {"hud_cpu", "GEKKOAOT_HUD_CPU", HudCpu},
  {"hud_gpu", "GEKKOAOT_HUD_GPU", HudGpu}, {"hud_graph", "GEKKOAOT_HUD_GRAPH", HudGraph}};

// Optional, size-checked plugin ABI. Host presentation only: never a guest
// clock, VI, audio or simulation multiplier.
struct GraphicsSettings {
  std::uint32_t output_width = 0, output_height = 0, fullscreen = 0;
  std::uint32_t render_percent = 100, msaa = 1, anisotropy = 16;
  std::uint32_t post = 1, fxaa = 0, interpolation = 0;
  std::uint32_t render_width = 0, render_height = 0;
  float sharpen = .28f, bloom = .10f, vignette = .06f, grain = 0;
  float tone = .15f, local_contrast = .08f, cinematic = 0;
  float brightness = 1, contrast = 1, saturation = 1, gamma = 1;
  float exposure = 0, temperature = 0, vibrance = 0, tint = 0;
  // v359 fields; a plugin also accepts the shorter pre-HUD layout.
  std::uint32_t hud = 0, hud_items = HudDefaultItems;
  // Frame-rate unlock predicts past the newest guest frame (no added
  // latency) instead of interpolating between the last two (smoother).
  std::uint32_t unlock_predict = 0;
  std::uint32_t enhanced = 0, ao_quality = 0, contact_shadows = 0;
  float ao_strength = 1, ao_radius = 1;
  float shadow_strength = .7f, shadow_distance = 8, shadow_thickness = .25f, shadow_softness = .2f;
  // Explicit direction TOWARDS the light, in view space. No inferred sun/world camera.
  float light_x = -.5f, light_y = .7f, light_z = .3f;
  float fov_y = 0; // 0: game projection; otherwise vertical FOV in degrees.
  std::uint32_t fragment_lighting = 1;
  // Environment lighting: EnvironmentMode and SunPreset of environment_lighting.h.
  // Sky and ground colors come from the preset; the scalars override it.
  std::uint32_t environment = 0, sun_preset = 0;
  float environment_strength = .5f, sun_intensity = 1, sun_temperature = 5800;
  float sun_azimuth = 35, sun_elevation = 55, sky_intensity = .55f;
  // Cascaded sun shadow maps of replayed GX geometry: 0 off, 1 = 1024, 2 = 2048
  // texels per cascade. They need an environment sun (game light or custom).
  std::uint32_t sun_shadows = 0;
  // Screen-space one-bounce diffuse light and reflections of scene receivers.
  std::uint32_t gi = 0, ssr = 0;
  float gi_strength = .35f, ssr_strength = 1;
  // Presentation: 0 presents as soon as a frame is ready (no refresh cap),
  // 1 waits for the display's vertical blank (no tearing, at most one
  // present per refresh).
  std::uint32_t vsync = 0;
  // HD resources share Aurora's asynchronous replacement workers and GPU LRU.
  std::uint32_t texture_scale = 1, texture_quality = 0;
  std::uint32_t hd_budget_mib = 512, hd_cache_mib = 4096;
  std::uint32_t material_enhancement = 0, pom_quality = 0;
  float material_normal_strength = .65f, material_relief = .015f;
  std::uint32_t material_compare = 0;
  std::uint32_t geometry_refinement = 0;
  std::uint32_t texture_backend = 1; // 0: non-AI internal examples, 1: native ESPCN, 2: native Real-ESRGAN compact
  std::uint32_t displacement = 0;
  // Far-field depth of field, with a fixed lens-relative focus plane.
  std::uint32_t dof_quality = 0;
  float dof_strength = .65f, dof_focus = 20.f;
  std::uint32_t particle_lights = 0;
  float particle_light_strength = 1.f;
  float particle_light_radius = 2.f;
  // Remake look: the surface's own structure at finer scales, and highlights.
  float surface_detail = 0.f, surface_specular = 0.f;
};
inline constexpr std::uint32_t GraphicsSettingsPreSurfaceSize = offsetof(GraphicsSettings, surface_detail);
inline constexpr std::uint32_t GraphicsSettingsPreParticleRadiusSize = offsetof(GraphicsSettings, particle_light_radius);
inline constexpr std::uint32_t GraphicsSettingsPreParticleLightsSize = offsetof(GraphicsSettings, particle_lights);
inline constexpr std::uint32_t GraphicsSettingsPreDofSize = offsetof(GraphicsSettings, dof_quality);
inline constexpr std::uint32_t GraphicsSettingsPreDisplacementSize = offsetof(GraphicsSettings, displacement);
inline constexpr std::uint32_t GraphicsSettingsPreTextureBackendSize = offsetof(GraphicsSettings, texture_backend);
inline constexpr std::uint32_t GraphicsSettingsPreGeometrySize = offsetof(GraphicsSettings, geometry_refinement);
inline constexpr std::uint32_t GraphicsSettingsPreCompareSize = offsetof(GraphicsSettings, material_compare);
inline constexpr std::uint32_t GraphicsSettingsPreMaterialSize = offsetof(GraphicsSettings, material_enhancement);
inline constexpr std::uint32_t GraphicsSettingsPreHdSize = offsetof(GraphicsSettings, texture_scale);
inline constexpr std::uint32_t GraphicsSettingsPreEnvironmentSize = offsetof(GraphicsSettings, environment);
inline constexpr std::uint32_t GraphicsSettingsPreSunShadowsSize = offsetof(GraphicsSettings, sun_shadows);
inline constexpr std::uint32_t GraphicsSettingsPreGiSize = offsetof(GraphicsSettings, gi);
inline constexpr std::uint32_t GraphicsSettingsPreVsyncSize = offsetof(GraphicsSettings, vsync);
inline constexpr std::uint32_t SunShadowResolution(std::uint32_t quality) noexcept {
  return quality == 1 ? 1024u : quality == 2 ? 2048u : 0u;
}
inline constexpr std::uint32_t GraphicsSettingsPreFragmentSize = offsetof(GraphicsSettings, fragment_lighting);
inline constexpr std::uint32_t GraphicsSettingsPreFovSize = offsetof(GraphicsSettings, fov_y);
inline constexpr std::uint32_t GraphicsSettingsPreEnhancedSize = offsetof(GraphicsSettings, enhanced);
inline constexpr std::uint32_t GraphicsSettingsPreHudSize = offsetof(GraphicsSettings, hud);
inline constexpr std::uint32_t GraphicsSettingsPreUnlockSize = offsetof(GraphicsSettings, unlock_predict);

// Used by the live INI reader and by unit tests. Reject partial/NaN/overflow
// input; a malformed setting cannot poison a shader or allocate a huge EFB.
inline bool ParseGraphicsSetting(GraphicsSettings& s, std::string_view key,
                                 std::string_view value) {
  const auto trim=[](std::string_view text){const auto first=text.find_first_not_of(" \t\r\n");if(first==text.npos)return std::string_view{};return text.substr(first,text.find_last_not_of(" \t\r\n")-first+1);};
  key=trim(key);value=trim(value);
  std::uint32_t hud_bit = 0;
  for (const auto& item : HudItemKeys)
    if (key == item.key) hud_bit = item.bit;
  if (key == "environment_lighting") {
    if (value == "off" || value == "false") value = "0";
    if (value == "auto" || value == "on" || value == "true") value = "1";
    if (value == "custom") value = "2";
  }
  if (key == "sun_shadows") {
    if (value == "off" || value == "false") value = "0";
    if (value == "on" || value == "true" || value == "medium") value = "1";
    if (value == "high") value = "2";
  }
  if (key == "sun_preset") {
    SunPreset preset;
    constexpr std::string_view indices[]{"0", "1", "2", "3"};
    if (ParseSunPreset(value, preset)) value = indices[unsigned(preset)];
  }
  if (key == "post_processing" || key == "fxaa" || key == "frame_interpolation" || key == "fullscreen" ||
      key == "hud" || key == "frame_unlock_predict" || key == "enhanced_renderer" || key == "contact_shadows" || key == "fragment_lighting" ||
      key == "gi" || key == "ssr" || key == "vsync" || key == "material_enhancement" ||
      key == "material_compare" || key == "displacement" || hud_bit) {
    if (value == "true" || value == "on") value = "1";
    if (value == "false" || value == "off" || value == "no") value = "0";
  }
  const std::string text(value);
  char* end = nullptr;
  const float v = std::strtof(text.c_str(), &end);
  if (text.empty() || end != text.c_str() + text.size() || !std::isfinite(v)) return false;
  const auto number = [&](float& field, float low, float high) {
    field = std::clamp(v, low, high); return true;
  };
  if (key == "enhanced_renderer" || key == "contact_shadows") {
    if (v != 0 && v != 1) return false;
    (key == "enhanced_renderer" ? s.enhanced : s.contact_shadows) = std::uint32_t(v); return true;
  }
  if (key == "fragment_lighting") {
    if (v != 0 && v != 1) return false;
    s.fragment_lighting = std::uint32_t(v); return true;
  }
  if (key == "environment_lighting") {
    if (v != 0 && v != 1 && v != 2) return false;
    s.environment = std::uint32_t(v); return true;
  }
  // Selecting a preset loads its sun and sky values; later keys refine them.
  if (key == "sun_preset") {
    if (v < 0 || v >= float(SunPreset::Count) || std::floor(v) != v) return false;
    s.sun_preset = std::uint32_t(v);
    const auto preset = ApplySunPreset({}, SunPreset(s.sun_preset));
    s.sun_intensity = preset.sun_intensity; s.sun_temperature = preset.sun_temperature;
    s.sun_elevation = preset.sun_elevation; s.sky_intensity = preset.sky_intensity;
    return true;
  }
  if (key == "sun_shadows") {
    if (v != 0 && v != 1 && v != 2) return false;
    s.sun_shadows = std::uint32_t(v); return true;
  }
  if (key == "texture_backend") {
    if(v!=0 && v!=1 && v!=2)return false;
    s.texture_backend=std::uint32_t(v);return true;
  }
  if (key == "displacement") {
    if(v!=0 && v!=1)return false;
    s.displacement=std::uint32_t(v);return true;
  }
  if (key == "geometry_refinement") {
    if(std::floor(v)!=v || v<0 || v>2)return false;
    s.geometry_refinement=std::uint32_t(v);return true;
  }
  if (key == "texture_scale") {
    if(v!=1 && v!=2 && v!=4)return false;
    s.texture_scale=std::uint32_t(v);return true;
  }
  if (key == "texture_quality") {
    if(v!=0 && v!=1)return false;
    s.texture_quality=std::uint32_t(v);return true;
  }
  if (key == "material_enhancement" || key == "pom_quality" || key == "material_compare") {
    if(std::floor(v)!=v || v<0 || v>(key=="pom_quality"?2:1))return false;
    (key=="pom_quality"?s.pom_quality:key=="material_compare"?s.material_compare:s.material_enhancement)=std::uint32_t(v);return true;
  }
  if (key == "material_normal_strength") return number(s.material_normal_strength,0,2);
  if (key == "material_relief") return number(s.material_relief,0,.05f);
  if (key == "surface_detail") return number(s.surface_detail,0,1);
  if (key == "surface_specular") return number(s.surface_specular,0,1);
  if (key == "hd_budget_mib" || key == "hd_cache_mib") {
    if(std::floor(v)!=v || v<64 || v>65536)return false;
    (key=="hd_budget_mib"?s.hd_budget_mib:s.hd_cache_mib)=std::uint32_t(v);return true;
  }
  if (key == "gi" || key == "ssr") {
    if (v != 0 && v != 1) return false;
    (key == "gi" ? s.gi : s.ssr) = std::uint32_t(v); return true;
  }
  if (key == "gi_strength") return number(s.gi_strength, 0, 2);
  if (key == "particle_lights") {if(v<0 || v>2 || std::floor(v)!=v)return false;s.particle_lights=std::uint32_t(v);return true;}
  if (key == "particle_light_strength") return number(s.particle_light_strength, 0, 4);
  if (key == "particle_light_radius") return number(s.particle_light_radius, .5f, 6.f);
  if (key == "dof_quality") {if(v<0 || v>2 || std::floor(v)!=v)return false;s.dof_quality=std::uint32_t(v);return true;}
  if (key == "dof_strength") return number(s.dof_strength, 0, 1);
  if (key == "dof_focus") return number(s.dof_focus, 1, 500);
  if (key == "ssr_strength") return number(s.ssr_strength, 0, 4);
  if (key == "environment_strength") return number(s.environment_strength, 0, 1);
  if (key == "sun_intensity") return number(s.sun_intensity, 0, 4);
  if (key == "sun_temperature") return number(s.sun_temperature, 1500, 12000);
  if (key == "sun_azimuth") return number(s.sun_azimuth, -360, 360);
  if (key == "sun_elevation") return number(s.sun_elevation, 1, 90);
  if (key == "sky_intensity") return number(s.sky_intensity, 0, 2);
  if (key == "ao_quality") { if (v < 0 || v > 3 || std::floor(v) != v) return false; s.ao_quality=std::uint32_t(v);return true; }
  if (key == "fov_y") {
    if (v != 0 && (v < 20 || v > 150)) return false;
    s.fov_y=v; return true;
  }
  if (key == "ao_strength") return number(s.ao_strength,0,2);
  if (key == "ao_radius") return number(s.ao_radius,.01f,1000);
  if (key == "shadow_strength") return number(s.shadow_strength,0,1);
  if (key == "shadow_distance") return number(s.shadow_distance,.01f,10000);
  if (key == "shadow_thickness") return number(s.shadow_thickness,.001f,100);
  if (key == "shadow_softness") return number(s.shadow_softness,0,1);
  if (key == "light_x") return number(s.light_x,-1,1);
  if (key == "light_y") return number(s.light_y,-1,1);
  if (key == "light_z") return number(s.light_z,-1,1);
  if (key == "render_width" || key == "render_height") {
    if (v < 0 || v > 16384 || std::floor(v) != v) return false;
    (key == "render_width" ? s.render_width : s.render_height) = std::uint32_t(v);
    return true;
  }
  if (key == "render_percent") { s.render_percent = std::uint32_t(std::clamp(v, 25.f, 400.f)); return true; }
  if (key == "msaa_samples") { if (v != 1 && v != 4) return false; s.msaa = std::uint32_t(v); return true; }
  if (key == "texture_anisotropy") {
    if (v != 1 && v != 2 && v != 4 && v != 8 && v != 16) return false;
    s.anisotropy = std::uint32_t(v); return true;
  }
  if (key == "post_processing" || key == "fxaa" || key == "frame_interpolation" || key == "fullscreen") {
    if (v != 0 && v != 1) return false;
    (key == "post_processing" ? s.post : key == "fxaa" ? s.fxaa :
      key == "fullscreen" ? s.fullscreen : s.interpolation) = std::uint32_t(v);
    return true;
  }
  if (key == "frame_unlock_predict") {
    if (v != 0 && v != 1) return false;
    s.unlock_predict = std::uint32_t(v);
    return true;
  }
  if (key == "vsync") {
    if (v != 0 && v != 1) return false;
    s.vsync = std::uint32_t(v);
    return true;
  }
  if (key == "hud" || hud_bit) {
    if (v != 0 && v != 1) return false;
    if (key == "hud") s.hud = std::uint32_t(v);
    else s.hud_items = v != 0 ? (s.hud_items | hud_bit) : (s.hud_items & ~hud_bit);
    return true;
  }
  if (key == "post_sharpen") return number(s.sharpen, 0, 1);
  if (key == "post_bloom") return number(s.bloom, 0, 1);
  if (key == "post_vignette") return number(s.vignette, 0, 1);
  if (key == "post_grain") return number(s.grain, 0, .25f);
  if (key == "post_tone") return number(s.tone, 0, 1);
  if (key == "post_local_contrast") return number(s.local_contrast, 0, 1);
  if (key == "post_cinematic") return number(s.cinematic, 0, 1);
  if (key == "post_brightness") return number(s.brightness, 0, 2);
  if (key == "post_contrast") return number(s.contrast, 0, 2);
  if (key == "post_saturation") return number(s.saturation, 0, 2);
  if (key == "post_gamma") return number(s.gamma, .5f, 2.5f);
  if (key == "post_exposure") return number(s.exposure, -3, 3);
  if (key == "post_temperature") return number(s.temperature, -1, 1);
  if (key == "post_vibrance") return number(s.vibrance, 0, 1);
  if (key == "post_tint") return number(s.tint, -1, 1);
  return false;
}

inline constexpr struct { const char* key; const char* env; } GraphicsEnvironment[] = {
  {"texture_scale", "GEKKOAOT_TEXTURE_SCALE"}, {"texture_quality", "GEKKOAOT_TEXTURE_QUALITY"},
  {"hd_budget_mib", "GEKKOAOT_HD_BUDGET_MIB"}, {"hd_cache_mib", "GEKKOAOT_HD_CACHE_MIB"},
  {"displacement", "GEKKOAOT_DISPLACEMENT"}, {"texture_backend", "GEKKOAOT_TEXTURE_BACKEND"}, {"geometry_refinement", "GEKKOAOT_GEOMETRY_REFINEMENT"}, {"material_compare", "GEKKOAOT_MATERIAL_COMPARE"}, {"material_enhancement", "GEKKOAOT_MATERIAL_ENHANCEMENT"}, {"pom_quality", "GEKKOAOT_POM_QUALITY"},
  {"material_normal_strength", "GEKKOAOT_MATERIAL_NORMAL_STRENGTH"}, {"material_relief", "GEKKOAOT_MATERIAL_RELIEF"},
  {"surface_detail", "GEKKOAOT_SURFACE_DETAIL"}, {"surface_specular", "GEKKOAOT_SURFACE_SPECULAR"},
  {"fov_y", "GEKKOAOT_FOV_Y"},
  {"fragment_lighting", "GEKKOAOT_FRAGMENT_LIGHTING"},
  {"enhanced_renderer", "GEKKOAOT_ENHANCED_RENDERER"}, {"ao_quality", "GEKKOAOT_AO_QUALITY"},
  {"contact_shadows", "GEKKOAOT_CONTACT_SHADOWS"}, {"ao_strength", "GEKKOAOT_AO_STRENGTH"},
  {"ao_radius", "GEKKOAOT_AO_RADIUS"}, {"shadow_strength", "GEKKOAOT_SHADOW_STRENGTH"},
  {"shadow_distance", "GEKKOAOT_SHADOW_DISTANCE"}, {"shadow_thickness", "GEKKOAOT_SHADOW_THICKNESS"},
  {"shadow_softness", "GEKKOAOT_SHADOW_SOFTNESS"}, {"light_x", "GEKKOAOT_LIGHT_X"},
  {"light_y", "GEKKOAOT_LIGHT_Y"}, {"light_z", "GEKKOAOT_LIGHT_Z"},
  {"environment_lighting", "GEKKOAOT_ENVIRONMENT_LIGHTING"}, {"sun_preset", "GEKKOAOT_SUN_PRESET"},
  {"environment_strength", "GEKKOAOT_ENVIRONMENT_STRENGTH"}, {"sun_intensity", "GEKKOAOT_SUN_INTENSITY"},
  {"sun_temperature", "GEKKOAOT_SUN_TEMPERATURE"}, {"sun_azimuth", "GEKKOAOT_SUN_AZIMUTH"},
  {"sun_elevation", "GEKKOAOT_SUN_ELEVATION"}, {"sky_intensity", "GEKKOAOT_SKY_INTENSITY"},
  {"sun_shadows", "GEKKOAOT_SUN_SHADOWS"}, {"gi", "GEKKOAOT_GI"}, {"gi_strength", "GEKKOAOT_GI_STRENGTH"},
  {"particle_lights", "GEKKOAOT_PARTICLE_LIGHTS"}, {"particle_light_strength", "GEKKOAOT_PARTICLE_LIGHT_STRENGTH"},
  {"particle_light_radius", "GEKKOAOT_PARTICLE_LIGHT_RADIUS"},
  {"dof_quality", "GEKKOAOT_DOF_QUALITY"}, {"dof_strength", "GEKKOAOT_DOF_STRENGTH"}, {"dof_focus", "GEKKOAOT_DOF_FOCUS"},
  {"ssr", "GEKKOAOT_SSR"}, {"ssr_strength", "GEKKOAOT_SSR_STRENGTH"},
  {"fullscreen", "GEKKOAOT_FULLSCREEN"},
  {"render_width", "GEKKOAOT_RENDER_WIDTH"}, {"render_height", "GEKKOAOT_RENDER_HEIGHT"},
  {"render_percent", "GEKKOAOT_RENDER_PERCENT"}, {"msaa_samples", "GEKKOAOT_NATIVE_GX_MSAA"},
  {"texture_anisotropy", "GEKKOAOT_NATIVE_GX_ANISOTROPY"}, {"post_processing", "GEKKOAOT_POST_PROCESSING"},
  {"fxaa", "GEKKOAOT_FXAA"}, {"frame_interpolation", "GEKKOAOT_FRAME_INTERPOLATION"},
  {"frame_unlock_predict", "GEKKOAOT_REPLAY_PREDICT"}, {"vsync", "GEKKOAOT_NATIVE_GX_VSYNC"},
  {"post_sharpen", "GEKKOAOT_POSTFX_SHARPEN"}, {"post_bloom", "GEKKOAOT_POSTFX_BLOOM"},
  {"post_vignette", "GEKKOAOT_POSTFX_VIGNETTE"}, {"post_grain", "GEKKOAOT_POSTFX_GRAIN"},
  {"post_tone", "GEKKOAOT_POSTFX_TONE"}, {"post_local_contrast", "GEKKOAOT_POSTFX_LOCAL_CONTRAST"},
  {"post_brightness", "GEKKOAOT_POSTFX_BRIGHTNESS"}, {"post_contrast", "GEKKOAOT_POSTFX_CONTRAST"},
  {"post_saturation", "GEKKOAOT_POSTFX_SATURATION"}, {"post_gamma", "GEKKOAOT_POSTFX_GAMMA"},
  {"post_exposure", "GEKKOAOT_POSTFX_EXPOSURE"}, {"post_temperature", "GEKKOAOT_POSTFX_TEMPERATURE"},
  {"post_vibrance", "GEKKOAOT_POSTFX_VIBRANCE"}, {"post_tint", "GEKKOAOT_POSTFX_TINT"}};

inline EnvironmentSettings EnvironmentFromGraphics(const GraphicsSettings& s) noexcept {
  auto e = ApplySunPreset({}, SunPreset(std::min(s.sun_preset, std::uint32_t(SunPreset::Count) - 1)));
  e.mode = s.environment <= 2 ? EnvironmentMode(s.environment) : EnvironmentMode::Off;
  e.strength = s.environment_strength; e.sun_intensity = s.sun_intensity; e.sun_temperature = s.sun_temperature;
  e.sun_azimuth = s.sun_azimuth; e.sun_elevation = s.sun_elevation; e.sky_intensity = s.sky_intensity;
  return e;
}

inline GraphicsSettings GraphicsSettingsFromEnvironment() {
  GraphicsSettings result;
  if (const char* preset = std::getenv("GEKKOAOT_POSTFX_PRESET"); preset && std::string_view(preset) == "cinematic")
    result.cinematic = .20f;
  for (const auto& option : GraphicsEnvironment)
    if (const char* value = std::getenv(option.env)) ParseGraphicsSetting(result, option.key, value);
  if (const char* value = std::getenv("GEKKOAOT_HUD")) ParseGraphicsSetting(result, "hud", value);
  for (const auto& item : HudItemKeys)
    if (const char* value = std::getenv(item.env)) ParseGraphicsSetting(result, item.key, value);
  return result;
}
}

#endif // GEKKOAOT_GX_GRAPHICS_SETTINGS_H_INCLUDED
