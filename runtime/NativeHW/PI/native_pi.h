#pragma once
// SPDX-License-Identifier: GPL-3.0-or-later

namespace GekkoAOT::NativeHW::PI {
// PI has no expensive device body to emulate: it is the guest interrupt
// contract. In NativeHW mode it remains only a compact cause/mask aggregator
// fed directly by host-native devices.
constexpr bool UseHostInterruptAggregation(bool enabled) noexcept { return enabled; }
} // namespace GekkoAOT::NativeHW::PI
