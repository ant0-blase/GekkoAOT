#pragma once
// SPDX-License-Identifier: GPL-3.0-or-later
#include "native/disc_io_worker.h"
#include <vector>
namespace GekkoAOT::Native {
// Host SDK request queue. Contains file ranges and callbacks, never DI
// commands, register state, DMA packets, optical delays or interrupt causes.
class DvdQueue final {
public:
  struct Request {
    std::uint32_t block = 0, destination = 0, bytes = 0, callback = 0,
                  cancel_callback = 0;
    std::uint64_t offset = 0, sequence = 0;
    unsigned priority = 2;
    bool cancelled = false;
  };
  struct Completion {
    Request request;
    int result = 0;
  };
  bool Submit(Request request);
  bool Cancel(std::uint32_t block, std::uint32_t callback);
  bool Advance(DiscIoWorker &, Completion &, std::vector<std::uint8_t> &);
  // Start the next queued read on an idle worker right away (no
  // completion handling), so I/O overlaps the guest instead of waiting for
  // the next host boundary. Returns true when a read was started.
  bool StartNext(DiscIoWorker &);
  void Reset();
  bool Empty() const;
  bool OwnsWorker() const { return active_ >= 0; }
  int Status(std::uint32_t block) const;
  std::uint32_t ActiveBlock() const {
    return active_ >= 0 ? slots_[active_].request.block : 0;
  }

private:
  enum class State { Free, Queued, Reading };
  struct Slot {
    State state = State::Free;
    Request request;
  };
  // The SDK queue is a list of caller-owned command blocks with no fixed
  // size (a level load queues dozens of reads at once: Star Fox Adventures
  // queues 70); MaxRequests only bounds a runaway guest.
  static constexpr std::size_t MaxRequests = 4096;
  std::vector<Slot> slots_;
  int active_ = -1;
  std::uint64_t sequence_ = 0;
};
} // namespace GekkoAOT::Native
