#pragma once
// SPDX-License-Identifier: GPL-3.0-or-later
#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>

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
  // v361: frame-rate unlock predicts past the newest guest frame (no added
  // latency) instead of interpolating between the last two (smoother).
  std::uint32_t unlock_predict = 0;
};
inline constexpr std::uint32_t GraphicsSettingsPreHudSize = offsetof(GraphicsSettings, hud);
inline constexpr std::uint32_t GraphicsSettingsPreUnlockSize = offsetof(GraphicsSettings, unlock_predict);

// Used by the live INI reader and by unit tests. Reject partial/NaN/overflow
// input; a malformed setting cannot poison a shader or allocate a huge EFB.
inline bool ParseGraphicsSetting(GraphicsSettings& s, std::string_view key,
                                 std::string_view value) {
  std::uint32_t hud_bit = 0;
  for (const auto& item : HudItemKeys)
    if (key == item.key) hud_bit = item.bit;
  if (key == "post_processing" || key == "fxaa" || key == "frame_interpolation" || key == "fullscreen" ||
      key == "hud" || key == "frame_unlock_predict" || hud_bit) {
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
  {"fullscreen", "GEKKOAOT_FULLSCREEN"},
  {"render_width", "GEKKOAOT_RENDER_WIDTH"}, {"render_height", "GEKKOAOT_RENDER_HEIGHT"},
  {"render_percent", "GEKKOAOT_RENDER_PERCENT"}, {"msaa_samples", "GEKKOAOT_NATIVE_GX_MSAA"},
  {"texture_anisotropy", "GEKKOAOT_NATIVE_GX_ANISOTROPY"}, {"post_processing", "GEKKOAOT_POST_PROCESSING"},
  {"fxaa", "GEKKOAOT_FXAA"}, {"frame_interpolation", "GEKKOAOT_FRAME_INTERPOLATION"},
  {"frame_unlock_predict", "GEKKOAOT_REPLAY_PREDICT"},
  {"post_sharpen", "GEKKOAOT_POSTFX_SHARPEN"}, {"post_bloom", "GEKKOAOT_POSTFX_BLOOM"},
  {"post_vignette", "GEKKOAOT_POSTFX_VIGNETTE"}, {"post_grain", "GEKKOAOT_POSTFX_GRAIN"},
  {"post_tone", "GEKKOAOT_POSTFX_TONE"}, {"post_local_contrast", "GEKKOAOT_POSTFX_LOCAL_CONTRAST"},
  {"post_brightness", "GEKKOAOT_POSTFX_BRIGHTNESS"}, {"post_contrast", "GEKKOAOT_POSTFX_CONTRAST"},
  {"post_saturation", "GEKKOAOT_POSTFX_SATURATION"}, {"post_gamma", "GEKKOAOT_POSTFX_GAMMA"},
  {"post_exposure", "GEKKOAOT_POSTFX_EXPOSURE"}, {"post_temperature", "GEKKOAOT_POSTFX_TEMPERATURE"},
  {"post_vibrance", "GEKKOAOT_POSTFX_VIBRANCE"}, {"post_tint", "GEKKOAOT_POSTFX_TINT"}};

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
