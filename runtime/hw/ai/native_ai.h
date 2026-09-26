#pragma once
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstdint>

namespace GekkoAOT::HW::AI {

// Standalone GameCube Audio Interface (AI) register block.
//
// This is the streaming/sample-counter side of Flipper audio. The DSP-side
// audio DMA registers at CC005030..CC00503A remain owned by NativeDSP. NativeAI
// owns CC006C00..CC006C0C and raises the PI audio interrupt directly.
class NativeAI final {
public:
  static constexpr std::uint32_t BasePhysical = 0x0c006c00u;
  static constexpr std::uint32_t WindowSize = 0x10u;

  NativeAI() { Reset(); }

  void Reset();
  bool Handles(std::uint32_t address) const;
  bool Read(std::uint32_t address, std::uint8_t size, std::uint64_t* value) const;
  bool Write(std::uint32_t address, std::uint64_t value, std::uint8_t size);
  void AdvanceCycles(std::uint64_t cycles);

  bool InterruptPending() const;
  bool IsPlaying() const { return (control_ & kPstat) != 0u; }
  std::uint32_t Control() const { return control_; }
  std::uint32_t Volume() const { return volume_; }
  std::uint32_t SampleCounter() const { return sample_counter_; }
  std::uint32_t InterruptTiming() const { return interrupt_timing_; }
  // Optical input rate selected by AISFR. SampleCounter advances at the
  // 48 kHz output rate after conversion.
  std::uint32_t StreamSampleRateHz() const;
  std::uint32_t DspSampleRateHz() const;

private:
  static constexpr std::uint32_t ToPhysical(std::uint32_t address) {
    return address & 0x3fffffffu;
  }

  static constexpr std::uint32_t kPstat = 1u << 0;
  static constexpr std::uint32_t kAisfr = 1u << 1;
  static constexpr std::uint32_t kAiIntMask = 1u << 2;
  static constexpr std::uint32_t kAiInt = 1u << 3;
  static constexpr std::uint32_t kAiIntValid = 1u << 4;
  static constexpr std::uint32_t kSampleCounterReset = 1u << 5;
  static constexpr std::uint32_t kAidfr = 1u << 6;
  static constexpr std::uint32_t kPersistentControlMask =
      kPstat | kAisfr | kAiIntMask | kAiIntValid | kAidfr;

  void AdvanceSamples(std::uint32_t samples);

  std::uint32_t control_ = 0;
  std::uint32_t volume_ = 0;
  std::uint32_t sample_counter_ = 0;
  std::uint32_t interrupt_timing_ = 0;
  std::uint64_t sample_phase_ = 0;
};

} // namespace GekkoAOT::HW::AI
