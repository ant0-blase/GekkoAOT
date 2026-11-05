#pragma once
// SPDX-License-Identifier: GPL-3.0-or-later
#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <span>
namespace GekkoAOT::Audio {
// Host PCM buffer playback, independent of the DSP/AI register adapters. AOT
// binds a real RAM span. Elapsed host audio time consumes frames directly;
// buffer completion requests the SDK callback, never a simulated DMA IRQ.
class NativeAiPlayback final {
public:
  void Reset() { *this = {}; }
  void Enable() { owned_ = true; }
  bool Owned() const { return owned_; }
  bool Playing() const { return playing_; }
  bool Failed() const { return failed_; }
  bool CallbackPending() const { return callback_pending_; }
  std::uint64_t Completions() const { return completions_; }
  std::uint32_t Address() const { return next_.address; }
  std::uint32_t Length() const { return next_.bytes; }
  std::uint32_t BytesLeft() const {
    return ((current_.bytes - cursor_ + 31u) / 32u) * 32u;
  }
  bool Bind(std::uint32_t address, std::span<const std::uint8_t> bytes) {
    if ((address & 31u) || (bytes.size() & 31u) || bytes.size() > 0x7fff * 32u)
      return false;
    next_ = {bytes.data(), address, static_cast<std::uint32_t>(bytes.size())};
    return true;
  }
  void Program(std::uint32_t address, const std::uint8_t *data,
               std::uint32_t bytes) {
    next_ = {data, address, bytes};
  }
  void Start() {
    if (playing_)
      return;
    playing_ = true;
    failed_ = false;
    phase_ = debt_ = 0;
    Latch();
    callback_pending_ = true;
    ++completions_;
  }
  void Stop() { playing_ = false; }
  void Acknowledge() {
    callback_pending_ = false;
    if (!current_.bytes && next_.bytes)
      Latch();
  }
  std::uint64_t CyclesUntilEvent(std::uint32_t rate) const {
    if (!playing_ || !current_.bytes || !rate)
      return std::numeric_limits<std::uint64_t>::max();
    const auto frames = (current_.bytes - cursor_) / 4;
    if (debt_ >= frames)
      return 1;
    const auto target = (frames - debt_) * ClockHz;
    return std::max<std::uint64_t>(1, (target - phase_ + rate - 1) / rate);
  }
  template <class Sink>
  void Advance(std::uint64_t cycles, std::uint32_t rate, Sink &&sink) {
    if (!owned_ || !playing_ || !rate || !cycles)
      return;
    debt_ += (cycles / ClockHz) * rate;
    const auto scaled = phase_ + (cycles % ClockHz) * rate;
    debt_ += scaled / ClockHz;
    phase_ = scaled % ClockHz;
    if (!current_.bytes) { debt_ = 0; return; }
    // Playback follows elapsed host audio time even if a game masks or delays
    // its callback. A completion is a coalescing notification, not permission
    // to stop the DAC or accumulate a later burst of overdue audio.
    while (debt_) {
      if (!current_.data) {
        failed_ = true;
        playing_ = false;
        return;
      }
      const auto frames = static_cast<unsigned>(std::min<std::uint64_t>(
          {debt_, (current_.bytes - cursor_) / 4, 512}));
      if (!frames) { debt_ = 0; return; }
      const auto *source = current_.data + cursor_;
      for (unsigned i = 0; i < frames; ++i) {
        pcm_[2 * i] = static_cast<std::int16_t>(
            (std::uint16_t(source[4 * i + 2]) << 8) | source[4 * i + 3]);
        pcm_[2 * i + 1] = static_cast<std::int16_t>(
            (std::uint16_t(source[4 * i]) << 8) | source[4 * i + 1]);
      }
      sink(pcm_.data(), frames, rate);
      cursor_ += frames * 4;
      debt_ -= frames;
      if (cursor_ == current_.bytes) {
        // The next buffer is latched BEFORE its predecessor's callback. This
        // preserves retail double/triple-buffer ordering without a DMA engine.
        Latch();
        callback_pending_ = true;
        ++completions_;
      }
    }
  }

private:
  static constexpr std::uint64_t ClockHz = 486000000;
  struct Buffer {
    const std::uint8_t *data = nullptr;
    std::uint32_t address = 0, bytes = 0;
  };
  void Latch() {
    current_ = next_;
    cursor_ = 0;
  }
  Buffer next_, current_;
  bool owned_ = false, playing_ = false, failed_ = false,
       callback_pending_ = false;
  std::uint32_t cursor_ = 0;
  std::uint64_t phase_ = 0, debt_ = 0, completions_ = 0;
  std::array<std::int16_t, 1024> pcm_{};
};
} // namespace GekkoAOT::Audio
