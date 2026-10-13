#pragma once
// SPDX-License-Identifier: GPL-3.0-or-later

namespace GekkoAOT::NativeHW::DSP {
// Known AX/JAudio tasks are already rendered by host-native mixers. Unknown
// microcode deliberately remains on the standalone DSP-core/fallback path.
constexpr bool HostTaskBackendActive(bool enabled, bool ax_hle,
                                     bool jaudio_hle) noexcept {
  return enabled && (ax_hle || jaudio_hle);
}

// Time-driven DSP causes only need to touch PI when the DSP actually latched an
// interrupt. MMIO writes synchronize immediately on their own path.
constexpr bool TimedIrqNeedsSync(bool enabled, bool pending) noexcept {
  return enabled && pending;
}
} // namespace GekkoAOT::NativeHW::DSP
