#pragma once
#include <array>
#include <cstdint>
#include <algorithm>
#include <cmath>
namespace GekkoAOT::Input {
struct Pad {
  std::uint16_t buttons = 0;
  std::int8_t stick_x = 0, stick_y = 0, sub_x = 0, sub_y = 0;
  std::uint8_t left = 0, right = 0, analog_a = 0, analog_b = 0;
  bool connected = false, rumble = false;
};
struct Stick {
  std::int8_t x = 0, y = 0;
  float normalized_x = 0.f, normalized_y = 0.f;
  float mapped_x = 0.f, mapped_y = 0.f;
};

// SDL's negative endpoint is -32768 and the positive endpoint is 32767.
// Preserve direction through one radial dead zone and one circular clamp.
inline Stick MapStick(int raw_x, int raw_y, int deadzone = 4096) {
  Stick out;
  out.normalized_x = std::clamp(raw_x / (raw_x < 0 ? 32768.f : 32767.f), -1.f, 1.f);
  out.normalized_y = std::clamp(raw_y / (raw_y < 0 ? 32768.f : 32767.f), -1.f, 1.f);
  const float magnitude = std::hypot(out.normalized_x, out.normalized_y);
  const float threshold = std::clamp(deadzone / 32767.f, 0.f, .95f);
  if (magnitude <= threshold) return out;
  const float radius = std::clamp((magnitude - threshold) / (1.f - threshold), 0.f, 1.f);
  out.mapped_x = out.normalized_x / magnitude * radius;
  out.mapped_y = out.normalized_y / magnitude * radius;
  out.x = static_cast<std::int8_t>(std::clamp(std::lround(out.mapped_x * 127.f), -127l, 127l));
  out.y = static_cast<std::int8_t>(std::clamp(std::lround(out.mapped_y * 127.f), -127l, 127l));
  return out;
}

inline std::int8_t Axis(std::int16_t raw, bool invert = false) {
  return MapStick(invert ? -int(raw) : int(raw), 0).x;
}
inline std::uint8_t Trigger(std::int16_t raw) {
  return static_cast<std::uint8_t>(std::max(0, int(raw)) * 255 / 32767);
}
inline std::uint32_t Encode(const std::array<Pad, 4>& pads, std::uint8_t* out) {
  std::uint32_t mask = 0;
  for (unsigned i=0; i<4; ++i) {
    auto* p = out + i*12;
    std::fill_n(p,12,0);
    const auto& pad = pads[i];
    if (!pad.connected) { p[10]=255; continue; }
    p[0]=pad.buttons>>8; p[1]=pad.buttons;
    p[2]=static_cast<std::uint8_t>(pad.stick_x); p[3]=static_cast<std::uint8_t>(pad.stick_y);
    p[4]=static_cast<std::uint8_t>(pad.sub_x); p[5]=static_cast<std::uint8_t>(pad.sub_y);
    p[6]=pad.left; p[7]=pad.right; p[8]=pad.analog_a; p[9]=pad.analog_b;
    if(pad.rumble) mask |= 0x80000000u >> i;
  }
  return mask;
}
}  // namespace GekkoAOT::Input
