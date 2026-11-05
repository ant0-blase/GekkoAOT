#pragma once
// SPDX-License-Identifier: GPL-3.0-or-later

namespace GekkoAOT::NativeHW {

struct Config {
  bool enabled = false;
  bool si = false;
  bool di = false;
  bool exi = false;
  bool vi = false;
  bool ai = false;
  bool pi = false;
  bool mi = false;
  bool cp = false;
  bool dsp = false;
  bool strict = false;
};

// NativeHW is deliberately a host-service policy layer, not a second hardware
// emulator. Guest-visible MMIO/register/IRQ contracts stay in the small HW::*
// facades while expensive work is routed to host-native services (Aurora, VFS,
// SDL/input, host files and native audio/task backends). Each component keeps
// its exact legacy path as a fallback.
//
// 1.0-safe intentionally keeps the proven v4 timing/CP/VI scheduler behavior.
// The "full" flag therefore describes host-service coverage, not permission to
// rewrite guest-visible scheduling or CP breakpoint semantics.
class Service final {
public:
  Service();
  explicit Service(Config config) : config_(config) {}
  const Config& Policy() const { return config_; }
  bool SI() const { return config_.enabled && config_.si; }
  bool DI() const { return config_.enabled && config_.di; }
  bool EXI() const { return config_.enabled && config_.exi; }
  bool VI() const { return config_.enabled && config_.vi; }
  bool AI() const { return config_.enabled && config_.ai; }
  bool PI() const { return config_.enabled && config_.pi; }
  bool MI() const { return config_.enabled && config_.mi; }
  bool CP() const { return config_.enabled && config_.cp; }
  bool DSP() const { return config_.enabled && config_.dsp; }
  bool Enabled() const { return config_.enabled; }
  bool Strict() const { return config_.enabled && config_.strict; }
  bool FullNative() const {
    return Enabled() && SI() && DI() && EXI() && VI() && AI() && PI() && MI() &&
           CP() && DSP();
  }
  unsigned ActiveComponentCount() const {
    return static_cast<unsigned>(SI()) + static_cast<unsigned>(DI()) +
           static_cast<unsigned>(EXI()) + static_cast<unsigned>(VI()) +
           static_cast<unsigned>(AI()) + static_cast<unsigned>(PI()) +
           static_cast<unsigned>(MI()) + static_cast<unsigned>(CP()) +
           static_cast<unsigned>(DSP());
  }
  static constexpr bool PreservesV4Timing() noexcept { return true; }
  static constexpr bool PreservesCpBreakpoints() noexcept { return true; }

private:
  Config config_{};
};

} // namespace GekkoAOT::NativeHW
