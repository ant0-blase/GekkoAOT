#pragma once
// SPDX-License-Identifier: GPL-3.0-or-later
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

namespace GekkoAOT::GX {
// Frame-unlock presentation clock. Guest frames finish at their work's whim
// (a few ms of jitter around a fixed game rate); deriving the blend position
// from those finish times makes every in-between frame judder. Instead:
// - each guest frame gets a *game time*: its predecessor's plus the nominal
//   step (mean of recent steps), pulled 10% toward its finish time; a frame
//   more than 1.5 steps off (a load, a real slowdown) is taken as it is;
// - presents show game time G = now - delay, the delay being one step plus the
//   mean recent lateness (finish time past its game time), so G stays around
//   the two newest frames until the next one arrives. Only the newest frame's
//   geometry exists, so G may run a quarter step past either frame (a light
//   extrapolation of its uniforms) instead of holding or jumping on jitter;
// - G follows that target at a speed within +-5% (a delay change never shows
//   as a jump), never runs backwards and never passes the newest frame (a
//   stall holds).
// Times are seconds on one monotonic clock.
class ReplayClock {
public:
  static constexpr std::size_t History = 32, LatenessHistory = 90;

  void Reset() { *this = ReplayClock{}; }

  // A guest frame finished at `finish`; returns its game time.
  double OnFrame(double finish) {
    if (frames_ == 0) {
      newest_ = finish;
    } else {
      const double step = finish - last_finish_;
      if (step > 0.0005 && step < 0.25) steps_[steps_count_++ % History] = step;
      const double nominal = Nominal();
      const double expected = newest_ + nominal;
      if (nominal <= 0 || finish - expected > 1.5 * nominal || finish - expected < -1.5 * nominal) {
        newest_ = finish; // a hitch or a load: resynchronize
        ++resyncs_;
      } else {
        newest_ = expected + 0.1 * (finish - expected);
      }
      lateness_[lateness_count_++ % LatenessHistory] = finish - newest_;
    }
    previous_game_ = frames_ ? previous_newest_ : newest_;
    previous_newest_ = newest_;
    last_finish_ = finish;
    ++frames_;
    return newest_;
  }

  double Nominal() const {
    const std::size_t n = std::min(steps_count_, History);
    if (n == 0) return 0;
    double sum = 0;
    for (std::size_t i = 0; i < n; ++i) sum += steps_[i];
    return sum / double(n);
  }
  double Delay() const {
    const std::size_t n = std::min(lateness_count_, LatenessHistory);
    double sum = 0;
    for (std::size_t i = 0; i < n; ++i) sum += lateness_[i];
    return Nominal() + (n ? sum / double(n) : 0.0);
  }

  // Blend position between the two newest frames for a present at `now`:
  // 0 = previous frame, 1 = newest. Interpolation (delayed by Delay()) stays
  // in [-0.25, 1.25]; prediction (no delay) extrapolates up to one step ahead.
  // False until two frames exist.
  bool Position(double now, bool interpolate, double& t) {
    if (frames_ < 2) return false;
    const double step = newest_ - previous_game_;
    if (!(step > 0)) return false;
    const double target = interpolate ? now - Delay() : now;
    double g = target;
    if (have_shown_) {
      const double dt = now - last_now_;
      const double error = target - (shown_ + dt);
      g = std::abs(error) > 0.1 ? target : shown_ + dt * (1.0 + std::clamp(error / 0.5, -0.05, 0.05));
      g = std::max(g, shown_);               // never backwards
    }
    const double limit = newest_ + (interpolate ? 0.25 : 1.0) * step;
    if (g > limit) { g = limit; ++stalls_; } // the next frame is late: hold
    shown_ = g; last_now_ = now; have_shown_ = true;
    t = (g - previous_game_) / step;
    t = std::clamp(t, interpolate ? -0.25 : 0.0, interpolate ? 1.25 : 2.0);
    return true;
  }

  std::uint64_t Resyncs() const { return resyncs_; }
  std::uint64_t Stalls() const { return stalls_; }

private:
  std::array<double, History> steps_{};
  std::array<double, LatenessHistory> lateness_{};
  std::size_t steps_count_ = 0, lateness_count_ = 0;
  std::uint64_t frames_ = 0, resyncs_ = 0, stalls_ = 0;
  double newest_ = 0, previous_newest_ = 0, previous_game_ = 0, last_finish_ = 0, shown_ = 0, last_now_ = 0;
  bool have_shown_ = false;
};
} // namespace GekkoAOT::GX
