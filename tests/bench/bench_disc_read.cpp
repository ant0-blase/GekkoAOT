// SPDX-License-Identifier: GPL-3.0-or-later
// Disc read latency/throughput through nod for a load-like access pattern:
// whole files read in 32 KiB..1 MiB requests, files visited in random order.
#include <nod.h>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <random>
#include <vector>

int main(int argc, char** argv) {
  if (argc < 3) { std::fprintf(stderr, "usage: bench_disc_read <image> <preloader-threads> [request-kib]\n"); return 2; }
  NodDiscOptions options{};
  options.preloader_threads = unsigned(std::atoi(argv[2]));
  const std::size_t request = (argc > 3 ? std::size_t(std::atoi(argv[3])) : 128) * 1024;
  NodHandle* disc = nullptr;
  if (nod_disc_open(argv[1], &options, &disc) != NOD_RESULT_OK || !disc) { std::fprintf(stderr, "open failed\n"); return 1; }
  // Synthetic "files": 400 extents of 256 KiB..4 MiB across the first 1.2 GB.
  std::mt19937_64 rng(42);
  struct Extent { std::int64_t offset; std::size_t size; };
  std::vector<Extent> files;
  for (int i = 0; i < 400; ++i)
    files.push_back({std::int64_t(rng() % (1200ull << 20)) & ~0x7fffll, std::size_t(256 * 1024) << (rng() % 5)});
  std::vector<std::uint8_t> buffer(request);
  std::vector<double> latencies;
  std::size_t total = 0;
  const auto start = std::chrono::steady_clock::now();
  for (const auto& f : files) {
    for (std::size_t done = 0; done < f.size; done += request) {
      const auto t0 = std::chrono::steady_clock::now();
      if (nod_seek(disc, f.offset + std::int64_t(done), 0) < 0) return 1;
      std::size_t got = 0;
      while (got < request) {
        const auto n = nod_read(disc, buffer.data() + got, request - got);
        if (n <= 0) break;
        got += std::size_t(n);
      }
      latencies.push_back(std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count());
      total += got;
    }
  }
  const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  std::sort(latencies.begin(), latencies.end());
  std::printf("preload=%s req=%zuKiB  %.0f MB/s  reads=%zu  p50=%.0fus p90=%.0fus p99=%.0fus\n", argv[2], request / 1024,
              total / secs / 1e6, latencies.size(), latencies[latencies.size() / 2], latencies[latencies.size() * 9 / 10],
              latencies[latencies.size() * 99 / 100]);
  nod_free(disc);
  return 0;
}
