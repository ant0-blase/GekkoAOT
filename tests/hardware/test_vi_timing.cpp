// SPDX-License-Identifier: GPL-3.0-or-later
#include "hw/vi/native_vi.h"

#include <cstdint>
#include <cstdlib>
#include <iostream>

namespace {
constexpr std::uint32_t kViBase = 0xcc002000u;

void Check(bool condition, const char* expression, int line) {
  if (condition) return;
  std::cerr << "VI timing check failed at line " << line << ": " << expression << '\n';
  std::exit(1);
}

#define CHECK(expression) Check((expression), #expression, __LINE__)

std::uint32_t Read32(const GekkoAOT::HW::VI::NativeVI& vi, std::uint32_t offset) {
  std::uint64_t value = 0;
  CHECK(vi.Read(kViBase + offset, 4, &value));
  return static_cast<std::uint32_t>(value);
}

std::uint16_t Read16(const GekkoAOT::HW::VI::NativeVI& vi, std::uint32_t offset) {
  std::uint64_t value = 0;
  CHECK(vi.Read(kViBase + offset, 2, &value));
  return static_cast<std::uint16_t>(value);
}
} // namespace

int main() {
  GekkoAOT::HW::VI::NativeVI vi;
  CHECK(vi.Write(kViBase + 0x02, 0x0002, 2)); // Reset and stop beam.
  CHECK(vi.Write(kViBase + 0x00, 0x0010, 2)); // One active line per field.
  CHECK(vi.Write(kViBase + 0x0c, 0, 4));
  CHECK(vi.Write(kViBase + 0x10, 0, 4));
  CHECK(vi.Write(kViBase + 0x06, 100, 2));    // 100 samples per half-line.
  CHECK(vi.Write(kViBase + 0x30, 0x10010019, 4)); // VCT 1, HCT 25.
  CHECK(vi.Write(kViBase + 0x34, 0x1001007d, 4)); // VCT 1, HCT 125.
  CHECK(vi.Write(kViBase + 0x38, 0x10020001, 4)); // VCT 2, HCT 1.
  CHECK(vi.Write(kViBase + 0x02, 0x0001, 2)); // Start beam.

  const std::uint32_t half_line_cycles = vi.TicksPerHalfLine();
  CHECK(half_line_cycles == 1800);
  const std::uint32_t sample_cycles = half_line_cycles / 100;

  // HCT compares the current sample, so the first interrupt is not visible
  // until sample 25 actually begins. A half-line-only comparator misses it.
  vi.AdvanceCycles(24 * sample_cycles - 1);
  CHECK(Read16(vi, 0x2e) == 24);
  CHECK(!vi.InterruptPending());
  vi.AdvanceCycles(1);
  CHECK(Read16(vi, 0x2e) == 25);
  CHECK(vi.InterruptPending());
  CHECK((Read32(vi, 0x30) & 0x80000000u) != 0);
  CHECK(vi.ConsumeInterruptUpdate());
  CHECK(vi.Write(kViBase + 0x30, 0x1001, 2)); // ACK by clearing status.
  CHECK(!vi.InterruptPending());
  vi.AdvanceCycles(10);
  CHECK(!vi.InterruptPending()); // Do not refire within the same sample.

  vi.AdvanceCycles(half_line_cycles - 24 * sample_cycles - 10);
  CHECK(vi.HalfLineCount() == 1);
  CHECK(Read16(vi, 0x2e) == 101);
  CHECK(!vi.InterruptPending());
  vi.AdvanceCycles(24 * sample_cycles - 1);
  CHECK(Read16(vi, 0x2e) == 124);
  CHECK(!vi.InterruptPending());
  vi.AdvanceCycles(1);
  CHECK(Read16(vi, 0x2e) == 125);
  CHECK(vi.InterruptPending());
  CHECK((Read32(vi, 0x34) & 0x80000000u) != 0);
  CHECK(vi.Write(kViBase + 0x34, 0x1001, 2));
  CHECK(!vi.InterruptPending());

  // HCT 1 fires at the exact next line boundary. The two fields still have
  // distinct boundaries, and the first target may fire again next frame.
  vi.AdvanceCycles(half_line_cycles - 24 * sample_cycles);
  CHECK(vi.HalfLineCount() == 2);
  CHECK(Read16(vi, 0x2c) == 2 && Read16(vi, 0x2e) == 1);
  CHECK((Read32(vi, 0x38) & 0x80000000u) != 0);
  CHECK(vi.ConsumeOddFieldBoundary());
  CHECK(!vi.ConsumeEvenFieldBoundary());
  CHECK(vi.Write(kViBase + 0x38, 0x1002, 2));
  vi.AdvanceCycles(2ull * half_line_cycles);
  CHECK(vi.HalfLineCount() == 0);
  CHECK(vi.ConsumeEvenFieldBoundary() && vi.ConsumeFrameBoundary());
  vi.AdvanceCycles(24 * sample_cycles);
  CHECK((Read32(vi, 0x30) & 0x80000000u) != 0);

  std::cout << "VI horizontal target, ACK and field timing passed\n";
}
