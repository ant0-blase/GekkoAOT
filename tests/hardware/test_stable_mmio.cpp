// SPDX-License-Identifier: GPL-3.0-or-later
#include "native/runtime.h"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "%d: %s\n", __LINE__, #x); std::exit(1); } } while (false)
extern "C" void gekkoaot_test_gx_publish_pe(std::uint32_t, std::uint32_t);
using namespace GekkoAOT::Native;
namespace GekkoAOT::Native {
struct HardwareTestAccess {
  static void Seed(HostRuntime& r, unsigned seed) {
    r.track_host_effects_ = true;
    r.pe_.Write(0xcc00100a, 3, 2);
    r.pe_.SetToken(seed, true);
    r.pe_.SetFinish();
    r.SyncNativePEInterrupts();
    r.cpu_.pc = 0x80012340;
    r.cpu_.gpr[20] = 0xdeadbeef;
    r.cpu_.external_read_count = seed % 256;
  }
  static void Compare(const HostRuntime& a, const HostRuntime& b) {
    CHECK(std::memcmp(&a.cpu_, &b.cpu_, offsetof(CPUState, external_read)) == 0);
    CHECK(a.cpu_.downcount == b.cpu_.downcount);
    CHECK(a.cpu_.cycle_budget == b.cpu_.cycle_budget);
    CHECK(a.fault_.kind == b.fault_.kind);
    CHECK(a.mmio_reads_ == b.mmio_reads_);
    CHECK(a.last_mmio_read_ == b.last_mmio_read_);
    CHECK(a.same_mmio_read_streak_ == b.same_mmio_read_streak_);
    CHECK(a.host_effects_ == b.host_effects_);
    CHECK(a.pi_.InterruptPending() == b.pi_.InterruptPending());
    CHECK(a.pe_.Token() == b.pe_.Token());
    CHECK(a.cp_.Status() == b.cp_.Status());
  }
};
}
static void Configure(const char* name, const char* value) {
#if defined(_WIN32)
  _putenv_s(name, value);
#else
  setenv(name, value, 1);
#endif
}
int main(int argc, char** argv) {
  CHECK(argc == 2);
  Configure("GEKKOAOT_NATIVE_HW", "1");
  Configure("GEKKOAOT_RUNTIME_PARALLEL", "1");
  auto a = std::make_unique<HostRuntime>(), b = std::make_unique<HostRuntime>();
  auto& ca = a->Cpu(); auto& cb = b->Cpu();
  const auto& service = HostRuntime::NativeStableMmioServices();
  CHECK(service.version == 1 && service.size == sizeof(service));
  constexpr std::array registers{0x0c000000u, 0x0c000002u, 0x0c000020u,
      0x0c001000u, 0x0c001002u, 0x0c001004u, 0x0c001006u,
      0x0c001008u, 0x0c00100au, 0x0c00100eu, 0x0c003000u, 0x0c003004u};
  for (unsigned trial = 0; trial < 256; ++trial) {
    HardwareTestAccess::Seed(*a, trial); HardwareTestAccess::Seed(*b, trial);
    for (auto physical : registers) {
      const auto address = physical | ((trial & 1) ? 0xc0000000u : 0x80000000u);
      const auto size = std::uint8_t(physical >= 0x0c003000u ? 4 : 2);
      const auto expected = ca.external_read(&ca, address, size);
      const auto packed = service.try_read(&cb, address, size);
      CHECK(packed >> 63);
      CHECK(std::uint32_t(packed) == expected);
      HardwareTestAccess::Compare(*a, *b);
    }
  }
  // Refusal has no CPU-state or telemetry effect; the generated fallback owns
  // synchronization, invalid widths, timer-driven devices and EFB readback.
  for (auto address : {0x80000000u, 0xcc000001u, 0xcc00100cu, 0xcc001010u,
                       0xcc00202eu, 0xcc003008u, 0xcc006400u, 0xdeadbeefu}) {
    const auto before = cb;
    CHECK(service.try_read(&cb, address, 2) == 0);
    CHECK(std::memcmp(&cb, &before, sizeof(cb)) == 0);
  }
  const auto before = cb;
  CHECK(service.try_read(&cb, 0xcc000000u, 1) == 0);
  CHECK(std::memcmp(&cb, &before, sizeof(cb)) == 0);
  cb.exception = 0x40;
  CHECK(service.try_read(&cb, 0xcc000000u, 2) == 0);
  cb.exception = 0;
  // A queued parser token must force the ordered fallback, which publishes it.
  CHECK(b->InitializeNativeGX(argv[1]));
  gekkoaot_test_gx_publish_pe(0x47, 0xabcd);
  CHECK(service.try_read(&cb, 0xcc00100e, 2) == 0);
  CHECK(cb.external_read(&cb, 0xcc00100e, 2) == 0xabcd);
  CHECK(std::uint32_t(service.try_read(&cb, 0xcc00100e, 2)) == 0xabcd);
  // Services always use the CPU's owning runtime, including distinct tokens.
  CHECK(std::uint32_t(service.try_read(&ca, 0xcc00100e, 2)) == 255);
  b->ShutdownNativeGX();
  Configure("GEKKOAOT_NATIVE_HW", "0");
  auto disabled = std::make_unique<HostRuntime>();
  CHECK(service.try_read(&disabled->Cpu(), 0xcc00100e, 2) == 0);
  std::puts("Stable CP/PE/PI values, epochs, IRQs, instance ownership and ordered fallback match");
}
