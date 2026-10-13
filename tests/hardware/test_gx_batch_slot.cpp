// SPDX-License-Identifier: GPL-3.0-or-later
#include "gx/host/host_bridge.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <future>
#include <vector>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "%d: %s\n", __LINE__, #x); std::exit(1); } } while (false)
extern "C" std::uint32_t gekkoaot_test_gx_bytes();
extern "C" std::uint8_t gekkoaot_test_gx_byte(std::uint32_t);
extern "C" void gekkoaot_test_gx_hold_writes(bool);
extern "C" std::uint32_t gekkoaot_test_gx_write_count();
extern "C" std::uint32_t gekkoaot_test_gx_write_pc(std::uint32_t);
extern "C" std::uint32_t gekkoaot_test_gx_write_size(std::uint32_t);

static void Env(const char* name, const char* value) {
#ifdef _WIN32
  CHECK(_putenv_s(name, value) == 0);
#else
  CHECK(setenv(name, value, 1) == 0);
#endif
}
struct Result {
  std::vector<std::uint8_t> bytes;
  std::vector<std::uint32_t> pcs, sizes;
};

static Result Exercise(const char* plugin, bool direct) {
  Env("GEKKOAOT_GX_DIRECT_BATCH_SLOT", direct ? "1" : "0");
  Env("GEKKOAOT_GX_WORKER_QUEUE_SLOTS", "4");
  Env("GEKKOAOT_WGPIPE_BATCH_BYTES", "512");
  GekkoAOT::Native::AddressSpace memory;
  GekkoAOT::GX::HostBridge bridge;
  CHECK(bridge.Open(plugin, memory));
  bridge.SetAsyncWritesEnabled(true);
  std::vector<std::uint8_t> expected;
  auto submit = [&](unsigned size, unsigned pc) {
    std::vector<std::uint8_t> input(size);
    for (unsigned i = 0; i < size; ++i) input[i] = (expected.size() + i) * 17u + pc;
    expected.insert(expected.end(), input.begin(), input.end());
    CHECK(bridge.WriteBurst(input.data(), size, pc));
    // The caller immediately reuses its memory. All published/unpublished
    // producer bytes must have their own lifetime, even when the parser stalls.
    std::fill(input.begin(), input.end(), 0xee);
  };
  gekkoaot_test_gx_hold_writes(true);
  for (unsigned i = 0; i < 40; ++i) submit(32, 0x80004000u + i * 4u);
  auto fence = std::async(std::launch::async, [&] { return bridge.FlushWrites(); });
  const auto state = fence.wait_for(std::chrono::milliseconds(20));
  gekkoaot_test_gx_hold_writes(false);
  CHECK(state == std::future_status::timeout);
  CHECK(fence.get());
  CHECK(gekkoaot_test_gx_bytes() == expected.size());

  // Force bounded queue backpressure. The fifth full packet waits for the
  // consumer; it must neither overwrite the active slot nor drop FIFO bytes.
  gekkoaot_test_gx_hold_writes(true);
  auto producer = std::async(std::launch::async, [&] {
    for (unsigned i = 0; i < 5u * 16u; ++i) submit(32, 0x80008000u + i * 4u);
    return bridge.FlushWrites();
  });
  const auto blocked = producer.wait_for(std::chrono::milliseconds(20));
  gekkoaot_test_gx_hold_writes(false);
  CHECK(blocked == std::future_status::timeout);
  CHECK(producer.get());

  // Mixed split packets, first-PC provenance, partial MMIO/cache fences,
  // repeated wrap of the four-slot ring and transitions to serialized mode.
  std::uint32_t seed = 0x242abcd;
  for (unsigned i = 0; i < 16000; ++i) {
    seed = seed * 1664525u + 1013904223u;
    unsigned size = i % 5u == 0u ? (seed % 9000u) + 1u : 32u;
    submit(size, 0x80010000u + i * 4u);
    if (i % 127u == 0u) CHECK(bridge.FlushWrites());
    if (i % 997u == 0u) { bridge.SetAsyncWritesEnabled(false); bridge.SetAsyncWritesEnabled(true); }
  }
  submit(13, 0x80090000u);
  bridge.SetAsyncWritesEnabled(false); // Publishes the final producer lease.
  submit(37, 0x80090004u); // Serialized fallback remains immediate.
  CHECK(bridge.FlushWrites());
  CHECK(gekkoaot_test_gx_bytes() == expected.size());
  Result result;
  result.bytes.reserve(expected.size());
  for (unsigned i = 0; i < expected.size(); ++i) {
    const auto value = gekkoaot_test_gx_byte(i);
    CHECK(value == expected[i]);
    result.bytes.push_back(value);
  }
  for (unsigned i = 0; i < gekkoaot_test_gx_write_count(); ++i) {
    result.pcs.push_back(gekkoaot_test_gx_write_pc(i));
    result.sizes.push_back(gekkoaot_test_gx_write_size(i));
  }
  bridge.Close();
  return result;
}

int main(int argc, char** argv) {
  CHECK(argc == 2 || argc == 3);
  if (argc == 3) {
    GekkoAOT::Native::AddressSpace memory;
    for (bool fast : {false, true}) {
      Env("GEKKOAOT_GX_IDLE_DRAIN_FAST", fast ? "1" : "0");
      GekkoAOT::GX::HostBridge bridge;
      CHECK(bridge.Open(argv[1], memory)); bridge.SetAsyncWritesEnabled(true);
      const auto start = std::chrono::steady_clock::now();
      for (unsigned i = 0; i < 2000000; ++i) CHECK(bridge.FlushWrites());
      const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();
      std::printf("Empty GX fence fast=%u calls=2000000 seconds=%.6f\n", fast, seconds);
      bridge.Close();
    }
    return 0;
  }
  Env("GEKKOAOT_GX_IDLE_DRAIN_FAST", "0");
  const auto reference = Exercise(argv[1], false);
  Env("GEKKOAOT_GX_IDLE_DRAIN_FAST", "1");
  const auto direct = Exercise(argv[1], true);
  CHECK(reference.bytes == direct.bytes);
  CHECK(reference.pcs == direct.pcs);
  CHECK(reference.sizes == direct.sizes);
  std::printf("Direct batch slot: %zu bytes and %zu packet PCs/sizes identical; fences/backpressure/lifetimes passed\n",
              direct.bytes.size(), direct.pcs.size());
}
