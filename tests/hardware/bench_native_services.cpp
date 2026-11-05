// SPDX-License-Identifier: GPL-3.0-or-later
// Reproduce: build/bin/gekkoaot-bench-native-services > native-services.json
#include "card/sdk_card.h"
#include "native/runtime.h"
#include "platform/input_snapshot.h"
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <future>
#include <iostream>
#include <new>
#include <vector>
namespace {
std::atomic<std::uint64_t> allocations{0};
std::atomic<bool> counting{false};
volatile std::uint64_t checksum = 0;
} // namespace
void *operator new(std::size_t n) {
  if (counting.load(std::memory_order_relaxed))
    allocations.fetch_add(1, std::memory_order_relaxed);
  if (auto *p = std::malloc(n ? n : 1))
    return p;
  throw std::bad_alloc();
}
void operator delete(void *p) noexcept { std::free(p); }
void operator delete(void *p, std::size_t) noexcept { std::free(p); }
using namespace GekkoAOT;
using Clock = std::chrono::steady_clock;
struct Measurement {
  double wall = 0, cpu = 0, alloc = 0;
};
template <class F> Measurement Measure(unsigned n, F fn) {
  for (unsigned i = 0; i < 4; ++i)
    fn(i);
  allocations = 0;
  counting = true;
  const auto cpu = std::clock();
  const auto start = Clock::now();
  for (unsigned i = 0; i < n; ++i) {
    std::atomic_signal_fence(std::memory_order_seq_cst);fn(i);
  }
  const auto stop = Clock::now();
  const auto used = std::clock() - cpu;
  counting = false;
  return {std::chrono::duration<double, std::nano>(stop - start).count() / n,
          double(used) * 1e9 / CLOCKS_PER_SEC / n,
          double(allocations.load()) / n};
}
void Print(const char *name, Measurement m, bool comma = true) {
  std::cout << "  \"" << name << "\": {\"wall_ns_per_op\":" << m.wall
            << ",\"process_cpu_ns_per_op\":" << m.cpu
            << ",\"cpp_allocations_per_op\":" << m.alloc << "}"
            << (comma ? "," : "") << "\n";
}
bool Fill(void *, std::uint64_t offset, void *out, std::uint32_t bytes) {
  std::memset(out, int(offset), bytes);
  return true;
}
int main() {
  const auto root = std::filesystem::temp_directory_path() /
                    ("gekkoaot-native-bench-" +
                     std::to_string(Clock::now().time_since_epoch().count()));
  std::filesystem::create_directories(root);
  NativeHW::EXI::HostCard image(root / "card.raw", 0);
  Card::SdkCard card;
  const std::array<std::uint8_t, 6> id{'G', 'T', 'E', 'S', '0', '1'};
  card.SetDisk(id);
  if (card.Mount(&image))
    return 1;
  Card::SdkCard::Info file;
  const std::array<std::uint8_t, 1> name{'s'};
  if (card.Create(name, 8192, 0, file) || !card.Submit() || card.Wait())
    return 1;
  std::array<std::uint8_t, 8192> data{}, out{};
  for (unsigned i = 0; i < data.size(); ++i)
    data[i] = i * 7;
  if (card.Write(file, data, 0) || !card.Submit() || card.Wait())
    return 1;
  const auto legacy = [&](unsigned iteration) {
    for (unsigned page = 0; page < 16; ++page) {
      const auto addr = 5 * 8192 + page * 512;
      image.Select(true);
      std::uint32_t cmd = 0x52000000u | ((addr >> 17) << 16) |
                          (((addr >> 9) & 255) << 8) | ((addr >> 7) & 3);
      image.Immediate(1, 4, &cmd);
      cmd = (addr & 127) << 24;
      image.Immediate(1, 4, &cmd);
      image.Dma(0, out.data() + page * 512, 512);
      image.Select(false);
    }
    checksum = checksum + out[iteration & 8191];
  };
  legacy(0);
  if (out != data)
    return 2;
  const auto old_card = Measure(100000, legacy);
  const auto new_card = Measure(100000, [&](unsigned i) {
    if (card.Read(file, out, 0))
      std::abort();
    checksum = checksum + out[i & 8191];
  });
  Input::PadSnapshot snapshot;
  std::array<Input::Pad, 4> pads{};
  for (auto &p : pads) {
    p.connected = true;
    p.buttons = 0x1234;
    p.rumble = true;
  }
  snapshot.Publish(pads);
  std::array<std::uint8_t, 48> status{};
  const auto input = Measure(1000000, [&](unsigned i) {
    checksum = checksum + snapshot.ReadEncoded(status.data()) + status[i % 48];
  });
  Native::DiscIoWorker worker;
  worker.Start(nullptr, Fill);
  std::vector<std::uint8_t> bytes;
  const auto persistent = Measure(10000, [&](unsigned i) {
    Native::DiscIoWorker::Result result;
    if (!worker.Submit(i, i, 4096) || !worker.WaitCollect(result, bytes) ||
        !result.ok)
      std::abort();
    checksum = checksum + bytes[0];
  });
  const auto asynchronous = Measure(10000, [&](unsigned i) {
    auto request = std::async(std::launch::async, [i] {
      std::vector<std::uint8_t> b(4096);
      Fill(nullptr, i, b.data(), b.size());
      return b;
    });
    auto b = request.get();
    checksum = checksum + b[0];
  });
  worker.Stop();
  Native::AddressSpace memory;
  HW::LC::NativeLC lc;
  auto *ram = memory.Resolve(0x80010000, 4096);
  std::memcpy(ram, data.data(), 4096);
  const auto lc_adapter = Measure(200000, [&](unsigned i) {
    if (!lc.MemoryToLockedCache(memory, 0xe0000000, 0x80010000, 128))
      std::abort();
    checksum = checksum + static_cast<const std::uint8_t *>(
                              lc.Pointer(0xe0000000, 4096))[i & 4095];
  });
  auto *locked = static_cast<std::uint8_t *>(lc.Pointer(0xe0000000, 4096));
  const auto lc_direct = Measure(200000, [&](unsigned i) {
    std::memcpy(locked, ram, 4096);
    checksum = checksum + locked[i & 4095];
  });
  const auto generic_ram = Measure(1000000, [&](unsigned) {
    std::uint64_t value = 0;
    if (!memory.Read64(0x80010000, &value))
      std::abort();
    checksum = checksum + value;
  });
  const auto direct_ram = Measure(1000000, [&](unsigned) {
    std::uint64_t value;
    std::memcpy(&value, ram, 8);
#if defined(__GNUC__) || defined(__clang__)
    value = __builtin_bswap64(value);
#else
 value=((value&0xff)<<56)|((value&0xff00)<<40)|((value&0xff0000)<<24)|((value&0xff000000)<<8)|((value>>8)&0xff000000)|((value>>24)&0xff0000)|((value>>40)&0xff00)|(value>>56);
#endif
    checksum = checksum + value;
  });
  std::cout << "{\n  \"scope\":\"isolated service microbenchmarks; no game-FPS "
               "or device-latency claim\",\n  \"card_bytes\":8192,\n  "
               "\"io_bytes\":4096,\n";
  Print("card_exi_512byte_transactions", old_card);
  Print("card_sdk_cached_read", new_card);
  Print("pad_snapshot_read", input);
  Print("io_std_async_per_request", asynchronous);
  Print("io_persistent_worker", persistent);
  Print("lc_existing_adapter", lc_adapter);
  Print("lc_direct_copy", lc_direct);
  Print("ram_generic_helper", generic_ram);
  Print("ram_direct_lowering", direct_ram, false);
  std::cout << "}\n";
  card.Unmount();
  std::filesystem::remove_all(root);
  if (new_card.alloc != 0 || input.alloc != 0 || persistent.alloc != 0 ||
      lc_direct.alloc != 0 || direct_ram.alloc != 0)
    return 3;
}
