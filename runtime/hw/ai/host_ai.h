#pragma once
// SPDX-License-Identifier: GPL-3.0-or-later
#include <cstdint>

namespace GekkoAOT::NativeHW::AI {
// AI already advances from HostRuntime's monotonic host-realtime clock. NativeHW
// marks that path as authoritative while the tiny CC006Cxx register facade stays
// guest-visible for SDK compatibility.
constexpr bool UseHostTimedInterface(bool enabled) noexcept { return enabled; }

// AX/JAudio host mixers render at the native GC 32 kHz domain. Unknown/custom
// DSP tasks retain the guest-selected AID rate.
constexpr std::uint32_t EffectiveAidRate(bool enabled, std::uint32_t guest_hz,
                                         bool host_mixer_active) noexcept {
  return enabled && host_mixer_active ? 32028u : guest_hz;
}

constexpr bool TimedIrqNeedsSync(bool enabled, bool pending) noexcept {
  return enabled && pending;
}
} // namespace GekkoAOT::NativeHW::AI
