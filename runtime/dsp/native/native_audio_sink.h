#pragma once
// SPDX-License-Identifier: GPL-3.0-or-later
#include <cstddef>
#include <cstdint>
#include <memory>

namespace GekkoAOT::DSP {

class NativeAudioSink final {
public:
  NativeAudioSink();
  ~NativeAudioSink();
  NativeAudioSink(const NativeAudioSink&) = delete;
  NativeAudioSink& operator=(const NativeAudioSink&) = delete;

  void Reset();
  void Flush();
  void PushStereoS16(const std::int16_t* interleaved, std::size_t frames,
                     std::uint32_t sample_rate_hz);
  std::uint64_t FramesSubmitted() const;
  bool Ready() const;
private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
} // namespace GekkoAOT::DSP
