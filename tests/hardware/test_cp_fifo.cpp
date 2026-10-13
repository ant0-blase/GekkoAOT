// SPDX-License-Identifier: GPL-3.0-or-later
#include "hw/cp/native_cp.h"
#include <cstdlib>
#include <iostream>
#include <vector>
#define CHECK(x) do { if (!(x)) { std::cerr << __LINE__ << ": " #x "\n"; std::exit(1); } } while (false)
using GekkoAOT::HW::CP::NativeCP;

struct BatchTrace {
  unsigned single = 0;
  unsigned batches = 0;
  unsigned bursts = 0;
};
static bool OnSingleBurst(void* user, std::uint32_t) {
  ++static_cast<BatchTrace*>(user)->single;
  return true;
}
static bool OnBatchBurst(void* user, std::uint32_t, std::uint32_t count) {
  auto& trace = *static_cast<BatchTrace*>(user);
  ++trace.batches;
  trace.bursts += count;
  return true;
}

struct AddressTrace {
  std::vector<std::uint32_t> accepted;
  std::uint32_t fail_at = 0xffffffffu;
};
static bool TraceSingle(void* user, std::uint32_t address) {
  auto& trace = *static_cast<AddressTrace*>(user);
  if (address == trace.fail_at) return false;
  trace.accepted.push_back(address);
  return true;
}
static bool TraceBatch(void* user, std::uint32_t address, std::uint32_t count) {
  auto& trace = *static_cast<AddressTrace*>(user);
  for (std::uint32_t i = 0; i < count; ++i)
    if (address + i * 32u == trace.fail_at) return false;
  for (std::uint32_t i = 0; i < count; ++i)
    trace.accepted.push_back(address + i * 32u);
  return true;
}

static void CompareBatchAgainstSingleBursts() {
  std::uint32_t seed = 0x241abcdeu;
  const auto random = [&]() { seed = seed * 1664525u + 1013904223u; return seed; };
  // Exercise normal and full rings, batches larger than the 128-burst cap,
  // wrap, arbitrary breakpoints, failed callbacks and 64 MiB aperture edges.
  for (unsigned trial = 0; trial < 8192; ++trial) {
    const auto span = (random() % 300u) + 1u;
    const auto base = (trial & 1u) ? 0x03ffffe0u - (span - 1u) * 32u : 0x0035ff80u;
    const auto start = random() % span;
    const auto queued = random() % (span + 1u);
    const auto breakpoint = random() % span;
    const bool armed = trial % 3u == 0u;
    const auto fail_at = trial % 5u == 0u ? base + (random() % span) * 32u : 0xffffffffu;
    NativeCP single, batched;
    AddressTrace a, b;
    a.fail_at = b.fail_at = fail_at;
    single.SetReadBurstCallback(&TraceSingle, &a);
    batched.SetReadBurstCallback(&TraceSingle, &b);
    batched.SetReadBurstBatchCallback(&TraceBatch, &b);
    for (auto* device : {&single, &batched}) {
      const auto pair = [&](std::uint32_t offset, std::uint32_t value) {
        CHECK(device->Write(0xcc000000u + offset, value, 2));
        CHECK(device->Write(0xcc000002u + offset, value >> 16u, 2));
      };
      pair(0x20u, base);
      pair(0x24u, base + (span - 1u) * 32u);
      pair(0x38u, base + start * 32u);
      pair(0x34u, base + ((start + queued) % span) * 32u);
      pair(0x30u, queued * 32u);
      pair(0x28u, span / 2u * 32u);
      pair(0x2cu, 32u);
      pair(0x3cu, base + breakpoint * 32u);
      CHECK(device->Write(0xcc000002u, armed ? 0x3fu : 0x1du, 2));
    }
    CHECK(a.accepted == b.accepted);
    CHECK(single.FifoReadPointer() == batched.FifoReadPointer());
    CHECK(single.FifoWritePointer() == batched.FifoWritePointer());
    CHECK(single.FifoReadWriteDistance() == batched.FifoReadWriteDistance());
    CHECK(single.Status() == batched.Status());
    CHECK(single.InterruptPending() == batched.InterruptPending());
  }
}

