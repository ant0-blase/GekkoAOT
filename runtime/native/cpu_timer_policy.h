#pragma once
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstdint>

namespace GekkoAOT::Native {

// With guest-work timing enabled, the CPU timebase/DEC follow retired AOT
// charges instead of being paced to the physical host clock. Device timing is
// intentionally NOT affected: VI, AI, DSP, DI and host presentation still use
// realtime hardware cycles. When the CPU is actually idle, retain realtime
// progression so pending alarms and decrementer interrupts can wake it.
//
// NOTE: an AOT charge is a compiler model, not an exact hardware cycle count.
// This is an opt-in experiment, NOT a retail-accurate overclock or a 60 FPS fix.
inline std::uint64_t CpuTimerCharge(bool guest_work_timers,
                                    std::uint64_t retired_guest_cycles,
                                    std::uint64_t realtime_hardware_cycles,
                                    bool idle_wait) noexcept {
  if (guest_work_timers && !idle_wait && retired_guest_cycles != 0u)
    return retired_guest_cycles;
  return realtime_hardware_cycles;
}

} // namespace GekkoAOT::Native
