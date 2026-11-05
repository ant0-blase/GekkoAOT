// SPDX-License-Identifier: GPL-3.0-or-later
#include "gx/host/host_bridge.h"
#include <cstdio>
#include <cstdlib>
#include <string_view>
#include <future>
#include <chrono>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "%d: %s\n", __LINE__, #x); std::exit(1); } } while (false)
extern "C" std::uint32_t gekkoaot_test_gx_bytes();
extern "C" std::uint8_t gekkoaot_test_gx_byte(std::uint32_t);
extern "C" void gekkoaot_test_gx_publish_frame(std::uint32_t);
extern "C" void gekkoaot_test_gx_hold_writes(bool);
extern "C" std::uint32_t gekkoaot_test_gx_legacy_polls();
extern "C" std::uint32_t gekkoaot_test_gx_async_polls();
int main(int argc, char** argv) {
  CHECK(argc == 3);
  const bool legacy = std::string_view(argv[2]) == "legacy";
  // "fenced" runs the mailbox ABI with GEKKOAOT_GX_ASYNC_COMMANDS=0 (v36 fence).
  const bool fenced = legacy || std::string_view(argv[2]) == "fenced";
  GekkoAOT::Native::AddressSpace memory;
  GekkoAOT::GX::HostBridge bridge;
  CHECK(bridge.Open(argv[1], memory));
  if(legacy) {
    CHECK(!bridge.HasTypedArrayCommands());
    CHECK(!bridge.StageArrayBinding(0,0x80010000,12,0));
  }
  CHECK(bridge.HasFrameReadySignal());
  bridge.SetAsyncWritesEnabled(true);
  std::uint32_t frame = 0;
  CHECK(!bridge.ConsumeFrameReady(&frame)); // Idle poll uses the advertised ABI.
  CHECK(gekkoaot_test_gx_legacy_polls() == (legacy ? 1u : 0u));
  CHECK(gekkoaot_test_gx_async_polls() == (legacy ? 0u : 1u));
  const std::uint8_t first[]{0xaa, 0xbb}, second[]{0xcc, 0xdd};
  gekkoaot_test_gx_hold_writes(true);
  CHECK(bridge.WriteBurst(first, 2, 0));
  CHECK(gekkoaot_test_gx_bytes() == 0); // Still in the host burst batch.
  auto poll = std::async(std::launch::async, [&] { return bridge.ConsumeFrameReady(&frame); });
  const auto status = poll.wait_for(std::chrono::milliseconds(30));
  // Release the artificial parser stall even when an assertion would fail.
  gekkoaot_test_gx_hold_writes(false);
  // Legacy plugins and the opt-out keep the pending-FIFO fence. v300 polls the
  // atomic mailbox without waiting for the parser worker.
  CHECK(status == (fenced ? std::future_status::timeout : std::future_status::ready));
  CHECK(!poll.get());
  CHECK(bridge.FlushWrites());
  CHECK(gekkoaot_test_gx_bytes() == 2); // Residual data was published, never lost.
  CHECK(bridge.WriteBurst(second, 2, 0));
  gekkoaot_test_gx_publish_frame(0x1000);
  gekkoaot_test_gx_publish_frame(0x2000); // Latest complete snapshot wins.
  CHECK(bridge.ConsumeFrameReady(&frame));
  CHECK(frame == 0x2000);
  if (fenced) CHECK(gekkoaot_test_gx_bytes() == 4); // A real boundary retains the old fence.
  else CHECK(gekkoaot_test_gx_bytes() == 2);        // Still batched: no hidden drain.
  CHECK(gekkoaot_test_gx_byte(0) == 0xaa && gekkoaot_test_gx_byte(1) == 0xbb);
  CHECK(!bridge.ConsumeFrameReady(&frame));
  CHECK(bridge.FlushWrites()); // Explicit MMIO/FIFO ordering is unchanged.
  CHECK(gekkoaot_test_gx_bytes() == 4);
  CHECK(gekkoaot_test_gx_byte(2) == 0xcc && gekkoaot_test_gx_byte(3) == 0xdd);
  std::puts("Frame signal, publication fence and plugin compatibility passed");
}
