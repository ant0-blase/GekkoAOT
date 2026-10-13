// SPDX-License-Identifier: GPL-3.0-or-later
#include "os/native_os.h"

#include <cmath>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <vector>

#define CHECK(expr) do { if (!(expr)) { std::cerr << __LINE__ << ": " #expr "\n"; std::exit(1); } } while (false)

using GekkoAOT::NativeOS::Kind;
using GekkoAOT::NativeOS::Service;

namespace {
constexpr std::uint32_t kMemBase = 0x80000000u;
constexpr std::uint32_t kContextA = 0x80010000u;
constexpr std::uint32_t kContextB = 0x80011000u;
constexpr std::uint32_t kCurrentContext = 0x800000d4u;
constexpr std::uint32_t kFpuContext = 0x800000d8u;
constexpr std::uint32_t kCtxFpr = 0x090u;
constexpr std::uint32_t kCtxFpscr = 0x194u;
constexpr std::uint32_t kCtxSrr1 = 0x19cu;
constexpr std::uint32_t kCtxState = 0x1a2u;
constexpr std::uint32_t kCtxPsf = 0x1c8u;

std::uint64_t Bits(double value) {
  std::uint64_t out = 0;
  std::memcpy(&out, &value, sizeof(out));
  return out;
}

struct Fixture {
  std::vector<std::uint8_t> mem = std::vector<std::uint8_t>(2u * 1024u * 1024u);
  CPUState cpu{};
  Service& os = Service::Get();
  unsigned calls = 0;

  static void* Pointer(CPUState* state, std::uint32_t address, std::uint32_t size) {
    auto& self = *static_cast<Fixture*>(state->external_user_data);
    if (address < kMemBase) return nullptr;
    const std::uint64_t offset = std::uint64_t(address) - kMemBase;
    if (offset + size > self.mem.size()) return nullptr;
    return self.mem.data() + offset;
  }

  Fixture(bool eager = true) {
#ifdef _WIN32
    _putenv_s("GEKKOAOT_NATIVE_OS_EAGER_FPU", eager ? "1" : "0");
#else
    setenv("GEKKOAOT_NATIVE_OS_EAGER_FPU", eager ? "1" : "0", 1);
#endif
    os.Reset();
    cpu.external_user_data = this;
    cpu.external_pointer = &Pointer;
    cpu.ram = mem.data();
    cpu.ram_size = static_cast<std::uint32_t>(mem.size());
    cpu.msr = 0x9032u;
    Word(kCurrentContext, kContextA);
    Word(kFpuContext, kContextA);
    Word(kContextA + kCtxSrr1, 0x9032u);
    Word(kContextB + kCtxSrr1, 0x9032u);
  }

  void Word(std::uint32_t address, std::uint32_t value) {
    auto* p = mem.data() + (address - kMemBase);
    p[0] = static_cast<std::uint8_t>(value >> 24);
    p[1] = static_cast<std::uint8_t>(value >> 16);
    p[2] = static_cast<std::uint8_t>(value >> 8);
    p[3] = static_cast<std::uint8_t>(value);
  }

  std::uint32_t Word(std::uint32_t address) const {
    const auto* p = mem.data() + (address - kMemBase);
    return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) |
           (std::uint32_t(p[2]) << 8) | p[3];
  }

  std::uint16_t Half(std::uint32_t address) const {
    const auto* p = mem.data() + (address - kMemBase);
    return static_cast<std::uint16_t>((std::uint16_t(p[0]) << 8) | p[1]);
  }

  void Half(std::uint32_t address, std::uint16_t value) {
    auto* p = mem.data() + (address - kMemBase);
    p[0] = static_cast<std::uint8_t>(value >> 8);
    p[1] = static_cast<std::uint8_t>(value);
  }

  void Double(std::uint32_t address, double value) {
    auto bits = Bits(value);
    auto* p = mem.data() + (address - kMemBase);
    for (unsigned i = 0; i < 8; ++i)
      p[i] = static_cast<std::uint8_t>(bits >> (56u - i * 8u));
  }

  template<class... Args> void Call(Kind kind, Args... args) {
    const std::array<std::uint32_t, sizeof...(args)> parameters{static_cast<std::uint32_t>(args)...};
    for (unsigned i = 0; i < sizeof...(args); ++i) cpu.gpr[3 + i] = parameters[i];
    cpu.lr = 0x800a0000u + (++calls * 4u);
    CHECK(os.Dispatch(kind, &cpu));
  }
};

