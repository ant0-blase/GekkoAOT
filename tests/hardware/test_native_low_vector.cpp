// SPDX-License-Identifier: GPL-3.0-or-later
#include "native/low_vector_native.h"
#include "native/runtime.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <random>
#include <string>

#define CHECK(x) do { if (!(x)) { std::cerr << __LINE__ << ": " #x "\n"; std::exit(1); } } while (false)

namespace GekkoAOT::Native {
struct HardwareTestAccess {
  static bool Reference(HostRuntime& runtime, std::uint64_t& cycles) {
    cycles = 0u;
    for (unsigned step = 0; step < 34u; ++step) {
      std::uint64_t charged = 0u;
      if (!runtime.ExecuteLowMemoryInstruction(runtime.Cpu().pc, &charged)) return false;
      cycles += charged;
    }
    return true;
  }
  static std::uint32_t& Sprg0(HostRuntime& runtime) { return runtime.spr_state_[272u]; }
  static void Publish(HostRuntime& runtime) { runtime.InvalidateLowInstructionCache(); }
  static bool Native(HostRuntime& runtime, std::uint64_t& cycles) {
    return runtime.ExecuteNativeLowMemoryVector(runtime.Cpu().pc, &cycles);
  }
  static bool Step(HostRuntime& runtime, std::uint64_t& cycles) {
    return runtime.ExecuteLowMemoryInstruction(runtime.Cpu().pc, &cycles);
  }
  static bool Enabled(const HostRuntime& runtime) { return runtime.native_low_vector_enabled_; }
  static bool Cached(const HostRuntime& runtime) { return runtime.native_low_vector_cache_.Find(0x500u); }
  static bool LineValid(const HostRuntime& runtime, std::uint32_t physical) {
    return runtime.low_instruction_cache_valid_[physical / 32u];
  }
  static bool Fetch(HostRuntime& runtime, std::uint32_t physical, std::uint32_t& word) {
    return runtime.FetchLowMemoryInstruction(physical, &word);
  }
  static void TrackEffects(HostRuntime& runtime, bool enabled) { runtime.track_host_effects_ = enabled; }
  static std::uint64_t Effects(const HostRuntime& runtime) { return runtime.host_effects_; }
  static bool HasFault(const HostRuntime& runtime) {
    return runtime.fault_.kind != HostRuntime::FaultKind::None;
  }
};
}

using namespace GekkoAOT::Native;
using Access = HardwareTestAccess;

