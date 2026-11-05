// SPDX-License-Identifier: GPL-3.0-or-later
// Microbenchmark for GEKKOAOT_NO_RESERVATIONS_V381: a guest stw in
// state-in-memory mode (EA from CPUState, MEM1 bounds check, byte-swapped
// store) with the hot-function branchless reservation update versus none.
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <vector>

struct CPU { std::uint32_t gpr[32]; std::uint32_t reserve_addr; bool reserve_valid; };

template <bool Reservation>
__attribute__((noinline)) double Stores(CPU* cpu, std::uint8_t* ram, long count) {
  const auto start = std::chrono::steady_clock::now();
  for (long i = 0; i < count; ++i) {
    const std::uint32_t ea = cpu->gpr[3] + std::uint32_t((i & 1023) * 4);
    const std::uint32_t offset = (ea & ~0x40000000u) - 0x80000000u;
    if (offset > 0x017ffffcu) __builtin_trap();
    const std::uint32_t value = __builtin_bswap32(cpu->gpr[4] + std::uint32_t(i));
    __builtin_memcpy(ram + offset, &value, 4);
    if (Reservation) {
      const bool different = ((cpu->reserve_addr ^ ea) & ~31u) != 0u;
      cpu->reserve_valid = cpu->reserve_valid && different;
    }
    asm volatile("" ::: "memory");
  }
  return std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count() / count;
}

int main() {
  std::vector<std::uint8_t> ram(24u << 20);
  CPU cpu{};
  cpu.gpr[3] = 0x80400000u; cpu.gpr[4] = 0x12345678u; cpu.reserve_addr = 0x80001000u;
  double with = 1e9, without = 1e9;
  for (int r = 0; r < 5; ++r) {
    with = std::min(with, Stores<true>(&cpu, ram.data(), 100'000'000));
    without = std::min(without, Stores<false>(&cpu, ram.data(), 100'000'000));
  }
  std::printf("guest stw  with-reservation %.3f ns  without %.3f ns  speedup %.2fx\n", with, without, with / without);
  return cpu.reserve_valid;
}
