#pragma once
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <cstdint>
#include <limits>

namespace GekkoAOT::NativeHW::Events {

constexpr std::uint64_t Never = std::numeric_limits<std::uint64_t>::max();

struct Deadlines {
  std::uint64_t vi = Never;
  std::uint64_t vi_frame = Never;
  std::uint64_t si = Never;
  std::uint64_t ai = Never;
  std::uint64_t dsp = Never;
  std::uint64_t di = Never;
  std::uint64_t decrementer = Never;
  std::uint64_t alarm = Never;
};

// NativeHW v4 treats idle hardware as an event graph rather than something the
// PPC thread has to poll.  Device implementations remain the guest-visible
// authority; this helper only collapses their already-computed deadlines.
constexpr std::uint64_t Earliest(const Deadlines& d) noexcept {
  return std::min({d.vi, d.vi_frame, d.si, d.ai, d.dsp, d.di,
                   d.decrementer, d.alarm});
}

constexpr std::uint64_t ClampMaintenance(std::uint64_t deadline,
                                         std::uint64_t cap) noexcept {
  return deadline == Never ? cap : std::min(deadline, cap);
}

} // namespace GekkoAOT::NativeHW::Events
