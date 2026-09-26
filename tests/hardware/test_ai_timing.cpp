// SPDX-License-Identifier: GPL-3.0-or-later
#include "hw/ai/native_ai.h"

#include <cstdint>
#include <cstdlib>
#include <iostream>

#define CHECK(condition) do { if (!(condition)) { \
  std::cerr << __LINE__ << ": " #condition "\n"; std::exit(1); \
} } while (false)

using GekkoAOT::HW::AI::NativeAI;

namespace {
constexpr std::uint32_t kControl = NativeAI::BasePhysical;
constexpr std::uint32_t kCounter = kControl + 8u;
constexpr std::uint32_t kTrigger = kControl + 12u;
constexpr std::uint32_t kPlaying = 0x43u; // playback, 48 kHz stream, 32 kHz DSP
constexpr std::uint32_t kInterruptMask = 0x04u;
constexpr std::uint32_t kInterruptCause = 0x08u;
constexpr std::uint32_t kCounterReset = 0x20u;
constexpr std::uint64_t kCpuClockHz = 486000000ull;
constexpr std::uint64_t kStreamClockHz = 48043ull;

void Write(NativeAI& ai, std::uint32_t address, std::uint32_t value) {
  CHECK(ai.Write(address, value, 4));
}

std::uint64_t CyclesForSamples(std::uint64_t samples) {
  return (samples * kCpuClockHz + kStreamClockHz - 1u) / kStreamClockHz;
}
} // namespace

int main() {
  NativeAI ai;
  CHECK(ai.StreamSampleRateHz() == 48043u);
  CHECK(ai.DspSampleRateHz() == 32028u);
  CHECK(!ai.IsPlaying());

  // The optical stream counter advances on the AI clock, including when its
  // CPU interrupt is masked. The latched cause becomes visible on unmask.
  Write(ai, kTrigger, 3u);
  Write(ai, kControl, kPlaying);
  const auto third_sample = CyclesForSamples(3u);
  ai.AdvanceCycles(CyclesForSamples(2u));
  CHECK(ai.SampleCounter() == 2u);
  CHECK((ai.Control() & kInterruptCause) == 0u);
  ai.AdvanceCycles(third_sample - CyclesForSamples(2u));
  CHECK(ai.SampleCounter() == 3u);
  CHECK((ai.Control() & kInterruptCause) != 0u);
  CHECK(!ai.InterruptPending());
  Write(ai, kControl, kPlaying | kInterruptMask);
  CHECK(ai.InterruptPending());
  Write(ai, kControl, kPlaying | kInterruptMask | kInterruptCause);
  CHECK(!ai.InterruptPending());

  // SDK AISetStreamTrigger(0) disables the trigger. A zero-valued threshold
  // must not fire merely because the 32-bit count rolls through zero.
  Write(ai, kTrigger, 0u);
  Write(ai, kCounter, 0xfffffffeu);
  ai.AdvanceCycles(CyclesForSamples(2u));
  CHECK(!ai.InterruptPending());
  CHECK((ai.Control() & kInterruptCause) == 0u);

  // The counter reset is a pulse. Stopping playback freezes the counter;
  // elapsed CPU cycles divided into host-sized chunks preserve the count.
  Write(ai, kControl, kPlaying | kInterruptMask | kCounterReset);
  CHECK(ai.SampleCounter() == 0u);
  CHECK((ai.Control() & kCounterReset) == 0u);
  Write(ai, kControl, kPlaying & ~1u);
  ai.AdvanceCycles(kCpuClockHz);
  CHECK(ai.SampleCounter() == 0u);

  NativeAI split;
  Write(ai, kControl, kPlaying);
  Write(split, kControl, kPlaying);
  ai.AdvanceCycles(kCpuClockHz);
  const auto frame_cycles = kCpuClockHz / 60u;
  for (int frame = 0; frame < 59; ++frame) split.AdvanceCycles(frame_cycles);
  split.AdvanceCycles(kCpuClockHz - 59u * frame_cycles);
  CHECK(ai.SampleCounter() == 48043u);
  CHECK(split.SampleCounter() == ai.SampleCounter());

  // The optical stream's 32 kHz input is converted before the sample
  // counter. The SDK documents a 48 kHz count at this point in the path.
  NativeAI converted;
  Write(converted, kControl, kPlaying & ~0x02u);
  CHECK(converted.StreamSampleRateHz() == 32028u);
  converted.AdvanceCycles(kCpuClockHz);
  CHECK(converted.SampleCounter() == 48043u);
  std::cout << "AI stream counter, SRC, trigger, IRQ and cycle partition tests passed\n";
}
