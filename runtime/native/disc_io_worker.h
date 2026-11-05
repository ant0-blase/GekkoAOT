// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

namespace GekkoAOT::Native {

// One runtime thread submits/collects requests; one persistent worker owns
// media I/O. No lock, allocation or thread creation in Submit/TryCollect.
// The result buffer stays private until collection, so cancellation never
// races writes to a destination the game has already reused.
class DiscIoWorker final {
public:
  using Reader = bool (*)(void *, std::uint64_t, void *, std::uint32_t);
  struct Result {
    std::uint64_t sequence = 0;
    bool ok = false;
  };
  DiscIoWorker() = default;
  ~DiscIoWorker() { Stop(); }
  DiscIoWorker(const DiscIoWorker &) = delete;
  DiscIoWorker &operator=(const DiscIoWorker &) = delete;

  void Start(void *context, Reader reader);
  void Stop();
  bool Submit(std::uint64_t sequence, std::uint64_t offset,
              std::uint32_t bytes);
  bool TryCollect(Result &result, std::vector<std::uint8_t> &bytes);
  void Drain();
  bool WaitCollect(Result &, std::vector<std::uint8_t> &);
  bool Idle() const {
    return state_.load(std::memory_order_acquire) == State::Idle;
  }
  std::uint64_t WorkerStarts() const { return starts_; }
  // A submitted read has not completed yet.
  bool Busy() const {
    const auto state = state_.load(std::memory_order_acquire);
    return state == State::Queued || state == State::Reading;
  }
  // Let the idle guest sleep exactly until the in-flight read lands
  // (or `until`), instead of until the next VI/maintenance deadline.
  bool WaitUntilComplete(std::chrono::steady_clock::time_point until);

private:
  enum class State : unsigned { Idle, Queued, Reading, Complete, Stopped };
  void Run();
  std::atomic<State> state_{State::Stopped};
  std::atomic<bool> stop_{false};
  std::thread worker_;
  void *context_ = nullptr;
  Reader reader_ = nullptr;
  std::uint64_t sequence_ = 0, offset_ = 0, starts_ = 0;
  std::uint64_t submitted_ns_ = 0;
  std::uint32_t size_ = 0;
  bool ok_ = false;
  std::vector<std::uint8_t> buffer_;
  std::mutex wake_mutex_;
  std::condition_variable wake_;
};
} // namespace GekkoAOT::Native
