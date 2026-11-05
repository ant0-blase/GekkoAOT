// SPDX-License-Identifier: GPL-3.0-or-later
#include "audio/native_ai_playback.h"
#include <cstdlib>
#include <iostream>
#include <vector>
#define CHECK(x)                                                               \
  do {                                                                         \
    if (!(x)) {                                                                \
      std::cerr << __LINE__ << ": " #x "\n";                                   \
      std::abort();                                                            \
    }                                                                          \
  } while (false)
using GekkoAOT::Audio::NativeAiPlayback;
int main() {
  NativeAiPlayback p;
  std::array<std::uint8_t, 640> a{}, b{};
  for (unsigned i = 0; i < 160; ++i) {
    a[4 * i] = 0xfe;
    a[4 * i + 1] = 0xdc;
    a[4 * i + 2] = 0x12;
    a[4 * i + 3] = 0x34;
    b[4 * i] = 0x11;
    b[4 * i + 1] = 0x11;
    b[4 * i + 2] = 0x22;
    b[4 * i + 3] = 0x22;
  }
  CHECK(!p.Bind(1, a));
  CHECK(!p.Bind(0x1000, std::span(a).first(639)));
  p.Enable();
  CHECK(p.Bind(0x1000, a));
  p.Start();
  CHECK(p.CallbackPending());
  unsigned frames = 0, calls = 0;
  std::int16_t expected_left = 0x1234,
               expected_right = static_cast<std::int16_t>(0xfedc);
  const auto sink = [&](const std::int16_t *samples, unsigned n,
                        unsigned rate) {
    CHECK(rate == 32028 || rate == 48043);
    for (unsigned i = 0; i < n; ++i) {
      CHECK(samples[2 * i] == expected_left);
      CHECK(samples[2 * i + 1] == expected_right);
    }
    frames += n;
    ++calls;
  };
  // The DAC keeps consuming real elapsed frames while a callback is pending.
  // Missing a callback coalesces the notification; it never pauses playback.
  p.Advance(2430000, 32028, sink); // 5 ms: one 160-frame buffer
  CHECK(frames == 160 && calls == 1 && p.CallbackPending());
  // The just-completed boundary already latched A. Rebinding selects B for
  // the following boundary, so the next 160 frames must still come from A.
  CHECK(p.Bind(0x2000, b));
  p.Advance(2430000, 32028, sink);
  CHECK(frames == 320 && p.CallbackPending());
  expected_left = 0x2222;
  expected_right = 0x1111;
  p.Acknowledge();
  p.Advance(2430000, 32028, sink);
  CHECK(frames == 480 && p.CallbackPending());
  // Callback latency must not make the audio clock slow or emit overdue
  // samples in a catch-up burst on acknowledgement.
  p.Advance(24300000, 32028, sink); // another 50 ms, no callback servicing
  CHECK(frames == 2081 && p.CallbackPending());
  const auto completions = p.Completions();
  p.Acknowledge();p.Advance(1,32028,sink);
  CHECK(frames == 2081 && !p.CallbackPending() && p.Completions()==completions);
  p.Stop();
  const auto before = frames;
  p.Advance(486000000, 32028, sink);
  CHECK(frames == before);
  // Fractional rates and arbitrarily partitioned host elapsed time produce the
  // same PCM/callback boundaries. No FIFO-sized 8-frame stepping is required.
  NativeAiPlayback whole, split;
  whole.Enable();
  split.Enable();
  CHECK(whole.Bind(0x1000, a));
  CHECK(split.Bind(0x1000, a));
  whole.Start();
  split.Start();
  whole.Acknowledge();
  split.Acknowledge();
  unsigned wf = 0, sf = 0;
  whole.Advance(2000000, 48043, [&](auto *, auto n, auto) { wf += n; });
  for (unsigned i = 0; i < 2000; ++i)
    split.Advance(1000, 48043, [&](auto *, auto n, auto) { sf += n; });
  CHECK(wf == sf && whole.CallbackPending() == split.CallbackPending() &&
        whole.BytesLeft() == split.BytesLeft());
  NativeAiPlayback zero;
  zero.Enable();
  CHECK(zero.Bind(0, {}));
  zero.Start();
  zero.Acknowledge();
  zero.Advance(486000000, 32028, [&](auto *, auto, auto) { std::abort(); });
  CHECK(!zero.Failed());
  std::cout << "NativeAI PCM: BE R/L conversion, buffer latch order, callback "
               "coalescing, continuous cadence, stop, zero payload, fractional clock and partition "
               "invariance PASS\n";
}
