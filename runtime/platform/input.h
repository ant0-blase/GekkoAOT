#pragma once
#include <array>
#include <cstdint>
#include <algorithm>
namespace GekkoAOT::Input {
struct Pad {
  std::uint16_t buttons = 0;
  std::int8_t stick_x = 0, stick_y = 0, sub_x = 0, sub_y = 0;
  std::uint8_t left = 0, right = 0, analog_a = 0, analog_b = 0;
  bool connected = false, rumble = false;
};
inline std::int8_t Axis(std::int16_t raw, bool invert = false) {
  int value = raw;
  if (invert) value = -value;
  constexpr int deadzone = 4096;
  if (value >= -deadzone && value <= deadzone) return 0;
  const int sign = value < 0 ? -1 : 1;
  const int magnitude = std::min(32767, value < 0 ? -value : value);
  return static_cast<std::int8_t>(sign * (magnitude - deadzone) * 80 / (32767 - deadzone));
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
