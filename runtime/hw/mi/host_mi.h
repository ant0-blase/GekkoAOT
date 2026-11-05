#pragma once
// SPDX-License-Identifier: GPL-3.0-or-later

namespace GekkoAOT::NativeHW::MI {
// Guest RAM is already backed directly by the host AddressSpace. NativeHW MI
// therefore keeps only the SDK-visible protection/perf register facade.
constexpr bool UseHostAddressSpace(bool enabled) noexcept { return enabled; }
} // namespace GekkoAOT::NativeHW::MI
