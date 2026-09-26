// SPDX-License-Identifier: GPL-3.0-or-later
#include "os/native_os.h"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <vector>

#define CHECK(expr) do { if (!(expr)) { std::cerr << __LINE__ << ": " #expr "\n"; std::exit(1); } } while (false)

using GekkoAOT::NativeOS::Kind;
using GekkoAOT::NativeOS::Service;

namespace {
constexpr std::uint32_t kMemBase = 0x80000000u;
constexpr std::uint32_t kMain = 0x80010000u;
constexpr std::uint32_t kLow = 0x80011000u;
constexpr std::uint32_t kHigh = 0x80012000u;
constexpr std::uint32_t kQueue = 0x80020000u;
constexpr std::uint32_t kArray = 0x80020100u;
constexpr std::uint32_t kOutMain = 0x80020200u;
constexpr std::uint32_t kOutLow = 0x80020204u;
constexpr std::uint32_t kOutHigh = 0x80020208u;
constexpr std::uint32_t kCurrentThread = 0x800000e4u;
constexpr std::uint32_t kCurrentContext = 0x800000d4u;

struct Fixture {
  std::vector<std::uint8_t> mem = std::vector<std::uint8_t>(2u * 1024u * 1024u);
  CPUState cpu{};
  Service& os = Service::Get();
  unsigned calls = 0;

  static void* Pointer(CPUState* state, std::uint32_t address, std::uint32_t size) {
    auto& self = *static_cast<Fixture*>(state->external_user_data);
    if (address < kMemBase) return nullptr;
    const std::uint64_t offset = std::uint64_t(address) - kMemBase;
    if (offset + size > self.mem.size()) return nullptr;
    return self.mem.data() + offset;
  }

  Fixture() {
    os.Reset();
    cpu.external_user_data = this;
    cpu.external_pointer = &Pointer;
    cpu.ram = mem.data();
    cpu.ram_size = static_cast<std::uint32_t>(mem.size());
    cpu.gpr[1] = 0x80070000u;
    cpu.msr = 0x9032u;
    Word(kCurrentThread, kMain);
    Word(kCurrentContext, kMain);
    Half(kMain + 0x2c8u, 2u); // Main thread is RUNNING.
    Word(kMain + 0x2ccu, 0u);
    Word(kMain + 0x2d0u, 31u);
    Word(kMain + 0x2d4u, 31u);
    Word(kMain + 0x304u, 0x80070000u);
    Word(kMain + 0x308u, 0x8006f000u);
    Word(0x8006f000u, 0xDEADBABEu);
    os.Register(Kind::ExitThread, 0x80090000u);
  }

  std::uint32_t Word(std::uint32_t address) const {
    const auto* p = mem.data() + (address - kMemBase);
    return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) |
           (std::uint32_t(p[2]) << 8) | p[3];
  }
  void Word(std::uint32_t address, std::uint32_t value) {
    auto* p = mem.data() + (address - kMemBase);
    p[0] = static_cast<std::uint8_t>(value >> 24);
    p[1] = static_cast<std::uint8_t>(value >> 16);
    p[2] = static_cast<std::uint8_t>(value >> 8);
    p[3] = static_cast<std::uint8_t>(value);
  }
  void Half(std::uint32_t address, std::uint16_t value) {
    auto* p = mem.data() + (address - kMemBase);
    p[0] = static_cast<std::uint8_t>(value >> 8);
    p[1] = static_cast<std::uint8_t>(value);
  }

  template<class... Args> void Call(Kind kind, Args... args) {
    const std::uint32_t parameters[] = {static_cast<std::uint32_t>(args)...};
    for (unsigned i = 0; i < sizeof...(args); ++i) cpu.gpr[3 + i] = parameters[i];
    cpu.lr = 0x800a0000u + (++calls * 4u);
    CHECK(os.Dispatch(kind, &cpu));
  }

  void PrepareQueueAndThreads() {
    Call(Kind::InitMessageQueue, kQueue, kArray, 1u);
    Call(Kind::CreateThread, kLow, 0x800b0000u, 0u, 0x80071000u, 0x1000u, 20u, 0u);
    CHECK(cpu.gpr[3] == 1u);
    Call(Kind::CreateThread, kHigh, 0x800b0100u, 0u, 0x80072000u, 0x1000u, 10u, 0u);
    CHECK(cpu.gpr[3] == 1u);
  }

  void ResumeWaiters() {
    Call(Kind::ResumeThread, kLow);
    CHECK(Word(kCurrentThread) == kLow);
    Call(Kind::ResumeThread, kHigh);
    CHECK(Word(kCurrentThread) == kHigh);
  }
};

void TestReceivers() {
  Fixture f;
  f.PrepareQueueAndThreads();
  f.ResumeWaiters();
  f.Call(Kind::ReceiveMessage, kQueue, kOutHigh, 1u);
  CHECK(f.Word(kCurrentThread) == kLow);
  f.Call(Kind::ReceiveMessage, kQueue, kOutLow, 1u);
  CHECK(f.Word(kCurrentThread) == kMain);
  CHECK(f.Word(kQueue + 8u) == kHigh); // ABI wait queue is priority ordered.
  CHECK(f.Word(kQueue + 12u) == kLow);
  f.Call(Kind::SendMessage, kQueue, 0x12345678u, 0u);
  CHECK(f.Word(kOutHigh) == 0x12345678u);
  CHECK(f.Word(kOutLow) == 0u);
}


void TestReadyPriorityMutationPreempts() {
  Fixture f;

  // Keep the current thread at priority 16 and place an equal-priority peer in
  // the READY set.  Resuming an equal-priority thread must not preempt.
  f.Word(kMain + 0x2d0u, 16u);
  f.Word(kMain + 0x2d4u, 16u);
  f.Call(Kind::CreateThread, kLow, 0x800b0000u, 0u, 0x80071000u, 0x1000u, 16u, 0u);
  CHECK(f.cpu.gpr[3] == 1u);
  f.Call(Kind::ResumeThread, kLow);
  CHECK(f.Word(kCurrentThread) == kMain);

  // Lowering the READY peer to priority 15 must refresh the host ready-cache
  // immediately and trigger the same preemption as retail SelectThread(0).
  f.Call(Kind::SetThreadPriority, kLow, 15u);
  CHECK(f.Word(kCurrentThread) == kLow);
}

void TestSenders() {
  Fixture f;
  f.PrepareQueueAndThreads();
  f.Call(Kind::SendMessage, kQueue, 0xaaaabbbbu, 0u); // Fill the one-slot queue.
  f.ResumeWaiters();
  f.Call(Kind::SendMessage, kQueue, 0x11112222u, 1u); // High priority blocks first.
  CHECK(f.Word(kCurrentThread) == kLow);
  f.Call(Kind::SendMessage, kQueue, 0x33334444u, 1u); // Lower priority also blocks.
  CHECK(f.Word(kCurrentThread) == kMain);
  CHECK(f.Word(kQueue) == kHigh);
  CHECK(f.Word(kQueue + 4u) == kLow);
  f.Call(Kind::ReceiveMessage, kQueue, kOutMain, 0u);
  CHECK(f.Word(kOutMain) == 0xaaaabbbbu);
  CHECK(f.Word(kArray) == 0x11112222u); // Highest priority sender fills free slot.
}
}

int main() {
  TestReceivers();
  TestReadyPriorityMutationPreempts();
  TestSenders();
  std::cout << "OS message receiver/sender priority tests passed\n";
}
