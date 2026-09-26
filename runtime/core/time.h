#pragma once
#include <chrono>
#include <cstdint>
#include <limits>
namespace GekkoAOT::Time {
constexpr std::uint64_t GameCubeTimebaseHz = 40500000;
constexpr std::uint64_t CpuCyclesPerTick = 12;
// Host elapsed time is useful for telemetry, not a substitute for guest virtual time.
inline std::uint64_t FromNanoseconds(std::uint64_t ns) {
  const auto seconds = ns / 1000000000;
  const auto fraction = (ns % 1000000000) * GameCubeTimebaseHz / 1000000000;
  if (seconds > (std::numeric_limits<std::uint64_t>::max() - fraction) / GameCubeTimebaseHz)
    return std::numeric_limits<std::uint64_t>::max();
  return seconds * GameCubeTimebaseHz + fraction;
}
class Monotonic {
  std::chrono::steady_clock::time_point origin_ = std::chrono::steady_clock::now();
public:
  std::uint64_t ElapsedTicks() const {
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - origin_).count();
    return FromNanoseconds(ns > 0 ? static_cast<std::uint64_t>(ns) : 0);
  }
};
template<class Context> bool GetTime(Context* state) {
  if (!state) return false;
  state->gpr[3] = static_cast<std::uint32_t>(state->timebase >> 32);
  state->gpr[4] = static_cast<std::uint32_t>(state->timebase);
  state->pc = state->lr;
  return true;
}
template<class Context> bool GetTick(Context* state) {
  if (!state) return false;
  state->gpr[3] = static_cast<std::uint32_t>(state->timebase);
  state->pc = state->lr;
  return true;
}
}  // namespace GekkoAOT::Time
