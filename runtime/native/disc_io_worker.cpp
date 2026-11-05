// SPDX-License-Identifier: GPL-3.0-or-later
#include "native/disc_io_worker.h"
#include "perf/latency.h"
#include "perf/capture.h"

namespace GekkoAOT::Native {
void DiscIoWorker::Start(void *context, Reader reader) {
  Stop();
  context_ = context;
  reader_ = reader;
  stop_.store(false, std::memory_order_relaxed);
  state_.store(State::Idle, std::memory_order_release);
  worker_ = std::thread(&DiscIoWorker::Run, this);
  ++starts_;
}
void DiscIoWorker::Stop() {
  if (!worker_.joinable())
    return;
  stop_.store(true, std::memory_order_release);
  // Wake an idle worker; an in-flight read is allowed to finish before its
  // media/context is destroyed. Only control paths join the worker.
  state_.exchange(State::Stopped, std::memory_order_acq_rel);
  state_.notify_all();
  { std::lock_guard<std::mutex> lock(wake_mutex_); }
  wake_.notify_all();
  worker_.join();
  state_.store(State::Stopped, std::memory_order_release);
}
bool DiscIoWorker::Submit(std::uint64_t sequence, std::uint64_t offset,
                          std::uint32_t bytes) {
  if (!Idle() || stop_.load(std::memory_order_relaxed) || !reader_)
    return false;
  sequence_ = sequence;
  submitted_ns_ = Perf::Enabled() ? Perf::Now() : 0;
  offset_ = offset;
  size_ = bytes;
  state_.store(State::Queued, std::memory_order_release);
  state_.notify_one();
  return true;
}
bool DiscIoWorker::TryCollect(Result &result,
                              std::vector<std::uint8_t> &bytes) {
  if (state_.load(std::memory_order_acquire) != State::Complete)
    return false;
  result = {sequence_, ok_};
  if(submitted_ns_)Perf::Record(Perf::Metric::IoWait,Perf::Now()-submitted_ns_);
  buffer_.swap(bytes); // both capacities are retained and reused
  state_.store(State::Idle, std::memory_order_release);
  return true;
}
bool DiscIoWorker::WaitCollect(Result &result,
                               std::vector<std::uint8_t> &bytes) {
  Capture::Scope capture(Capture::Kind::DiscWait);
  auto state = state_.load(std::memory_order_acquire);
  while (state == State::Queued || state == State::Reading) {
    state_.wait(state, std::memory_order_acquire);
    state = state_.load(std::memory_order_acquire);
  }
  return TryCollect(result, bytes);
}
void DiscIoWorker::Drain() {
  Capture::Scope capture(Capture::Kind::DiscWait);
  auto state = state_.load(std::memory_order_acquire);
  while (state == State::Queued || state == State::Reading) {
    state_.wait(state, std::memory_order_acquire);
    state = state_.load(std::memory_order_acquire);
  }
  if (state == State::Complete)
    state_.store(State::Idle, std::memory_order_release);
}
void DiscIoWorker::Run() {
  for (;;) {
    auto state = state_.load(std::memory_order_acquire);
    if (stop_.load(std::memory_order_acquire))
      return;
    if (state != State::Queued) {
      state_.wait(state, std::memory_order_acquire);
      continue;
    }
    state_.store(State::Reading, std::memory_order_release);
    try {
      buffer_.resize(size_);
      Capture::Scope capture(Capture::Kind::DiscRead, sequence_, size_);
      ok_ = reader_(context_, offset_, buffer_.data(), size_);
      if (!ok_)
        buffer_.clear();
    } catch (...) {
      ok_ = false;
      buffer_.clear();
    }
    state_.store(State::Complete, std::memory_order_release);
    state_.notify_all();
    { std::lock_guard<std::mutex> lock(wake_mutex_); }
    wake_.notify_all();
  }
}
bool DiscIoWorker::WaitUntilComplete(std::chrono::steady_clock::time_point until) {
  std::unique_lock<std::mutex> lock(wake_mutex_);
  return wake_.wait_until(lock, until, [&] { return !Busy(); });
}
} // namespace GekkoAOT::Native