int main() {
  NativeCP cp;
  const auto write = [&](unsigned offset, unsigned value) { CHECK(cp.Write(0xcc000000u + offset, value, 2)); };
  const auto pair = [&](unsigned offset, unsigned value) { write(offset, value); write(offset + 2, value >> 16); };
  // SDK top points to the last word; CP drops its low five bits.
  pair(0x20, 0x1000);
  pair(0x24, 0x107c);
  pair(0x34, 0x1000);
  pair(0x38, 0x1000);
  pair(0x28, 0x60);
  pair(0x2c, 0x20);
  write(2, 0x10); // linked CPU producer, GP stopped
  CHECK(!cp.NotifyGatherWrite(31));
  CHECK(cp.FifoReadWriteDistance() == 0);
  CHECK(cp.NotifyGatherWrite(1));
  CHECK(cp.FifoReadWriteDistance() == 32);
  CHECK(cp.NotifyGatherWrite(96));
  CHECK(cp.FifoWritePointer() == 0x1000);
  CHECK(cp.FifoReadWriteDistance() == 128); // Full FIFO must retain all four bursts.
  pair(0x3c, 0x1040);
  write(2, 0x33); // read + link + breakpoint + breakpoint IRQ
  CHECK(cp.FifoReadPointer() == 0x1040);
  CHECK(cp.FifoReadWriteDistance() == 64);
  CHECK(cp.InterruptPending());
  CHECK((cp.Status() & 0x10) != 0);

  // Runtime-only deadlock recovery must not cross a breakpoint until the guest
  // handler has acknowledged the breakpoint interrupt (BPInt cleared).
  CHECK(!cp.StepPastHandledBreakpointOnce());
  write(2, 0x13); // keep BP armed, clear BPInt; GP remains stopped
  CHECK(cp.BreakpointActiveForHost());
  CHECK(!cp.InterruptPending());
  CHECK(cp.StepPastHandledBreakpointOnce());
  CHECK(cp.FifoReadPointer() == 0x1060);
  CHECK(cp.FifoReadWriteDistance() == 32);

  write(2, 0x11); // release breakpoint and drain through wrap
  CHECK(cp.FifoReadPointer() == 0x1000);
  CHECK(cp.FifoReadWriteDistance() == 0);
  CHECK(!cp.InterruptPending());
  CHECK(cp.NotifyGatherWrite(128));
  CHECK(cp.FifoReadPointer() == 0x1000 && cp.FifoWritePointer() == 0x1000);
  CHECK(cp.FifoReadWriteDistance() == 0);
  // Moving a breakpoint resumes the already queued FIFO. No further CPU
  // gather write or GP-enable toggle is required to make the device progress.
  pair(0x3c, 0x1020);
  write(2, 0x33);
  CHECK(cp.NotifyGatherWrite(128));
  CHECK(cp.FifoReadPointer() == 0x1020 && cp.FifoReadWriteDistance() == 96);
  CHECK(cp.InterruptPending());
  pair(0x3c, 0x1060);
  CHECK(cp.FifoReadPointer() == 0x1060 && cp.FifoReadWriteDistance() == 32);
  CHECK(cp.InterruptPending()); // new stop, not a skipped completion fence
  write(2, 0x13); // acknowledge the interrupt while retaining the stop
  pair(0x3c, 0x1040); // move the stop behind the current reader
  CHECK(cp.FifoReadPointer() == 0x1000 && cp.FifoReadWriteDistance() == 0);
  CHECK(!cp.InterruptPending());

  // v161: a backlog accumulated while GP reads are disabled must be drained
  // through the batch callback once reads are enabled, without changing FIFO
  // accounting or crossing the ring wrap.
  NativeCP batched;
  BatchTrace trace{};
  batched.SetReadBurstCallback(&OnSingleBurst, &trace);
  batched.SetReadBurstBatchCallback(&OnBatchBurst, &trace);
  const auto bwrite = [&](unsigned offset, unsigned value) {
    CHECK(batched.Write(0xcc000000u + offset, value, 2));
  };
  const auto bpair = [&](unsigned offset, unsigned value) {
    bwrite(offset, value);
    bwrite(offset + 2, value >> 16);
  };
  bpair(0x20, 0x2000);
  bpair(0x24, 0x23fc);
  bpair(0x34, 0x2000);
  bpair(0x38, 0x2000);
  bwrite(2, 0x10); // link on, GP read off
  CHECK(batched.NotifyGatherWrite(256));
  CHECK(batched.FifoReadWriteDistance() == 256);
  bwrite(2, 0x11); // enable GP read -> batched drain
  CHECK(trace.batches >= 1);
  CHECK(trace.bursts >= 2);
  CHECK(batched.FifoReadWriteDistance() == 0);

  // v162 regression: an enabled breakpoint inside a batched backlog must cap
  // the batch at the breakpoint boundary.  The old v161 path saw that the
  // breakpoint was not active at 0x3000, submitted all eight bursts at once,
  // advanced to 0x3100 and silently skipped the stop at 0x3080.
  NativeCP bp_batched;
  BatchTrace bp_trace{};
  bp_batched.SetReadBurstCallback(&OnSingleBurst, &bp_trace);
  bp_batched.SetReadBurstBatchCallback(&OnBatchBurst, &bp_trace);
  const auto bpwrite = [&](unsigned offset, unsigned value) {
    CHECK(bp_batched.Write(0xcc000000u + offset, value, 2));
  };
  const auto bppair = [&](unsigned offset, unsigned value) {
    bpwrite(offset, value);
    bpwrite(offset + 2, value >> 16);
  };
  bppair(0x20, 0x3000);
  bppair(0x24, 0x33fc);
  bppair(0x34, 0x3000);
  bppair(0x38, 0x3000);
  bpwrite(2, 0x10); // accumulate while GP reads are stopped
  CHECK(bp_batched.NotifyGatherWrite(256));
  CHECK(bp_batched.FifoReadWriteDistance() == 256);
  bppair(0x3c, 0x3080);
  bpwrite(2, 0x33); // GP read + link + breakpoint + breakpoint IRQ
  CHECK(bp_batched.FifoReadPointer() == 0x3080);
  CHECK(bp_batched.FifoReadWriteDistance() == 128);
  CHECK(bp_batched.BreakpointActiveForHost());
  CHECK(bp_batched.InterruptPending());
  CHECK((bp_batched.Status() & 0x10) != 0);
  CHECK(bp_trace.bursts == 4); // 0x3000..0x3060, never across 0x3080

  // Driven (GDVE6L) regression: CPBreakpoint is a pair of 16-bit registers.
  // When a live breakpoint crosses a 64 KiB boundary, draining on the first
  // half-write observes a bogus intermediate address.  With low-first SDK
  // writes, 0x0035ff80 -> 0x00360040 briefly looked like 0x00350040 and the
  // old code consumed the complete backlog instead of stopping at 0x00360040.
  NativeCP bp_atomic;
  BatchTrace atomic_trace{};
  bp_atomic.SetReadBurstCallback(&OnSingleBurst, &atomic_trace);
  bp_atomic.SetReadBurstBatchCallback(&OnBatchBurst, &atomic_trace);
  const auto awrite = [&](unsigned offset, unsigned value) {
    CHECK(bp_atomic.Write(0xcc000000u + offset, value, 2));
  };
  const auto apair = [&](unsigned offset, unsigned value) {
    awrite(offset, value);
    awrite(offset + 2, value >> 16);
  };
  apair(0x20, 0x00350000);
  apair(0x24, 0x0036fffc);
  apair(0x34, 0x0035ff80);
  apair(0x38, 0x0035ff80);
  awrite(2, 0x10); // link on, GP read off while the backlog is produced
  CHECK(bp_atomic.NotifyGatherWrite(256));
  CHECK(bp_atomic.FifoWritePointer() == 0x00360080);
  CHECK(bp_atomic.FifoReadWriteDistance() == 256);
  apair(0x3c, 0x0035ff80);
  awrite(2, 0x33); // stop exactly at the old breakpoint
  CHECK(bp_atomic.FifoReadPointer() == 0x0035ff80);
  CHECK(bp_atomic.FifoReadWriteDistance() == 256);
  awrite(2, 0x13); // acknowledge BP IRQ but retain GP read + breakpoint

  // First half alone must never run the GPU against 0x00350040.
  awrite(0x3c, 0x0040);
  CHECK(bp_atomic.FifoBreakpoint() == 0x00350040);
  CHECK(bp_atomic.FifoReadPointer() == 0x0035ff80);
  CHECK(bp_atomic.FifoReadWriteDistance() == 256);

  // Completing the pair commits 0x00360040 and resumes exactly to that stop.
  awrite(0x3e, 0x0036);
  CHECK(bp_atomic.FifoBreakpoint() == 0x00360040);
  CHECK(bp_atomic.FifoReadPointer() == 0x00360040);
  CHECK(bp_atomic.FifoReadWriteDistance() == 64);

  // Disney Hide & Sneak releases a stopped BP before stopping GP and
  // programming the next BP. MMIO must not drain the entire backlog between
  // those register writes, or several frame tokens pass the VI poll at once.
  bp_atomic.SetDeferredRegisterDrain(true);
  awrite(2, 0x11); // GXDisableBreakPt
  CHECK(bp_atomic.RegisterDrainPending());
  CHECK(bp_atomic.FifoReadPointer() == 0x00360040);
  awrite(2, 0x10); // GXEnableBreakPt first stops GP
  apair(0x3c, 0x00360060);
  awrite(2, 0x32); // enable new breakpoint while GP is stopped
  awrite(2, 0x33); // resume GP
  CHECK(bp_atomic.FifoReadPointer() == 0x00360040);
  bp_atomic.ServiceRegisterDrain();
  CHECK(bp_atomic.FifoReadPointer() == 0x00360060);
  CHECK(bp_atomic.FifoReadWriteDistance() == 32);
  CHECK(bp_atomic.InterruptPending());
  awrite(2, 0x11); // an intentional release must also resume without a new BP
  bp_atomic.ServiceRegisterDrain();
  CHECK(bp_atomic.FifoReadPointer() == 0x00360080);
  CHECK(bp_atomic.FifoReadWriteDistance() == 0);

  CompareBatchAgainstSingleBursts();
  std::cout << "CP FIFO capacity/wrap/breakpoint/batch tests passed; 8192 differential rings\n";
}
