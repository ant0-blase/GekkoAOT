// SPDX-License-Identifier: GPL-3.0-or-later
#include "hw/exi/native_exi.h"

#include <cstdint>
#include <cstdlib>
#include <iostream>

#define CHECK(condition) do { if (!(condition)) { \
  std::cerr << __LINE__ << ": " #condition "\n"; std::exit(1); \
} } while (false)

using GekkoAOT::HW::EXI::NativeEXI;

int main() {
  NativeEXI exi;
  constexpr std::uint32_t kStatus = NativeEXI::BasePhysical;
  constexpr std::uint32_t kControl = kStatus + 0x0cu;
  constexpr std::uint32_t kTcMask = 0x04u;
  constexpr std::uint32_t kTcCause = 0x08u;
  constexpr std::uint32_t kExtMask = 0x400u;
  constexpr std::uint32_t kExtCause = 0x800u;

  // A one-byte immediate transfer raises a transfer-complete cause.
  CHECK(exi.WriteRegister(kStatus, kTcMask, 4));
  CHECK(exi.WriteRegister(kControl, 1u, 4));
  CHECK((exi.Status(0) & (kTcMask | kTcCause)) == (kTcMask | kTcCause));
  CHECK(exi.InterruptPending());

  // The upper half of the 32-bit status register does not contain W1C bits.
  // A halfword store there must not acknowledge the lower-half TC cause.
  CHECK(exi.WriteRegister(kStatus, 0x1234u, 2));
  CHECK((exi.Status(0) & kTcCause) != 0u);
  CHECK(exi.InterruptPending());

  // A lower-byte store with TCINT=1 does acknowledge the cause while the
  // neighboring interrupt mask remains enabled.
  CHECK(exi.WriteRegister(kStatus + 3u, kTcMask | kTcCause, 1));
  CHECK((exi.Status(0) & (kTcMask | kTcCause)) == kTcMask);
  CHECK(!exi.InterruptPending());

  // Channel 1 powers up with its external-event cause set. Its upper-half
  // write has the same lane isolation requirement as channel 0.
  constexpr std::uint32_t kChannel1 = kStatus + NativeEXI::ChannelStride;
  CHECK((exi.Status(1) & kExtCause) != 0u);
  CHECK(exi.WriteRegister(kChannel1, kExtMask, 4));
  CHECK(exi.InterruptPending());
  CHECK(exi.WriteRegister(kChannel1, 0u, 2));
  CHECK((exi.Status(1) & kExtCause) != 0u);
  CHECK(exi.InterruptPending());

  // A byte-sized TC acknowledgement cannot clear EXTINT in the next lane.
  CHECK(exi.WriteRegister(kChannel1, kExtMask | kTcMask, 4));
  CHECK(exi.WriteRegister(kChannel1 + 0x0cu, 1u, 4));
  CHECK((exi.Status(1) & (kTcCause | kExtCause)) == (kTcCause | kExtCause));
  CHECK(exi.WriteRegister(kChannel1 + 3u, kTcMask | kTcCause, 1));
  CHECK((exi.Status(1) & (kTcCause | kExtCause)) == kExtCause);
  CHECK(exi.WriteRegister(kChannel1 + 2u, kExtMask | kExtCause, 2));
  CHECK(!exi.InterruptPending());

  std::cout << "EXI partial status writes preserve untouched W1C causes\n";
}
