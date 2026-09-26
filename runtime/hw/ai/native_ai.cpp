// SPDX-License-Identifier: GPL-3.0-or-later
#include "hw/ai/native_ai.h"

#include <algorithm>
#include <limits>

namespace GekkoAOT::HW::AI {
namespace {
constexpr std::uint32_t kControl = 0x00u;
constexpr std::uint32_t kVolume = 0x04u;
constexpr std::uint32_t kSampleCounter = 0x08u;
constexpr std::uint32_t kInterruptTiming = 0x0cu;

// Retail GameCube clocks/rates. The named 48/32 kHz modes are slightly high
// on real hardware; keeping those rates makes the standalone counter useful
// without depending on Dolphin SystemTimers/Mixer.
constexpr std::uint64_t kCpuClockHz = 486000000ull;
constexpr std::uint32_t kStream48Hz = 48043u;
constexpr std::uint32_t kStream32Hz = 32028u;
constexpr std::uint32_t kDsp48Hz = 48043u;
constexpr std::uint32_t kDsp32Hz = 32028u;
}

void NativeAI::Reset() {
  // SDK-visible reset state used by retail software: AIS at 48 kHz, AID at
  // 32 kHz, playback/counter interrupt disabled.
  control_ = kAisfr | kAidfr;
  volume_ = 0;
  sample_counter_ = 0;
  interrupt_timing_ = 0;
  sample_phase_ = 0;
}

bool NativeAI::Handles(std::uint32_t address) const {
  const std::uint32_t physical = ToPhysical(address);
  return physical >= BasePhysical && physical < BasePhysical + WindowSize;
}

bool NativeAI::Read(std::uint32_t address, std::uint8_t size, std::uint64_t* value) const {
  if (!value || !Handles(address) || size != 4u) return false;
  const std::uint32_t offset = ToPhysical(address) - BasePhysical;
  if ((offset & 3u) != 0u) return false;

  switch (offset) {
  case kControl: *value = control_; return true;
  case kVolume: *value = volume_; return true;
  case kSampleCounter: *value = sample_counter_; return true;
  case kInterruptTiming: *value = interrupt_timing_; return true;
  default: return false;
  }
}

bool NativeAI::Write(std::uint32_t address, std::uint64_t value, std::uint8_t size) {
  if (!Handles(address) || size != 4u) return false;
  const std::uint32_t offset = ToPhysical(address) - BasePhysical;
  if ((offset & 3u) != 0u) return false;
  const std::uint32_t v = static_cast<std::uint32_t>(value);

  switch (offset) {
  case kControl: {
    const bool old_stream_rate = (control_ & kAisfr) != 0u;
    // AIINT is write-one-to-clear. All other persistent control bits are
    // ordinary state; SCRESET is a write pulse and never latches.
    if (v & kAiInt) control_ &= ~kAiInt;
    const std::uint32_t cause = control_ & kAiInt;
    control_ = cause | (v & kPersistentControlMask);
    if (v & kSampleCounterReset) {
      sample_counter_ = 0;
      sample_phase_ = 0;
    }
    if (old_stream_rate != ((control_ & kAisfr) != 0u))
      sample_phase_ = 0;
    return true;
  }
  case kVolume:
    volume_ = v & 0x0000ffffu;
    return true;
  case kSampleCounter:
    sample_counter_ = v;
    sample_phase_ = 0;
    return true;
  case kInterruptTiming:
    interrupt_timing_ = v;
    return true;
  default:
    return false;
  }
}

std::uint32_t NativeAI::StreamSampleRateHz() const {
  return (control_ & kAisfr) ? kStream48Hz : kStream32Hz;
}

std::uint32_t NativeAI::DspSampleRateHz() const {
  // AIDFR is intentionally inverted versus AISFR on GameCube.
  return (control_ & kAidfr) ? kDsp32Hz : kDsp48Hz;
}

bool NativeAI::InterruptPending() const {
  return (control_ & kAiInt) != 0u && (control_ & kAiIntMask) != 0u;
}

void NativeAI::AdvanceSamples(std::uint32_t samples) {
  if (samples == 0u) return;

  const std::uint32_t old_plus_one = sample_counter_ + 1u;
  sample_counter_ += samples;

  // A zero trigger disables the streamed sample count interrupt. Without
  // this guard, a wrapping 32-bit counter spuriously latches AIINT at zero.
  if (interrupt_timing_ == 0u) return;

  // Match a programmed threshold across a possibly wrapping sample interval.
  // AIINTVLD is retained as hardware-visible state; the sample-timing event
  // itself still latches AIINT when the programmed counter value is crossed.
  const std::uint32_t until_interrupt = interrupt_timing_ - old_plus_one;
  const std::uint32_t traversed = sample_counter_ - old_plus_one;
  if (until_interrupt <= traversed)
    control_ |= kAiInt;
}

void NativeAI::AdvanceCycles(std::uint64_t cycles) {
  if (!IsPlaying() || cycles == 0u) return;

  // The stream counter is downstream of the 32-to-48 kHz converter. Its
  // clock remains at the output rate even when AISFR selects 32 kHz input.
  const std::uint64_t rate = kStream48Hz;
  const std::uint64_t whole_seconds = cycles / kCpuClockHz;
  const std::uint64_t remainder_cycles = cycles % kCpuClockHz;

  std::uint64_t samples = whole_seconds * rate;
  const std::uint64_t phase = sample_phase_ + remainder_cycles * rate;
  samples += phase / kCpuClockHz;
  sample_phase_ = phase % kCpuClockHz;

  while (samples != 0u) {
    const std::uint32_t chunk = static_cast<std::uint32_t>(
        std::min<std::uint64_t>(samples, std::numeric_limits<std::uint32_t>::max()));
    AdvanceSamples(chunk);
    samples -= chunk;
  }
}

} // namespace GekkoAOT::HW::AI
