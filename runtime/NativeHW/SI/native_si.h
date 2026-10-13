#pragma once
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstdint>
#include <limits>

namespace GekkoAOT::NativeHW::SI {
// An explicit COMCSR transfer is a useful low-latency sampling boundary, but it
// is not a replacement for retail SIPOLL automatic sampling. Some games never
// issue a buffer transfer while reading pad state; they consume IN_HI/IN_LO
// populated by periodic SI polls. NativeHW therefore keeps both paths.
inline bool RefreshOnTransfer(bool enabled, bool input_ready) {
  return enabled && input_ready;
}

// NativeHW v3 incorrectly disabled the periodic path when SI was enabled. That
// froze both keyboard and gamepad state after the initial snapshot. The host
// sample + SI latch is cheap (~120 Hz) and is part of the hardware contract, so
// it remains active in both NativeHW and legacy modes.
constexpr bool NeedsPeriodicHostPoll(bool /*enabled*/, bool input_ready) noexcept {
  return input_ready;
}

constexpr std::uint64_t PollIntervalCycles(std::uint64_t cpu_clock_hz,
                                           std::uint32_t hz = 120u) noexcept {
  return hz == 0u ? 0u : (cpu_clock_hz / hz ? cpu_clock_hz / hz : 1u);
}

constexpr std::uint64_t CyclesUntilPoll(bool input_ready,
                                        std::uint64_t phase,
                                        std::uint64_t interval) noexcept {
  if (!input_ready || interval == 0u)
    return std::numeric_limits<std::uint64_t>::max();
  phase %= interval;
  return phase == 0u ? interval : interval - phase;
}
} // namespace GekkoAOT::NativeHW::SI
