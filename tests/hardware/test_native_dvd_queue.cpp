// SPDX-License-Identifier: GPL-3.0-or-later
#include "native/dvd_queue.h"
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <thread>
#define CHECK(x)                                                               \
  do {                                                                         \
    if (!(x)) {                                                                \
      std::cerr << __LINE__ << ": " #x "\n";                                   \
      std::abort();                                                            \
    }                                                                          \
  } while (false)
using namespace GekkoAOT::Native;
struct Reader {
  std::atomic<bool> blocked{false}, entered{false};
  unsigned calls = 0;
  static bool Read(void *self, std::uint64_t offset, void *out,
                   std::uint32_t bytes) {
    auto &r = *static_cast<Reader *>(self);
    ++r.calls;
    r.entered = true;
    while (r.blocked.load())
      std::this_thread::yield();
    if (offset == 999)
      return false;
    std::fill_n(static_cast<std::uint8_t *>(out), bytes, std::uint8_t(offset));
    return true;
  }
};
int main() {
  Reader reader;
  DiscIoWorker worker;
  worker.Start(&reader, Reader::Read);
  DvdQueue queue;
  std::vector<std::uint8_t> bytes;
  auto submit = [&](unsigned block, unsigned offset, unsigned priority) {
    DvdQueue::Request r;
    r.block = block;
    r.bytes = 32;
    r.offset = offset;
    r.priority = priority;
    r.callback = block + 100;
    return queue.Submit(r);
  };
  auto collect = [&] {
    DvdQueue::Completion out;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!queue.Advance(worker, out, bytes)) {
      CHECK(std::chrono::steady_clock::now() < deadline);
      std::this_thread::yield();
    }
    return out;
  };
  CHECK(submit(1, 11, 2));
  CHECK(submit(2, 22, 0));
  CHECK(submit(3, 33, 2));
  CHECK(!submit(1, 44, 0));
  auto c = collect();
  CHECK(c.request.block == 2 && c.result == 32 && bytes.size() == 32 &&
        bytes[0] == 22);
  c = collect();
  CHECK(c.request.block == 1 && bytes[0] == 11);
  c = collect();
  CHECK(c.request.block == 3 && bytes[0] == 33);
  CHECK(queue.Empty());
  // Cancel before I/O: no reader call. Cancel while reading: no bytes exposed.
  auto calls = reader.calls;
  CHECK(submit(4, 44, 2));
  CHECK(queue.Cancel(4, 55));
  c = collect();
  CHECK(c.result == -6 && c.request.cancel_callback == 55 && bytes.empty() &&
        reader.calls == calls);
  reader.blocked = true;
  reader.entered = false;
  CHECK(submit(5, 55, 2));
  DvdQueue::Completion pending;
  CHECK(!queue.Advance(worker, pending, bytes));
  while (!reader.entered)
    std::this_thread::yield();
  CHECK(queue.Status(5) == 1);
  CHECK(queue.Cancel(5, 66));
  CHECK(!queue.Cancel(5, 77));
  reader.blocked = false;
  c = collect();
  CHECK(c.result == -6 && bytes.empty() && c.request.cancel_callback == 66);
  CHECK(submit(6, 999, 2));
  c = collect();
  CHECK(c.result == -1 && bytes.empty());
  CHECK(submit(7, 77, 2));
  c = collect();
  CHECK(c.request.callback == 107);
  CHECK(submit(7, 88, 0));
  c = collect();
  CHECK(bytes[0] == 88); // callback-style reuse/chaining
  // A level load's burst (Star Fox Adventures queues 70 reads): every
  // request is accepted, as by the SDK's unbounded list, and completes in
  // order.
  for (unsigned i = 1; i <= 70; ++i)
    CHECK(submit(i, 0, 2));
  CHECK(!submit(70, 0, 2)); // a block already queued is still refused
  for (unsigned i = 1; i <= 70; ++i) {
    c = collect();
    CHECK(c.request.block == i);
  }
  CHECK(queue.Empty());
  // StartNext begins the read at request time; Advance only collects.
  CHECK(submit(200, 55, 2));
  CHECK(queue.StartNext(worker));
  CHECK(queue.Status(200) == 1);           // reading, before any Advance
  CHECK(!queue.StartNext(worker));         // one read in flight at a time
  CHECK(submit(201, 66, 2));
  CHECK(worker.WaitUntilComplete(std::chrono::steady_clock::now() + std::chrono::seconds(2)));
  c = collect();
  CHECK(c.request.block == 200 && bytes[0] == 55);
  CHECK(submit(202, 77, 0));               // higher priority queued after 201
  CHECK(queue.StartNext(worker));
  c = collect();
  CHECK(c.request.block == 202 && bytes[0] == 77);
  c = collect();
  CHECK(c.request.block == 201 && bytes[0] == 66);
  CHECK(queue.Empty() && !worker.Busy());
  CHECK(worker.WorkerStarts() == 1);
  std::cout << "NativeDVD: FIFO/priority, stable block state, cancel "
               "queued/in-flight, errors, chaining, unbounded queue, one "
               "persistent worker PASS\n";
}