void TestTemporaryContextDoesNotClobberInterruptedFpState() {
  Fixture f;
  for (unsigned i = 0; i < 32; ++i) {
    f.cpu.fpr[i] = 1000.0 + i * 0.25;
    f.cpu.ps1[i] = -500.0 - i * 0.5;
  }
  f.cpu.fpscr = 0x12345678u;

  // This models the SDK IRQ pattern: save interrupted context, clear a local
  // exception context, make it current, run a callback, then restore the
  // interrupted context. Native AOT must isolate FPR/PS state even though it
  // cannot take the Gekko FP-unavailable trap used by the real SDK.
  f.Call(Kind::SetCurrentContext, kContextB);
  for (unsigned i = 0; i < 32; ++i) {
    f.cpu.fpr[i] = 2000.0 + i;
    f.cpu.ps1[i] = 3000.0 + i;
  }
  f.cpu.fpscr = 0xa5a5a5a5u;
  f.Call(Kind::SetCurrentContext, kContextA);

  for (unsigned i = 0; i < 32; ++i) {
    CHECK(f.cpu.fpr[i] == 1000.0 + i * 0.25);
    CHECK(f.cpu.ps1[i] == -500.0 - i * 0.5);
  }
  CHECK(f.cpu.fpscr == 0x12345678u);
  CHECK(f.Word(kFpuContext) == kContextA); // OSSetCurrentContext must not eagerly rewrite SDK owner.
}

void TestGuestSavedFpuPayloadIsAuthoritative() {
  Fixture f;
  f.Half(kContextB + kCtxState, 1u);
  f.Word(kContextB + kCtxFpscr, 0x0badc0deu);
  for (unsigned i = 0; i < 32; ++i) {
    f.Double(kContextB + kCtxFpr + i * 8u, 10.0 + i);
    f.Double(kContextB + kCtxPsf + i * 8u, 20.0 + i * 2.0);
  }

  f.Call(Kind::SetCurrentContext, kContextB);
  CHECK(f.cpu.fpscr == 0x0badc0deu);
  for (unsigned i = 0; i < 32; ++i) {
    CHECK(f.cpu.fpr[i] == 10.0 + i);
    CHECK(f.cpu.ps1[i] == 20.0 + i * 2.0);
  }
}

void TestNativeFpuSdkEntryPoints() {
  Fixture f;
  for (unsigned i = 0; i < 32; ++i) {
    f.cpu.fpr[i] = 40.0 + i;
    f.cpu.ps1[i] = 80.0 + i;
  }
  f.cpu.fpscr = 0x00c0ffeeu;
  f.Call(Kind::SaveFpuContext, kContextA);
  CHECK((f.Half(kContextA + kCtxState) & 1u) != 0u);

  for (unsigned i = 0; i < 32; ++i) {
    f.cpu.fpr[i] = 0.0;
    f.cpu.ps1[i] = 0.0;
  }
  f.cpu.fpscr = 0;
  f.Call(Kind::LoadFpuContext, kContextA);
  CHECK(f.cpu.fpscr == 0x00c0ffeeu);
  for (unsigned i = 0; i < 32; ++i) {
    CHECK(f.cpu.fpr[i] == 40.0 + i);
    CHECK(f.cpu.ps1[i] == 80.0 + i);
  }
}

void TestLiveSavedFpuPayloadTracksContextSwitches() {
  Fixture f;
  f.cpu.fpr[30] = 12.0;
  f.cpu.ps1[30] = 13.0;
  f.Call(Kind::SaveFpuContext, kContextA);
  // Once FPSAVED is set, restoring a context reads its guest payload. The
  // native eager ownership transfer must publish newer live registers there.
  f.cpu.fpr[30] = 4503601774854144.0;
  f.cpu.ps1[30] = 14.0;
  f.cpu.fpscr = 0x00004000u;
  f.Call(Kind::SetCurrentContext, kContextB);
  f.cpu.fpr[30] = 99.0;
  f.cpu.ps1[30] = 100.0;
  f.Call(Kind::SetCurrentContext, kContextA);
  CHECK(f.cpu.fpr[30] == 4503601774854144.0);
  CHECK(f.cpu.ps1[30] == 14.0);
  CHECK(f.cpu.fpscr == 0x00004000u);

  // A cooperative save must also update an already materialized payload.
  f.cpu.fpr[30] = 15.0;
  f.cpu.ps1[30] = 16.0;
  f.Call(Kind::SaveContext, kContextA);
  f.cpu.fpr[30] = 101.0;
  f.cpu.ps1[30] = 102.0;
  f.Call(Kind::LoadContext, kContextA);
  CHECK(f.cpu.fpr[30] == 15.0);
  CHECK(f.cpu.ps1[30] == 16.0);
}

struct FpuBank {
  std::array<std::uint64_t, 32> fpr{}, ps1{};
  std::uint32_t fpscr = 0;
  explicit FpuBank(const CPUState& cpu) : fpscr(cpu.fpscr) {
    for (unsigned i = 0; i < 32; ++i) {
      fpr[i] = Bits(cpu.fpr[i]);
      ps1[i] = Bits(cpu.ps1[i]);
    }
  }
  void Check(const CPUState& cpu) const {
    CHECK(cpu.fpscr == fpscr);
    for (unsigned i = 0; i < 32; ++i) {
      CHECK(Bits(cpu.fpr[i]) == fpr[i]);
      CHECK(Bits(cpu.ps1[i]) == ps1[i]);
    }
  }
};

