// SPDX-License-Identifier: GPL-3.0-or-later
#include "native/runtime.h"
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <memory>
#define CHECK(x) do { if (!(x)) { std::cerr << __LINE__ << ": " #x "\n"; std::exit(1); } } while(false)
namespace GekkoAOT::Native {
struct HardwareTestAccess {
  static std::uint64_t Calls(const HostRuntime& runtime) { return runtime.hle_calls_; }
};
}
using namespace GekkoAOT::Native;
int main(int argc, char** argv) {
  auto runtime = std::make_unique<HostRuntime>();
  auto& cpu = runtime->Cpu();
  const auto first_calls = HardwareTestAccess::Calls(*runtime);
  std::uint64_t random = 0x123456789abcdef0ull;
  unsigned calls = 0;
  for (unsigned kind = 0; kind < 2; ++kind) {
    const auto token = 0xfffc0000u | static_cast<unsigned>(
        kind ? SimpleHleKind::OsGetTime : SimpleHleKind::OsGetTick);
    for (unsigned i = 0; i < 512; ++i) {
      random = random * 6364136223846793005ull + 1;
      cpu.timebase = i == 0 ? 0 : i == 1 ? UINT64_MAX : random;
      cpu.pc = 0x80010000; cpu.lr = 0x80020004;
      cpu.gpr[3] = 0xdeadbeef; cpu.gpr[4] = 0xabcdef01;
      cpu.downcount = -777; cpu.cycle_budget = 123;
      cpu.external_write_count = static_cast<unsigned char>(i);
      CPUState expected; std::memcpy(&expected, &cpu, sizeof cpu);
      expected.gpr[3] = static_cast<std::uint32_t>(kind ? cpu.timebase >> 32 : cpu.timebase);
      if (kind) expected.gpr[4] = static_cast<std::uint32_t>(cpu.timebase);
      expected.pc = expected.lr;
      if (expected.external_write_count != 255) ++expected.external_write_count;
      CHECK(cpu.host_call(&cpu, token));
      CHECK(std::memcmp(&cpu, &expected, sizeof cpu) == 0);
      ++calls;
    }
  }
  CHECK(HardwareTestAccess::Calls(*runtime) == first_calls + calls);
  // The new hot leaves must leave the ordinary libc token path operational.
  // v301: libc leaves are chain-transparent (no host-write epoch bump);
  // cache publication still ends the native superchain.
  cpu.gpr[3] = 0x80010000; cpu.gpr[4] = 0xab; cpu.gpr[5] = 8; cpu.external_write_count = 3;
  CHECK(cpu.host_call(&cpu, 0xfffc0000u | static_cast<unsigned>(SimpleHleKind::Memset)));
  CHECK(cpu.external_write_count == 3);
  std::uint32_t word = 0;
  CHECK(runtime->Memory().Read32(0x80010000, &word) && word == 0xabababab);
  CHECK(HardwareTestAccess::Calls(*runtime) == first_calls + calls + 1);
  cpu.gpr[3] = 0x80010000; cpu.gpr[4] = 32;
  CHECK(cpu.host_call(&cpu, 0xfffc0000u | static_cast<unsigned>(SimpleHleKind::DcStoreRange)));
  CHECK(cpu.external_write_count == 4);
  ++calls;
  CHECK(!cpu.host_call(nullptr, 0xfffc0000));
  // v301 strcmp fast path: in-place MEM1 compare (retail unsigned byte
  // difference), returns to LR and leaves the host-write epoch untouched.
  const auto strcmp_token = 0xfffc0000u | static_cast<unsigned>(SimpleHleKind::Strcmp);
  CHECK(runtime->Memory().Write32(0x80011000, 0x61626300) &&
        runtime->Memory().Write32(0x80011100, 0x6162fa00));
  const auto before_strcmp = HardwareTestAccess::Calls(*runtime);
  cpu.gpr[3] = 0x80011000; cpu.gpr[4] = 0x80011100; cpu.pc = 0x80010000; cpu.lr = 0x80020004;
  cpu.external_write_count = 7;
  CHECK(cpu.host_call(&cpu, strcmp_token));
  CHECK(static_cast<std::int32_t>(cpu.gpr[3]) == int('c') - 0xfa && cpu.pc == 0x80020004);
  CHECK(cpu.external_write_count == 7);
  cpu.gpr[3] = 0x80011000; cpu.gpr[4] = 0xc0011000; cpu.pc = 0x80010000;
  CHECK(cpu.host_call(&cpu, strcmp_token) && cpu.gpr[3] == 0u && cpu.pc == 0x80020004);
  CHECK(HardwareTestAccess::Calls(*runtime) == before_strcmp + 2);
  if (argc == 2 && std::strcmp(argv[1], "--bench") == 0) {
    // Saturation is part of the existing host boundary protocol, not disabled
    // instrumentation. Measure the same callback ABI used by generated AOT.
    cpu.external_write_count = 255;
    constexpr unsigned count = 4000000;
    for (auto kind : {SimpleHleKind::OsGetTick, SimpleHleKind::OsGetTime}) {
      const auto begin = std::chrono::steady_clock::now();
      for (unsigned i = 0; i < count; ++i)
        cpu.host_call(&cpu, 0xfffc0000u | static_cast<unsigned>(kind));
      const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now() - begin).count();
      std::cout << "clock callback kind=" << static_cast<unsigned>(kind)
                << " ns/call=" << double(ns) / count << '\n';
    }
    // "abcdefghijk": cached (fast path) vs uncached alias (general dispatcher).
    CHECK(runtime->Memory().Write32(0x80011000, 0x61626364) &&
          runtime->Memory().Write32(0x80011004, 0x65666768) &&
          runtime->Memory().Write32(0x80011008, 0x696a6b00));
    for (std::uint32_t rhs : {0x80011000u, 0xc0011000u}) {
      const auto begin = std::chrono::steady_clock::now();
      for (unsigned i = 0; i < count; ++i) {
        cpu.gpr[3] = 0x80011000; cpu.gpr[4] = rhs;
        cpu.host_call(&cpu, strcmp_token);
      }
      const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now() - begin).count();
      std::cout << "strcmp callback rhs=" << std::hex << rhs << std::dec
                << " ns/call=" << double(ns) / count << '\n';
    }
  }
  std::cout << "SDK clock leaves: full CPU state, time wrap, counters, host epoch and libc fallback passed\n";
}
