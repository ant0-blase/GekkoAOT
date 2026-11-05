// SPDX-License-Identifier: GPL-3.0-or-later
// Microbenchmarks for v372: (1) WGPIPE stores through the host try service
// versus the inline mid-line append, (2) Aurora's current render pass lookup
// through std::deque<RenderPass> versus the memoized pointer.
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <deque>

struct CPUState { std::uint64_t pad[64]; };
struct Runtime {
  CPUState cpu{};
  bool fast = true, ready = true, track = false;
  std::uint32_t cpu_gather = 0, gather_size = 0, first_pc = 0, lines = 0;
  std::array<std::uint8_t, 32> gather{};
  std::uint64_t sink = 0;
  void Line() { for (auto b : gather) sink += b; ++lines; gather_size = 0; }
};
static Runtime* g_runtime;
static Runtime* Self(CPUState* cpu) { return reinterpret_cast<Runtime*>(cpu); }

// Mirrors HostRuntime::NativeTryWgpipeWrite + TryGatherPartial.
__attribute__((noinline)) std::uint32_t TryWrite(CPUState* cpu, std::uint32_t address, std::uint64_t value,
                                                 std::uint8_t size, std::uint32_t pc) {
  auto* self = Self(cpu);
  if (self->track) ++self->sink;
  if ((address & 0x3fffffe0u) != 0x0c008000u) return 0;
  if (!self->fast || !self->ready || self->cpu_gather || self->gather_size + size >= 32u) return 0;
  if (!self->gather_size) self->first_pc = pc;
  auto* out = self->gather.data() + self->gather_size;
  for (unsigned i = 0; i < size; ++i) out[i] = std::uint8_t(value >> ((size - 1u - i) * 8u));
  self->gather_size += size;
  return 1;
}
__attribute__((noinline)) void SlowWrite(CPUState* cpu, std::uint64_t value, std::uint8_t size) {
  auto* self = Self(cpu);
  auto* out = self->gather.data() + self->gather_size;
  for (unsigned i = 0; i < size; ++i) out[i] = std::uint8_t(value >> ((size - 1u - i) * 8u));
  self->gather_size += size;
  if (self->gather_size == 32u) self->Line();
}
struct Window { CPUState* cpu; std::uint8_t* bytes; std::uint32_t* size; } g_window;

template <bool Inline>
__attribute__((noinline)) double Stores(CPUState* cpu, long count) {
  const auto start = std::chrono::steady_clock::now();
  for (long i = 0; i < count; ++i) {
    const std::uint64_t value = 0x3f800000u + (i & 255);
    bool accepted = false;
    if (Inline && g_window.cpu == cpu) {
      const std::uint32_t s = *g_window.size, next = s + 4u;
      if (s != 0u && next < 32u) {
        const std::uint32_t be = __builtin_bswap32(std::uint32_t(value));
        __builtin_memcpy(g_window.bytes + s, &be, 4);
        *g_window.size = next;
        accepted = true;
      }
    }
    if (!accepted && !TryWrite(cpu, 0xcc008000u, value, 4, 0x80001000u)) SlowWrite(cpu, value, 4);
  }
  return std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count() / count;
}

struct RenderPass { std::array<std::uint8_t, 7568> body{}; std::uint32_t commands = 0; };
struct Memo { const void* list; std::uint64_t frame; std::uint32_t index; RenderPass* pass; };
template <bool Memoized>
__attribute__((noinline)) double Lookups(std::deque<RenderPass>& passes, long count) {
  static Memo memo{};
  const std::uint64_t frame = 7;
  const auto start = std::chrono::steady_clock::now();
  for (long i = 0; i < count; ++i) {
    static volatile std::uint32_t current = 39;
    const std::uint32_t index = current;
    RenderPass* pass;
    if (Memoized) {
      if (memo.list == &passes && memo.frame == frame && memo.index == index) pass = memo.pass;
      else { pass = &passes[index]; memo = {&passes, frame, index, pass}; }
    } else {
      pass = &passes[index];
    }
    ++pass->commands;
    asm volatile("" ::: "memory");
  }
  return std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count() / count;
}

int main() {
  static Runtime runtime;
  g_runtime = &runtime;
  CPUState* cpu = reinterpret_cast<CPUState*>(&runtime);
  g_window = {cpu, runtime.gather.data(), &runtime.gather_size};
  double call = 1e9, inl = 1e9;
  for (int r = 0; r < 5; ++r) { call = std::min(call, Stores<false>(cpu, 50'000'000)); inl = std::min(inl, Stores<true>(cpu, 50'000'000)); }
  std::printf("wgpipe store  host-try %.3f ns  inline %.3f ns  speedup %.2fx\n", call, inl, call / inl);
  std::deque<RenderPass> passes(40);
  double deque_ns = 1e9, memo_ns = 1e9;
  for (int r = 0; r < 5; ++r) { deque_ns = std::min(deque_ns, Lookups<false>(passes, 50'000'000)); memo_ns = std::min(memo_ns, Lookups<true>(passes, 50'000'000)); }
  std::printf("render pass   deque    %.3f ns  memo   %.3f ns  speedup %.2fx\n", deque_ns, memo_ns, deque_ns / memo_ns);
  return int(runtime.sink & 0) + int(runtime.lines & 0);
}