void FillBank(CPUState& cpu, double base) {
  for (unsigned i = 0; i < 32; ++i) {
    cpu.fpr[i] = base + i * 0.25;
    cpu.ps1[i] = -base - i * 0.5;
  }
  cpu.fpr[26] = 4503601774854144.0; // integer-to-float conversion bias
  cpu.ps1[31] = -0.0;
  cpu.fpscr = 0x82008000u;
}

void TestLazyContextOperationsLeavePhysicalBankAndSavedPayloadAlone() {
  Fixture f(false);
#ifdef _WIN32
  _putenv_s("GEKKOAOT_NATIVE_OS_EAGER_FPU", "");
#else
  unsetenv("GEKKOAOT_NATIVE_OS_EAGER_FPU");
#endif
  f.os.Reset(); // the default policy must also preserve SDK lazy ownership
  FillBank(f.cpu, 800.0);
  const FpuBank physical(f.cpu);
  // A valid payload for a context which has not acquired the live FPU. Loading
  // it on OSSetCurrentContext would corrupt the old owner's subsequent save.
  f.Half(kContextB + kCtxState, 1u);
  f.Word(kContextB + kCtxFpscr, 0x12345678u);
  for (unsigned i = 0; i < 32; ++i) {
    f.Double(kContextB + kCtxFpr + i * 8u, 2000.0 + i);
    f.Double(kContextB + kCtxPsf + i * 8u, -3000.0 - i);
  }
  const auto original = f.mem;
  f.Call(Kind::SetCurrentContext, kContextB);
  physical.Check(f.cpu);
  CHECK(f.Word(kFpuContext) == kContextA);
  CHECK((f.cpu.msr & 0x2000u) == 0u); // next FP op takes the SDK lazy transfer
  f.Call(Kind::SaveContext, kContextB);
  CHECK(std::memcmp(f.mem.data() + 0x11090, original.data() + 0x11090, 0x108) == 0);
  CHECK(std::memcmp(f.mem.data() + 0x111c8, original.data() + 0x111c8, 0x100) == 0);
  physical.Check(f.cpu);
  f.Call(Kind::LoadContext, kContextB);
  physical.Check(f.cpu);
  CHECK(f.Word(kFpuContext) == kContextA);

  // An interrupt context which never executes FP must not acquire or reload
  // the FPU merely because OSSetCurrentContext/OSLoadContext are native.
  f.Call(Kind::SetCurrentContext, kContextA);
  physical.Check(f.cpu);
  CHECK(f.Word(kFpuContext) == kContextA);
}

void TestLazyOwnershipTransferDoesNotDoubleSaveTheWrongContext() {
  Fixture f(false);
  FillBank(f.cpu, 200.0);
  const FpuBank b(f.cpu);
  f.Call(Kind::SaveFpuContext, kContextB);
  FillBank(f.cpu, 100.0);
  const FpuBank a(f.cpu);
  // Independent SDK sequence: SetCurrentContext updates MSR/current pointers;
  // only the first FP instruction saves the owner, sets D8, and loads target.
  for (unsigned i = 0; i < 4096; ++i) {
    f.Call(Kind::SetCurrentContext, kContextB);
    a.Check(f.cpu);
    CHECK(f.Word(kFpuContext) == kContextA);
    f.Call(Kind::SaveFpuContext, kContextA);
    f.Word(kFpuContext, kContextB);
    f.Call(Kind::LoadFpuContext, kContextB);
    b.Check(f.cpu);
    f.cpu.msr |= 0x2000u;
    f.Call(Kind::SetCurrentContext, kContextA);
    b.Check(f.cpu);
    CHECK(f.Word(kFpuContext) == kContextB);
    f.Call(Kind::SaveFpuContext, kContextB);
    f.Word(kFpuContext, kContextA);
    f.Call(Kind::LoadFpuContext, kContextA);
    a.Check(f.cpu);
    f.cpu.msr |= 0x2000u;
  }
}
} // namespace

int main() {
  TestTemporaryContextDoesNotClobberInterruptedFpState();
  TestGuestSavedFpuPayloadIsAuthoritative();
  TestNativeFpuSdkEntryPoints();
  TestLiveSavedFpuPayloadTracksContextSwitches();
  TestLazyContextOperationsLeavePhysicalBankAndSavedPayloadAlone();
  TestLazyOwnershipTransferDoesNotDoubleSaveTheWrongContext();
  std::cout << "NativeOS OSContext/FPU parity tests passed\n";
}
