// SPDX-License-Identifier: GPL-3.0-or-later
#include "native/guest_ram.h"
#include "native/guest_write_tracker.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <vector>

#define CHECK(x)                                                              \
  do {                                                                        \
    if (!(x)) {                                                               \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #x); \
      std::exit(1);                                                           \
    }                                                                         \
  } while (0)

using GekkoAOT::Native::GuestRam;
using GekkoAOT::Native::GuestWriteTracker;

int main() {
#if defined(_WIN32)
  // Guest RAM is write-watched only for the opt-in idle-loop prover.
  _putenv_s("GEKKOAOT_IDLE_LOOP_PROOF", "1");
#endif
  constexpr std::size_t kPage = GuestWriteTracker::kPageBytes;
  GuestRam ram(256u * kPage);
  GuestWriteTracker tracker;
  if (!tracker.Enable({std::span<std::uint8_t>(ram.data(), ram.size())})) {
    // No exact backend on this host: the idle-loop prover never parks.
    std::printf("guest write tracker unavailable; prover disabled on this host\n");
    return 0;
  }
  std::printf("backend=%s\n", tracker.BackendName());
  const auto page = [&](std::size_t index) {
    return reinterpret_cast<std::uintptr_t>(ram.data()) + index * kPage;
  };
  const auto has = [](const std::vector<std::uintptr_t>& pages, std::uintptr_t value) {
    return std::find(pages.begin(), pages.end(), value) != pages.end();
  };

  CHECK(tracker.Reset());
  std::vector<std::uintptr_t> pages;
  CHECK(tracker.Collect(&pages, 64u));
  CHECK(pages.empty());

  // Writes are reported even when they store the value already present, and
  // for never-touched pages, including ones first mapped by a read.
  auto* bytes = reinterpret_cast<volatile std::uint8_t*>(ram.data());
  bytes[3u * kPage + 17u] = bytes[3u * kPage + 17u];
  bytes[200u * kPage] = 0x5au;
  const std::uint8_t untouched = bytes[100u * kPage];
  bytes[100u * kPage] = static_cast<std::uint8_t>(untouched + 1u);
  CHECK(tracker.Collect(&pages, 64u));
  CHECK(has(pages, page(3u)) && has(pages, page(200u)) && has(pages, page(100u)));
  CHECK(!has(pages, page(4u)));

  // Collect starts a new window: the same pages written again are reported
  // again, and nothing else is.
  pages.clear();
  bytes[200u * kPage + 1u] = 0x11u;
  CHECK(tracker.Collect(&pages, 64u));
  CHECK(pages.size() == 1u && pages[0] == page(200u));
  pages.clear();
  CHECK(tracker.Collect(&pages, 64u));
  CHECK(pages.empty());

  // More written pages than the caller accepts is reported as a failure.
  for (std::size_t i = 0; i < 32u; ++i) bytes[i * kPage] = static_cast<std::uint8_t>(i);
  pages.clear();
  CHECK(!tracker.Collect(&pages, 8u));
  pages.clear();
  CHECK(tracker.Collect(&pages, 64u));
  CHECK(pages.empty());
  return 0;
}
