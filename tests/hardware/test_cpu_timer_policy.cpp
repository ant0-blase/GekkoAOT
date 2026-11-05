// SPDX-License-Identifier: GPL-3.0-or-later
#include "native/cpu_timer_policy.h"
#include <cassert>
#include <cstdint>

int main() {
  using GekkoAOT::Native::CpuTimerCharge;
  // The default path preserves exact legacy behavior.
  assert(CpuTimerCharge(false, 9000, 486, false) == 486);
  assert(CpuTimerCharge(false, 9000, 0, false) == 0);
  // The opt-in path never waits for a hardware-clock sample to advance TB/DEC.
  assert(CpuTimerCharge(true, 9000, 0, false) == 9000);
  assert(CpuTimerCharge(true, 9000, 486, false) == 9000);
  // Idle, event waits and zero-charge callbacks preserve interrupt progression.
  assert(CpuTimerCharge(true, 0, 486, false) == 486);
  assert(CpuTimerCharge(true, 9000, 486, true) == 486);
  assert(CpuTimerCharge(true, 9000, 0, true) == 0);
  assert(CpuTimerCharge(true, 0, 0, false) == 0);
}