namespace {
constexpr std::array<std::uint32_t, NativeLowVector::WordCount> Code{
    0x7c9043a6u, 0x808000c0u, 0x9064000cu, 0x7c7042a6u,
    0x90640010u, 0x90a40014u, 0xa06401a2u, 0x60630002u,
    0xb06401a2u, 0x7c600026u, 0x90640080u, 0x7c6802a6u,
    0x90640084u, 0x7c6902a6u, 0x90640088u, 0x7c6102a6u,
    0x9064008cu, 0x7c7a02a6u, 0x90640198u, 0x7c7b02a6u,
    0x9064019cu, 0x7c651b78u, 0x60000000u, 0x7c6000a6u,
    0x60630030u, 0x7c7b03a6u, 0x38600004u, 0x808000d4u,
    0x54a507bdu, 0x40820014u, 0x3ca0803eu, 0x38a544d4u,
    0x7cba03a6u, 0x4c000064u, 0x546515bau, 0x80a53000u,
    0x7cba03a6u, 0x4c000064u};
constexpr std::uint32_t Context = 0x80010000u;

void VerifyProof() {
  const auto valid = NativeLowVector::Decode(Code);
  CHECK(valid);
  for (std::size_t i = 0; i < Code.size(); ++i) {
    for (unsigned bit = 0; bit < 32u; ++bit) {
      auto mutation = Code;
      mutation[i] ^= (1u << bit);
      const bool parameter = (i == 26u || i == 30u || i == 31u) && bit < 16u;
      CHECK(bool(NativeLowVector::Decode(mutation)) == parameter);
    }
  }
  CHECK(!NativeLowVector::Decode(std::span(Code).first(Code.size() - 1u)));
  NativeLowVectorCache cache;
  CHECK(!cache.Find(0x80000500u));
  cache.Store(0x80000500u, *valid);
  cache.Store(0x80000900u, *valid);
  CHECK(cache.Find(0xc0000500u) && cache.Find(0x500u));
  CHECK(!cache.Find(0x504u) && !cache.Find(0x800u));
  cache.InvalidateLine(0xc000059fu);
  CHECK(!cache.Find(0x500u) && cache.Find(0x900u));
  cache.Reset();
  CHECK(!cache.Find(0x900u));
}

void VerifyParity(HostRuntime& runtime) {
  auto& memory = runtime.Memory();
  auto& cpu = runtime.Cpu();
  const auto context_base = memory.Mem2().empty() ? Context : 0x90010000u;
  std::mt19937 random(0x294501u);
  constexpr std::uint32_t Pages[] = {
      0x100u,0x200u,0x300u,0x400u,0x500u,0x600u,0x700u,
      0x800u,0x900u,0xc00u,0xd00u,0xf00u,0x1300u,0x1700u};
  std::uint64_t checks = 0;
  for (const auto page : Pages) {
    for (const auto alias : {0u, 0x80000000u, 0xc0000000u}) {
      for (unsigned round = 0; round < 256u; ++round) {
        auto code = Code;
        code[26] = 0x38600000u | (random() & 0xffffu);
        code[30] = 0x3ca08020u | (random() & 0xfu);
        code[31] = 0x38a50000u | (random() & 0xffffu);
        const auto leaf = NativeLowVector::Decode(code);
        CHECK(leaf);
        for (std::size_t i = 0; i < code.size(); ++i)
          CHECK(memory.Write32(page + std::uint32_t(i) * 4u, code[i]));
        Access::Publish(runtime);
        // MEM1 aliases and intentionally unaligned context payloads retain
        // the interpreter's byte-addressed RAM semantics.
        const auto context = context_base + (round & 3u);
        CHECK(memory.Write32(0xc0u, (context & 0x3fffffffu) | alias));
        CHECK(memory.Write32(0xd4u, random()));
        const auto number = std::uint32_t(std::int32_t(std::int16_t(code[26])));
        CHECK(memory.Write32(0x3000u + ((number << 2u) & 0x3fcu), 0x80500000u | (random() & 0xffffu)));
        auto* payload = memory.Resolve(context, 0x300u);
        CHECK(payload);
        std::array<std::uint8_t, 0x300u> before{};
        for (auto& byte : before) byte = std::uint8_t(random());
        std::copy(before.begin(), before.end(), payload);
        for (auto& value : cpu.gpr) value = random();
        for (auto& value : cpu.gqr) value = random();
        cpu.pc = page | alias;
        cpu.msr = random() & ~0xc000u;
        cpu.srr0 = random(); cpu.srr1 = (random() & ~2u) | (round & 1u ? 2u : 0u);
        cpu.cr = random(); cpu.lr = random(); cpu.ctr = random(); cpu.xer = random();
        cpu.exception = random(); cpu.program_exception = random();
        Access::Sprg0(runtime) = random();
        const auto original_sprg0 = Access::Sprg0(runtime);
        const CPUState original = cpu;
        std::uint64_t reference_cycles = 0u;
        CHECK(Access::Reference(runtime, reference_cycles));
        const CPUState reference = cpu;
        const auto reference_sprg0 = Access::Sprg0(runtime);
        std::array<std::uint8_t, 0x300u> expected{};
        std::copy_n(payload, expected.size(), expected.begin());

        cpu = original;
        Access::Sprg0(runtime) = original_sprg0;
        std::copy(before.begin(), before.end(), payload);
        std::uint64_t native_cycles = 0u;
        CHECK(leaf->Execute(cpu, memory, original.pc, Access::Sprg0(runtime), native_cycles));
        CHECK(native_cycles == reference_cycles);
        if (std::memcmp(&cpu, &reference, sizeof(cpu)) != 0) {
          std::cerr << "CPU mismatch page=" << std::hex << page << " round=" << round << "\n";
          CHECK(false);
        }
        CHECK(Access::Sprg0(runtime) == reference_sprg0);
        CHECK(std::equal(expected.begin(), expected.end(), payload));
        ++checks;
      }
    }
  }
  std::cout << "Native low-vector production-interpreter parity: " << checks
            << " complete CPU/context/cycle states region="
            << (memory.Mem2().empty() ? "MEM1" : "MEM2") << "\n";
}

void VerifyRejections() {
  AddressSpace memory(0x20000u, 0u);
  CPUState cpu{};
  cpu.pc = 0x500u;
  CHECK(memory.Write32(0xc0u, Context));
  CHECK(memory.Write32(0xd4u, Context));
  const auto leaf = NativeLowVector::Decode(Code);
  CHECK(leaf);
  const auto reject = [&] {
    const CPUState before = cpu;
    const auto bytes = std::vector<std::uint8_t>(memory.Mem1().begin(), memory.Mem1().end());
    std::uint32_t sprg0 = 0x12345678u;
    std::uint64_t cycles = 0x987654321ull;
    CHECK(!leaf->Execute(cpu, memory, cpu.pc, sprg0, cycles));
    CHECK(std::memcmp(&before, &cpu, sizeof(cpu)) == 0);
    CHECK(sprg0 == 0x12345678u && cycles == 0x987654321ull);
    CHECK(std::equal(bytes.begin(), bytes.end(), memory.Mem1().begin()));
  };
  for (const auto mode : {0x8000u, 0x4000u, 0xc000u}) { cpu.msr = mode; reject(); }
  cpu.msr = 0u;
  for (const auto pc : {0u, 0x504u, 0x1000u, 0x1800u, 0x80020000u}) { cpu.pc = pc; reject(); }
  cpu.pc = 0x500u;
  for (const auto context : {0u, 0xc0u, 0x3000u, 0x3fffu, 0x1ff00u, 0xcc000000u, 0x7e000000u, 0x10000000u, 0xfffffffcu}) {
    CHECK(memory.Write32(0xc0u, context));
    reject();
  }
  // Unmapped globals or an unmapped RI handler table must not begin saving
  // a perfectly valid MEM2 context before returning to the old executor.
  for (const auto mem1_bytes : {0xc3u, 0xd7u, 0x200u}) {
    AddressSpace truncated(mem1_bytes, 0x20000u);
    CPUState state{};
    state.pc = 0x500u;
    state.srr1 = 2u;
    (void)truncated.Write32(0xc0u, 0x10010000u);
    (void)truncated.Write32(0xd4u, 0x90010000u);
    const auto mem1_before = std::vector<std::uint8_t>(truncated.Mem1().begin(), truncated.Mem1().end());
    const auto mem2_before = std::vector<std::uint8_t>(truncated.Mem2().begin(), truncated.Mem2().end());
    const auto state_before = state;
    std::uint32_t sprg0 = 123u;
    std::uint64_t cycles = 456u;
    CHECK(!leaf->Execute(state, truncated, state.pc, sprg0, cycles));
    CHECK(std::memcmp(&state, &state_before, sizeof(state)) == 0);
    CHECK(sprg0 == 123u && cycles == 456u);
    CHECK(std::equal(mem1_before.begin(), mem1_before.end(), truncated.Mem1().begin()));
    CHECK(std::equal(mem2_before.begin(), mem2_before.end(), truncated.Mem2().begin()));
  }
}

void VectorEnvironment(const char* value) {
#if defined(_WIN32)
  CHECK(_putenv_s("GEKKOAOT_NATIVE_EXCEPTION_VECTOR", value ? value : "") == 0);
#else
  CHECK(value ? setenv("GEKKOAOT_NATIVE_EXCEPTION_VECTOR", value, 1) == 0
              : unsetenv("GEKKOAOT_NATIVE_EXCEPTION_VECTOR") == 0);
#endif
}

void VerifyAdapter(HostRuntime& runtime) {
  const char* saved_value = std::getenv("GEKKOAOT_NATIVE_EXCEPTION_VECTOR");
  const std::optional<std::string> saved = saved_value ? std::optional<std::string>(saved_value) : std::nullopt;
  auto& memory = runtime.Memory();
  auto& cpu = runtime.Cpu();
  const auto install = [&] {
    for (std::size_t i = 0u; i < Code.size(); ++i)
      CHECK(memory.Write32(0x500u + std::uint32_t(i) * 4u, Code[i]));
    CHECK(memory.Write32(0xc0u, Context));
    CHECK(memory.Write32(0xd4u, Context));
    CHECK(memory.Write32(0x3010u, 0x80501234u));
  };
  const auto prepare = [&] {
    cpu.pc = 0x500u; cpu.msr = 0u; cpu.srr1 = 0u;
    cpu.srr0 = 0x81234567u; cpu.exception = 0x55u;
    cpu.cr = 0x87654321u; cpu.xer = 0x80000000u;
    cpu.gpr[3] = 0x12345678u; cpu.gpr[4] = 0x87654321u; cpu.gpr[5] = 0x11223344u;
  };
  const auto reject = [&] {
    const auto before = cpu;
    const auto old_sprg0 = Access::Sprg0(runtime);
    const auto effects = Access::Effects(runtime);
    std::uint64_t cycles = 123456u;
    CHECK(!Access::Native(runtime, cycles));
    CHECK(cycles == 123456u && Access::Sprg0(runtime) == old_sprg0);
    CHECK(std::memcmp(&cpu, &before, sizeof(cpu)) == 0);
    CHECK(Access::Effects(runtime) == effects);
  };
  std::uint64_t cycles = 0u;

  VectorEnvironment(nullptr);
  runtime.Reset();
  CHECK(!Access::Enabled(runtime)); // Default stays off until gameplay A/B.
  install(); prepare(); reject();
  VectorEnvironment("0");
  runtime.Reset();
  CHECK(!Access::Enabled(runtime));
  install(); prepare(); reject();
  CHECK(Access::Reference(runtime, cycles) && cycles == 35u && cpu.pc == 0x803e44d4u);

  VectorEnvironment("1");
  runtime.Reset();
  CHECK(Access::Enabled(runtime) && !Access::Cached(runtime));
  install(); prepare();
  Access::TrackEffects(runtime, true);
  const auto effects = Access::Effects(runtime);
  CHECK(Access::Native(runtime, cycles));
  CHECK(cycles == 35u && cpu.pc == 0x803e44d4u && Access::Cached(runtime));
  CHECK(Access::Effects(runtime) - effects == 43u);
  for (std::uint32_t line = 0x500u; line < 0x5a0u; line += 32u)
    CHECK(Access::LineValid(runtime, line));

  // A RAM rewrite without icbi remains unpublished while ICE is enabled.
  CHECK(memory.Write32(0x500u, 0x60000000u));
  prepare();
  CHECK(Access::Native(runtime, cycles) && cpu.pc == 0x803e44d4u);
  std::uint32_t visible = 0u;
  CHECK(Access::Fetch(runtime, 0x500u, visible) && visible == Code[0]);

  const auto hid0 = cpu.spr_read(&cpu, 1008u, 0x80010000u);
  cpu.spr_write(&cpu, 1008u, hid0 & ~(1u << 15u), 0x80010000u);
  prepare(); reject();
  CHECK(Access::Fetch(runtime, 0x500u, visible) && visible == 0x60000000u);
  // ICE off/on without ICFI preserves the original cached line contents.
  cpu.spr_write(&cpu, 1008u, hid0, 0x80010000u);
  prepare();
  CHECK(Access::Native(runtime, cycles) && cpu.pc == 0x803e44d4u);
  CHECK(Access::Fetch(runtime, 0x500u, visible) && visible == Code[0]);

  cpu.cache_control(&cpu, 3u, 0xc0000500u, 0x80010000u);
  CHECK(!Access::Cached(runtime) && !Access::LineValid(runtime, 0x500u));
  prepare(); reject();
  // The failed proof must not fetch untouched lines or be negatively cached.
  CHECK(!Access::LineValid(runtime, 0x500u));
  CHECK(memory.Write32(0x500u, Code[0]));
  prepare();
  CHECK(Access::Native(runtime, cycles) && Access::Cached(runtime));

  CHECK(memory.Write32(0x57cu, 0x38a599aau));
  prepare();
  CHECK(Access::Native(runtime, cycles) && cpu.pc == 0x803e44d4u);
  cpu.cache_control(&cpu, 3u, 0x8000057cu, 0x80010000u);
  CHECK(!Access::Cached(runtime));
  prepare();
  CHECK(Access::Native(runtime, cycles) && cpu.pc == 0x803d99a8u && Access::Cached(runtime));

  CHECK(memory.Write32(0x500u, 0x60000000u));
  cpu.spr_write(&cpu, 1008u, hid0 | (1u << 11u), 0x80010000u);
  CHECK(!Access::Cached(runtime));
  for (std::uint32_t line = 0x500u; line < 0x5a0u; line += 32u)
    CHECK(!Access::LineValid(runtime, line));
  prepare(); reject();
  for (std::uint32_t line = 0x500u; line < 0x5a0u; line += 32u)
    CHECK(!Access::LineValid(runtime, line));

  install();
  CHECK(memory.Write32(0xc0u, 0x04000000u));
  prepare(); reject();
  CHECK(!Access::HasFault(runtime) && !Access::Cached(runtime));
  // Once native admission refuses, the unchanged old executor retains its
  // precise sequential SPRG0/load/store effects and instruction fault site.
  CHECK(Access::Step(runtime, cycles) && cycles == 1u);
  CHECK(Access::Sprg0(runtime) == 0x87654321u && cpu.pc == 0x504u);
  CHECK(Access::Step(runtime, cycles) && cpu.gpr[4] == 0x04000000u && cpu.pc == 0x508u);
  CHECK(Access::Step(runtime, cycles) && Access::HasFault(runtime) && cpu.pc == 0x508u);

  VectorEnvironment("0");
  runtime.Reset();
  CHECK(!Access::Enabled(runtime) && !Access::Cached(runtime));
  install(); prepare(); reject();
  VectorEnvironment(saved ? saved->c_str() : nullptr);
  runtime.Reset();
  std::cout << "Native low-vector adapter policy/ICE/ICBI/ICFI/fault controls passed\n";
}

bool BenchmarkReference(HostRuntime& runtime, std::uint64_t& cycles) {
  return Access::Reference(runtime, cycles);
}
bool BenchmarkNative(const NativeLowVector& leaf, HostRuntime& runtime,
                     std::uint64_t& cycles) {
  return leaf.Execute(runtime.Cpu(), runtime.Memory(), 0x500u,
                      Access::Sprg0(runtime), cycles);
}

void Benchmark(HostRuntime& runtime) {
  auto& cpu = runtime.Cpu();
  auto& memory = runtime.Memory();
  constexpr std::uint32_t entry = 0x500u;
  for (std::size_t i = 0; i < Code.size(); ++i)
    CHECK(memory.Write32(entry + std::uint32_t(i) * 4u, Code[i]));
  Access::Publish(runtime);
  CHECK(memory.Write32(0xc0u, Context));
  CHECK(memory.Write32(0xd4u, Context));
  CHECK(memory.Write32(0x3010u, 0x80501234u));
  cpu.pc = entry; cpu.msr = 0u; cpu.srr1 = 2u;
  const auto leaf = NativeLowVector::Decode(Code);
  CHECK(leaf);
  std::uint64_t cycles = 0u;
  CHECK(Access::Reference(runtime, cycles)); // Warm the authoritative I-cache.
  // Opaque calls prevent specializing the leaf for these fixed benchmark
  // pointers/constants or deleting the repeated context/CPU stores.
  auto* volatile reference_call = &BenchmarkReference;
  auto* volatile native_call = &BenchmarkNative;
  constexpr unsigned iterations = 2000000u;
  const auto start = std::chrono::steady_clock::now();
  for (unsigned i = 0; i < iterations; ++i) {
    cpu.pc = entry; cpu.msr = 0u; cpu.srr1 = (i & 1u) ? 2u : 0u;
    cpu.gpr[3] = i; cpu.cr = i * 123456789u;
    CHECK(reference_call(runtime, cycles));
    std::atomic_signal_fence(std::memory_order_seq_cst);
  }
  const auto middle = std::chrono::steady_clock::now();
  for (unsigned i = 0; i < iterations; ++i) {
    cpu.pc = entry; cpu.msr = 0u; cpu.srr1 = (i & 1u) ? 2u : 0u;
    cpu.gpr[3] = i; cpu.cr = i * 123456789u;
    CHECK(native_call(*leaf, runtime, cycles));
    std::atomic_signal_fence(std::memory_order_seq_cst);
  }
  const auto end = std::chrono::steady_clock::now();
  const auto old_ns = std::chrono::duration<double, std::nano>(middle - start).count() / iterations;
  const auto native_ns = std::chrono::duration<double, std::nano>(end - middle).count() / iterations;
  std::cout << "Native low-vector benchmark ns/vector interpreter=" << old_ns << " native=" << native_ns << " ratio=" << old_ns / native_ns << "\n";
}
}

int main(int argc, char**) {
  VerifyProof();
  VerifyRejections();
  auto runtime = std::make_unique<HostRuntime>();
  VerifyParity(*runtime);
  VerifyAdapter(*runtime);
  if (argc > 1) Benchmark(*runtime);
  auto mem2_runtime = std::make_unique<HostRuntime>(true);
  VerifyParity(*mem2_runtime);
  std::cout << "Native low-vector v294 checks passed\n";
}
