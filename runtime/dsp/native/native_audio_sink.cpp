// SPDX-License-Identifier: GPL-3.0-or-later
#include "dsp/native/native_audio_sink.h"

#include <cstdint>
#include <iostream>
#include <vector>

#if defined(GEKKOAOT_NATIVE_SDL3)
#include <SDL3/SDL.h>
#endif

namespace GekkoAOT::DSP {

struct NativeAudioSink::Impl {
  std::uint64_t frames_submitted = 0;
  std::uint32_t rate = 0;

#if defined(GEKKOAOT_NATIVE_SDL3)
  SDL_AudioStream* stream = nullptr;
  bool sdl_audio_owned = false;
  bool playback_started = false;
  std::vector<std::int16_t> staging;

  // AID exposes 8 stereo frames at a time (32 bytes). Feeding those directly
  // to a host device means ~4,000 tiny writes/s at 32 kHz and makes host
  // scheduling jitter audible as a repeating click/buzz. Keep the hardware
  // DMA cadence internally, but batch it before SDL and start playback only
  // after a small safety cushion has accumulated.
  static constexpr std::size_t kBatchFrames = 64;
  static constexpr std::uint32_t kPrebufferMs = 24;
  static constexpr std::uint32_t kEmergencyLowMs = 6;
  static constexpr std::uint32_t kMaxQueueMs = 250;

  void ResetBuffering() {
    staging.clear();
    playback_started = false;
  }

  bool Ensure(std::uint32_t hz) {
    if (hz == 0) return false;
    if (stream && rate == hz) return true;
    if (stream) {
      SDL_DestroyAudioStream(stream);
      stream = nullptr;
    }
    ResetBuffering();
    if ((SDL_WasInit(SDL_INIT_AUDIO) & SDL_INIT_AUDIO) == 0) {
      if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        static bool once = false;
        if (!once) {
          once = true;
          std::cerr << "GEKKOAOT_NATIVE_AUDIO_SDL3=0 error=\"" << SDL_GetError() << "\"\n";
        }
        return false;
      }
      sdl_audio_owned = true;
    }
    SDL_AudioSpec spec{};
    spec.format = SDL_AUDIO_S16;
    spec.channels = 2;
    spec.freq = static_cast<int>(hz);
    stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
    if (!stream) {
      static bool once = false;
      if (!once) {
        once = true;
        std::cerr << "GEKKOAOT_NATIVE_AUDIO_SDL3=0 error=\"" << SDL_GetError() << "\"\n";
      }
      return false;
    }
    SDL_PauseAudioStreamDevice(stream);
    rate = hz;
    staging.reserve(kBatchFrames * 2u * 4u);
    std::cout << "GEKKOAOT_NATIVE_AUDIO_SDL3=1 format=s16-stereo source_hz=" << hz
              << " buffering=v51.2-prebuffer\n";
    return true;
  }

  int BytesForMs(std::uint32_t ms) const {
    if (rate == 0) return 0;
    const std::uint64_t frames = (static_cast<std::uint64_t>(rate) * ms + 999u) / 1000u;
    return static_cast<int>(frames * 2u * sizeof(std::int16_t));
  }

  bool PutStaging(bool force) {
    if (!stream || staging.empty()) return true;
    const std::size_t frames = staging.size() / 2u;
    if (!force && frames < kBatchFrames) return true;
    const int bytes = static_cast<int>(staging.size() * sizeof(std::int16_t));
    if (!SDL_PutAudioStreamData(stream, staging.data(), bytes)) {
      static bool once = false;
      if (!once) {
        once = true;
        std::cerr << "GEKKOAOT_NATIVE_AUDIO_SDL3_PUT=0 error=\"" << SDL_GetError() << "\"\n";
      }
      return false;
    }
    staging.clear();
    return true;
  }

  void MaybeStartPlayback() {
    if (!stream || playback_started) return;
    if (SDL_GetAudioStreamQueued(stream) < BytesForMs(kPrebufferMs)) return;
    if (!SDL_ResumeAudioStreamDevice(stream)) return;
    playback_started = true;
    std::cout << "GEKKOAOT_NATIVE_AUDIO_BUFFER_V51_2=1 prebuffer_ms=" << kPrebufferMs
              << " batch_frames=" << kBatchFrames << " source_hz=" << rate << "\n";
  }
#endif

  ~Impl() {
#if defined(GEKKOAOT_NATIVE_SDL3)
    if (stream) SDL_DestroyAudioStream(stream);
    if (sdl_audio_owned) SDL_QuitSubSystem(SDL_INIT_AUDIO);
#endif
  }
};

NativeAudioSink::NativeAudioSink() : impl_(std::make_unique<Impl>()) {}
NativeAudioSink::~NativeAudioSink() = default;

void NativeAudioSink::Reset() {
  impl_->frames_submitted = 0;
  Flush();
}

void NativeAudioSink::Flush() {
#if defined(GEKKOAOT_NATIVE_SDL3)
  if (impl_->stream) {
    SDL_PauseAudioStreamDevice(impl_->stream);
    SDL_ClearAudioStream(impl_->stream);
  }
  impl_->ResetBuffering();
#endif
}

bool NativeAudioSink::Ready() const {
#if defined(GEKKOAOT_NATIVE_SDL3)
  return impl_->stream != nullptr;
#else
  return false;
#endif
}

std::uint64_t NativeAudioSink::FramesSubmitted() const { return impl_->frames_submitted; }

void NativeAudioSink::PushStereoS16(const std::int16_t* samples, std::size_t frames,
                                    std::uint32_t sample_rate_hz) {
  if (!samples || frames == 0 || sample_rate_hz == 0) return;
  impl_->frames_submitted += frames;

#if defined(GEKKOAOT_NATIVE_SDL3)
  if (!impl_->Ensure(sample_rate_hz)) return;

  impl_->staging.insert(impl_->staging.end(), samples, samples + frames * 2u);

  const int queued = SDL_GetAudioStreamQueued(impl_->stream);
  const bool emergency = impl_->playback_started && queued < impl_->BytesForMs(Impl::kEmergencyLowMs);
  if (!impl_->PutStaging(emergency)) return;

  if (!impl_->playback_started) {
    const int staged_bytes = static_cast<int>(impl_->staging.size() * sizeof(std::int16_t));
    if (SDL_GetAudioStreamQueued(impl_->stream) + staged_bytes >= impl_->BytesForMs(Impl::kPrebufferMs))
      impl_->PutStaging(true);
    impl_->MaybeStartPlayback();
  } else {
    const int now_queued = SDL_GetAudioStreamQueued(impl_->stream);
    if (now_queued > impl_->BytesForMs(Impl::kMaxQueueMs)) {
      SDL_PauseAudioStreamDevice(impl_->stream);
      SDL_ClearAudioStream(impl_->stream);
      impl_->ResetBuffering();
      std::cerr << "GEKKOAOT_NATIVE_AUDIO_BUFFER_V51_2_WARN queue_ms>"
                << Impl::kMaxQueueMs << " action=rebuffer\n";
    }
  }
#else
  (void)sample_rate_hz;
#endif
}
} // namespace GekkoAOT::DSP
