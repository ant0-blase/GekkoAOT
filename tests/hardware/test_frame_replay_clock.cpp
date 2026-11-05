// SPDX-License-Identifier: GPL-3.0-or-later
#include "../../runtime/gx/frame_replay_clock.h"

#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

namespace {
int failures = 0;
void Expect(bool condition, const char* what) {
  if (!condition) { std::fprintf(stderr, "FAIL: %s\n", what); ++failures; }
}

// A 30 Hz game whose frames finish with +-4 ms of work jitter, presented at
// 120 Hz: the shown game time must advance by one present period every
// present (no judder), stay between the two newest frames and never stall.
void TestSmoothInterpolation() {
  GekkoAOT::GX::ReplayClock clock;
  std::mt19937 rng(7);
  std::uniform_real_distribution<double> jitter(-0.004, 0.004);
  const double step = 1.0 / 30.0, present = 1.0 / 120.0;
  std::vector<double> finishes;
  for (int i = 0; i < 400; ++i) finishes.push_back(i * step + 0.010 + jitter(rng));
  std::size_t next = 0;
  double previous_game = -1, worst_error = 0, previous_shown = -1;
  std::vector<double> games;
  for (double now = 0; now < 13.0; now += present) {
    while (next < finishes.size() && finishes[next] <= now) {
      games.push_back(clock.OnFrame(finishes[next]));
      ++next;
    }
    double t;
    if (!clock.Position(now, true, t) || games.size() < 2) continue;
    const double shown = games[games.size() - 2] + t * (games.back() - games[games.size() - 2]);
    if (now > 1.0 && previous_shown > 0) worst_error = std::max(worst_error, std::abs((shown - previous_shown) - present));
    previous_shown = shown;
    (void)previous_game;
  }
  Expect(clock.Stalls() == 0, "a jittered 30 Hz game never stalls the delayed clock");
  Expect(worst_error < 0.0015, "shown game time advances by one present period (+-1.5 ms) every present");
  // The old rule (alpha from finish times) for comparison: its step error is
  // the work jitter itself.
  Expect(std::abs(clock.Nominal() - step) < 0.002, "nominal step is the game's rate");
}

// A load (a 400 ms gap) resynchronizes instead of sliding.
void TestHitch() {
  GekkoAOT::GX::ReplayClock clock;
  double t = 0;
  for (int i = 0; i < 30; ++i) clock.OnFrame(t += 1.0 / 30.0);
  const auto before = clock.Resyncs();
  const double game = clock.OnFrame(t += 0.4);
  Expect(clock.Resyncs() == before + 1 && std::abs(game - t) < 1e-9, "a hitch resynchronizes to the finish time");
}

// Prediction (no delay) extrapolates at most one step past the newest frame.
void TestPrediction() {
  GekkoAOT::GX::ReplayClock clock;
  double t = 0;
  for (int i = 0; i < 10; ++i) clock.OnFrame(t += 1.0 / 30.0);
  double pos;
  Expect(clock.Position(t + 1.0 / 60.0, false, pos) && pos > 1.4 && pos < 1.6, "prediction half a step ahead");
  Expect(clock.Position(t + 1.0, false, pos) && pos == 2.0, "prediction capped one step ahead");
}
} // namespace

int main() {
  TestSmoothInterpolation();
  TestHitch();
  TestPrediction();
  if (failures == 0) std::puts("frame replay clock: ok");
  return failures == 0 ? 0 : 1;
}
