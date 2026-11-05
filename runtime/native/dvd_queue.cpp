// SPDX-License-Identifier: GPL-3.0-or-later
#include "native/dvd_queue.h"
namespace GekkoAOT::Native {
bool DvdQueue::Submit(Request request) {
  if (!request.block || request.priority > 3)
    return false;
  for (const auto &slot : slots_)
    if (slot.state != State::Free && slot.request.block == request.block)
      return false;
  request.sequence = ++sequence_;
  for (auto &slot : slots_)
    if (slot.state == State::Free) {
      slot = {State::Queued, request};
      return true;
    }
  if (slots_.size() >= MaxRequests)
    return false;
  slots_.push_back({State::Queued, request});
  return true;
}
bool DvdQueue::Cancel(std::uint32_t block, std::uint32_t callback) {
  for (auto &slot : slots_)
    if (slot.state != State::Free && slot.request.block == block) {
      if (slot.request.cancelled)
        return false;
      slot.request.cancelled = true;
      slot.request.cancel_callback = callback;
      return true;
    }
  return false;
}
bool DvdQueue::Empty() const {
  for (const auto &slot : slots_)
    if (slot.state != State::Free)
      return false;
  return true;
}
int DvdQueue::Status(std::uint32_t block) const {
  for (const auto &slot : slots_)
    if (slot.state != State::Free && slot.request.block == block)
      return slot.state == State::Reading ? 1 : 2;
  return 0;
}
void DvdQueue::Reset() {
  slots_.clear();
  active_ = -1;
}
bool DvdQueue::Advance(DiscIoWorker &worker, Completion &completion,
                       std::vector<std::uint8_t> &bytes) {
  if (active_ >= 0) {
    DiscIoWorker::Result result;
    if (!worker.TryCollect(result, bytes))
      return false;
    auto &slot = slots_[active_];
    completion.request = slot.request;
    completion.result =
        slot.request.cancelled
            ? -6
            : (result.ok && result.sequence == slot.request.sequence
                   ? static_cast<int>(slot.request.bytes)
                   : -1);
    if (completion.result < 0)
      bytes.clear();
    slot = {};
    active_ = -1;
    return true;
  }
  int selected = -1;
  for (unsigned i = 0; i < slots_.size(); ++i) {
    const auto &slot = slots_[i];
    if (slot.state != State::Queued)
      continue;
    if (selected < 0 ||
        slot.request.priority < slots_[selected].request.priority ||
        (slot.request.priority == slots_[selected].request.priority &&
         slot.request.sequence < slots_[selected].request.sequence))
      selected = i;
  }
  if (selected < 0)
    return false;
  auto &slot = slots_[selected];
  if (slot.request.cancelled) {
    completion = {slot.request, -6};
    slot = {};
    bytes.clear();
    return true;
  }
  if (worker.Idle() && worker.Submit(slot.request.sequence, slot.request.offset,
                                     slot.request.bytes)) {
    slot.state = State::Reading;
    active_ = selected;
  }
  return false;
}
bool DvdQueue::StartNext(DiscIoWorker &worker) {
  if (active_ >= 0 || !worker.Idle())
    return false;
  int selected = -1;
  for (unsigned i = 0; i < slots_.size(); ++i) {
    const auto &slot = slots_[i];
    if (slot.state != State::Queued)
      continue;
    if (selected < 0 ||
        slot.request.priority < slots_[selected].request.priority ||
        (slot.request.priority == slots_[selected].request.priority &&
         slot.request.sequence < slots_[selected].request.sequence))
      selected = i;
  }
  // A cancelled head is completed (and its callback ordered) by Advance.
  if (selected < 0 || slots_[selected].request.cancelled)
    return false;
  auto &slot = slots_[selected];
  if (!worker.Submit(slot.request.sequence, slot.request.offset, slot.request.bytes))
    return false;
  slot.state = State::Reading;
  active_ = selected;
  return true;
}
} // namespace GekkoAOT::Native
