// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
namespace GekkoAOT::GX {
struct PeEvent { std::uint32_t reg=0,value=0; };
// Parser notifications cross a thread boundary. A latest-value mailbox loses
// intermediate draw-sync tokens; retain a bounded FIFO until the CPU consumes
// them. The mutex is used only for PE events, never for ordinary primitives.
class PeEventQueue {
public:
  static constexpr std::size_t Capacity=8192;
  bool Pending() const {return count_.load(std::memory_order_acquire)!=0;}
  bool Push(PeEvent event) {
    std::lock_guard lock(mutex_);
    const auto count=count_.load(std::memory_order_relaxed);
    if(count==Capacity) return false;
    events_[(head_+count)%Capacity]=event;
    count_.store(count+1,std::memory_order_release);return true;
  }
  bool Pop(PeEvent* event) {
    if(!event||!Pending()) return false;
    std::lock_guard lock(mutex_);
    const auto count=count_.load(std::memory_order_relaxed);
    if(!count) return false;
    *event=events_[head_];head_=(head_+1)%Capacity;
    count_.store(count-1,std::memory_order_release);return true;
  }
  void Reset() {std::lock_guard lock(mutex_);head_=0;count_.store(0,std::memory_order_release);}
private:
  std::array<PeEvent,Capacity> events_{};
  std::mutex mutex_;
  std::atomic<std::size_t> count_{0};
  std::size_t head_=0;
};
}
