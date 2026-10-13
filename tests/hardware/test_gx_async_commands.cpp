// SPDX-License-Identifier: GPL-3.0-or-later
// v300: memory publication and VI scanout are ordered GX-worker commands.
#include "gx/host/host_bridge.h"
#include <cstdio>
#include <cstdlib>
#include <vector>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "%d: %s\n", __LINE__, #x); std::exit(1); } } while (false)
extern "C" std::uint32_t gekkoaot_test_gx_command_count();
extern "C" std::uint32_t gekkoaot_test_gx_command(std::uint32_t);
extern "C" void gekkoaot_test_gx_hold_writes(bool);
namespace {
constexpr std::uint32_t kBurst = 0x42555200u, kMemory = 0x4d454d00u, kPresent = 0x50525300u;
std::vector<std::uint32_t> Log() {
  std::vector<std::uint32_t> out;
  for (std::uint32_t i = 0; i < gekkoaot_test_gx_command_count(); ++i)
    out.push_back(gekkoaot_test_gx_command(i));
  return out;
}
}
int main(int argc, char** argv) {
  CHECK(argc == 2);
  GekkoAOT::Native::AddressSpace memory;
  GekkoAOT::GX::HostBridge bridge;
  CHECK(bridge.Open(argv[1], memory));
  bridge.SetAsyncWritesEnabled(true);
  std::uint8_t line[32]{};
  // Stall the parser so every command below is still queued when issued: the
  // guest thread must not wait on any of them.
  gekkoaot_test_gx_hold_writes(true);
  CHECK(bridge.WriteBurst(line, 32, 0x80001000u));
  bridge.NotifyMemoryWrite(0x80100000u, 64u);
  bridge.NotifyCacheControl(1u, 0x80100040u);  // dcbf line extends the range
  bridge.NotifyCacheControl(1u, 0x80100060u);
  bridge.NotifyCacheControl(7u, 0x80200000u);  // not a publication operation
  bridge.NotifyMemoryWrite(0x80300000u, 32u);  // disjoint range, same command
  const bool typed=bridge.HasTypedArrayCommands();
  if(typed) CHECK(bridge.StageArrayBinding(4u,0x80123400u,8u,19u));
  else CHECK(!bridge.StageArrayBinding(4u,0x80123400u,8u,19u));
  CHECK(bridge.WriteBurst(line, 32, 0x80001004u));
  bridge.PresentNow(0x00300000u, 0x00300500u, 640u, 1280u, 240u);
  bridge.PresentNow(0x00400000u, 0x00400000u, 640u, 0u, 240u);  // partial geometry
  CHECK(Log().empty());  // the worker has not run anything yet
  gekkoaot_test_gx_hold_writes(false);
  CHECK(bridge.FlushWrites());
  std::vector<std::uint32_t> expected{
      kBurst, 32u,
      kMemory, 0x80100000u, 0x80u,
      kMemory, 0x80300000u, 32u,
      kBurst, 32u,
      kPresent, 0x00300000u, 0x00300500u, 640u, 1280u, 240u,
      kPresent, 0x00400000u, 0x00400000u, 0u, 0u, 0u};
  if(typed) {
    expected.erase(expected.begin()+8,expected.begin()+10);
    expected.insert(expected.begin()+8,{0x4e415400u,1u,kBurst,19u,0x41525200u,4u,0x80123400u,8u,0u,kBurst,13u});
  }
  const auto log = Log();
  if (log != expected) {
    for (auto v : log) std::fprintf(stderr, "%08x ", v);
    std::fprintf(stderr, "\n");
  }
  CHECK(log == expected);
  std::puts("Ordered GX worker commands passed");
}
