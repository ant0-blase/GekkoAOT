// SPDX-License-Identifier: GPL-3.0-or-later
#include "hw/cp/native_cp.h"
#include <cstdlib>
#include <iostream>
#define CHECK(x) do { if (!(x)) { std::cerr << __LINE__ << ": " #x "\n"; std::exit(1); } } while (false)
using GekkoAOT::HW::CP::NativeCP;
int main() {
  NativeCP cp;
  const auto write = [&](unsigned offset, unsigned value) { CHECK(cp.Write(0xcc000000u + offset, value, 2)); };
  const auto pair = [&](unsigned offset, unsigned value) { write(offset, value); write(offset + 2, value >> 16); };
  // SDK top points to the last word; CP drops its low five bits.
  pair(0x20, 0x1000);
  pair(0x24, 0x107c);
  pair(0x34, 0x1000);
  pair(0x38, 0x1000);
  pair(0x28, 0x60);
  pair(0x2c, 0x20);
  write(2, 0x10); // linked CPU producer, GP stopped
  CHECK(!cp.NotifyGatherWrite(31));
  CHECK(cp.FifoReadWriteDistance() == 0);
  CHECK(cp.NotifyGatherWrite(1));
  CHECK(cp.FifoReadWriteDistance() == 32);
  CHECK(cp.NotifyGatherWrite(96));
  CHECK(cp.FifoWritePointer() == 0x1000);
  CHECK(cp.FifoReadWriteDistance() == 128); // Full FIFO must retain all four bursts.
  pair(0x3c, 0x1040);
  write(2, 0x33); // read + link + breakpoint + breakpoint IRQ
  CHECK(cp.FifoReadPointer() == 0x1040);
  CHECK(cp.FifoReadWriteDistance() == 64);
  CHECK(cp.InterruptPending());
  CHECK((cp.Status() & 0x10) != 0);
  write(2, 0x11); // release breakpoint and drain through wrap
  CHECK(cp.FifoReadPointer() == 0x1000);
  CHECK(cp.FifoReadWriteDistance() == 0);
  CHECK(!cp.InterruptPending());
  CHECK(cp.NotifyGatherWrite(128));
  CHECK(cp.FifoReadPointer() == 0x1000 && cp.FifoWritePointer() == 0x1000);
  CHECK(cp.FifoReadWriteDistance() == 0);
  std::cout << "CP FIFO capacity/wrap/breakpoint tests passed\n";
}
