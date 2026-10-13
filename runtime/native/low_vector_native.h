#pragma once
// SPDX-License-Identifier: GPL-3.0-or-later

#include "address_space.h"
#include "core/cpu_state.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace GekkoAOT::Native {

// v294: the SDK installs this first-level exception leaf in RAM after boot,
// outside the immutable DOL AOT ranges. Only an exact instruction-visible
// proof admits the native leaf; custom vectors remain with the interpreter.
// The three patched fields are the SDK exception number and the signed
// LIS/ADDI handler address. No title-specific address is accepted here.
class NativeLowVector {
public:
  static constexpr std::size_t WordCount = 38u;
  static constexpr std::uint32_t ByteCount = WordCount * 4u;
  static constexpr std::uint32_t ContextBytes = 0x1a4u;

  static std::optional<NativeLowVector> Decode(std::span<const std::uint32_t> words) {
    if (words.size() != WordCount) return std::nullopt;
    constexpr std::array<std::uint32_t, WordCount> expected{
        0x7c9043a6u, 0x808000c0u, 0x9064000cu, 0x7c7042a6u,
        0x90640010u, 0x90a40014u, 0xa06401a2u, 0x60630002u,
        0xb06401a2u, 0x7c600026u, 0x90640080u, 0x7c6802a6u,
        0x90640084u, 0x7c6902a6u, 0x90640088u, 0x7c6102a6u,
        0x9064008cu, 0x7c7a02a6u, 0x90640198u, 0x7c7b02a6u,
        0x9064019cu, 0x7c651b78u, 0x60000000u, 0x7c6000a6u,
        0x60630030u, 0x7c7b03a6u, 0x38600000u, 0x808000d4u,
        0x54a507bdu, 0x40820014u, 0x3ca00000u, 0x38a50000u,
        0x7cba03a6u, 0x4c000064u, 0x546515bau, 0x80a53000u,
        0x7cba03a6u, 0x4c000064u};
    for (std::size_t i = 0; i < words.size(); ++i) {
      const std::uint32_t mask = i == 26u || i == 30u || i == 31u
                                     ? 0xffff0000u : 0xffffffffu;
      if ((words[i] & mask) != expected[i]) return std::nullopt;
    }
    NativeLowVector result;
    result.number_ = SignExtend16(words[26]);
    result.default_handler_ = (words[30] << 16u) + SignExtend16(words[31]);
    return result;
  }

  // All admission checks and potentially failing reads precede any mutation.
  // AddressSpace stores in the interpreter touch this same RAM backing, so
  // host write-watch faults retain their ordinary publication path. Exotic
  // contexts (MMIO, paged VM, low memory aliases, truncated regions) fall back
  // before changing CPUState, SPRG0, RAM, or the charged-cycle output.
  bool Execute(CPUState& cpu, AddressSpace& memory, std::uint32_t entry,
               std::uint32_t& sprg0, std::uint64_t& cycles) const {
    const auto physical = AddressSpace::ToPhysical(entry);
    constexpr std::uint32_t pages = 0x0098b3feu;
    if (cpu.pc != entry || (cpu.msr & 0xc000u) != 0u ||
        physical < 0x100u || physical >= 0x1800u ||
        (physical & 0xffu) != 0u || (pages & (1u << (physical >> 8u))) == 0u)
      return false;

    std::uint32_t context = 0u;
    std::uint32_t current_context = 0u;
    if (!memory.Read32(0xc0u, &context) ||
        !memory.Read32(0xd4u, &current_context))
      return false;
    const auto context_physical = AddressSpace::ToPhysical(context);
    // Exclude the vector code, global pointers and the exception table, whose
    // overlap would make sequential interpreter reads/stores observable.
    const bool mem1 = context_physical >= 0x4000u &&
                      context_physical < memory.Mem1().size();
    const bool mem2 = context_physical >= AddressSpace::Mem2PhysicalBase &&
                      std::uint64_t(context_physical) <
                          std::uint64_t(AddressSpace::Mem2PhysicalBase) + memory.Mem2().size();
    if (!mem1 && !mem2) return false;
    auto* payload = memory.Resolve(context, ContextBytes);
    if (!payload) return false;

    const bool recoverable = (cpu.srr1 & 2u) != 0u;
    std::uint32_t handler = default_handler_;
    if (recoverable && !memory.Read32(0x3000u + ((number_ << 2u) & 0x3fcu), &handler))
      return false;

    const auto old_r4 = cpu.gpr[4];
    const auto old_srr1 = cpu.srr1;
    const auto entry_msr = cpu.msr;
    // Preserve the SDK's instruction/store order, including exception state.
    sprg0 = old_r4;
    Write32(payload + 0x0cu, cpu.gpr[3]);
    Write32(payload + 0x10u, old_r4);
    Write32(payload + 0x14u, cpu.gpr[5]);
    const auto saved = std::uint16_t((std::uint16_t(payload[0x1a2u]) << 8u) |
                                      payload[0x1a3u] | 2u);
    payload[0x1a2u] = std::uint8_t(saved >> 8u);
    payload[0x1a3u] = std::uint8_t(saved);
    Write32(payload + 0x80u, cpu.cr);
    Write32(payload + 0x84u, cpu.lr);
    Write32(payload + 0x88u, cpu.ctr);
    Write32(payload + 0x8cu, cpu.xer);
    Write32(payload + 0x198u, cpu.srr0);
    Write32(payload + 0x19cu, old_srr1);

    // rlwinm. r5,r5,0,30,30 sets CR0 from SRR1[RI], including XER[SO].
    const auto cr0 = std::uint32_t(recoverable ? 4u : 2u) | (cpu.xer >> 31u);
    cpu.cr = (cpu.cr & 0x0fffffffu) | (cr0 << 28u);
    cpu.gpr[3] = number_;
    cpu.gpr[4] = current_context;
    cpu.gpr[5] = handler;
    cpu.srr0 = handler;
    cpu.srr1 = entry_msr | 0x30u;
    constexpr std::uint32_t rfi_mask = 0x87c0ffffu;
    cpu.msr = ((entry_msr & ~rfi_mask) | (cpu.srr1 & rfi_mask)) & ~0x40000u;
    cpu.pc = handler & ~3u;

    // The chassis consumes only the cause matching the accepted entry page.
    // These are the existing ExecuteLowMemoryInstruction acknowledgement bits.
    switch (physical) {
    case 0x500u: cpu.exception &= ~0x40u; break;
    case 0x200u: cpu.exception &= ~0x10u; break;
    case 0x300u: cpu.exception &= ~0x2u; break;
    case 0x600u: cpu.exception &= ~0x4u; break;
    case 0x700u: cpu.exception &= ~0x1u; cpu.program_exception = 0u; break;
    case 0x800u: cpu.exception &= ~0x20u; break;
    case 0xc00u: cpu.exception &= ~0x8u; break;
    default: break;
    }
    // Both RI paths execute 34 guest instructions; RFI costs two cycles.
    cycles = 35u;
    return true;
  }

private:
  static std::uint32_t SignExtend16(std::uint32_t word) {
    return static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<std::int16_t>(word)));
  }
  static void Write32(std::uint8_t* p, std::uint32_t v) {
    p[0] = std::uint8_t(v >> 24u); p[1] = std::uint8_t(v >> 16u);
    p[2] = std::uint8_t(v >> 8u); p[3] = std::uint8_t(v);
  }
  std::uint32_t number_ = 0u;
  std::uint32_t default_handler_ = 0u;
};

// Positive proofs are bound to instruction-cache publication, not data RAM
// writes. Call InvalidateLine for icbi and Reset for ICFI/HID0 ICE changes and
// runtime reset. Never store a proof while ICE is off. Decode failure is not
// cached: untouched I-cache lines may still observe later RAM writes.
class NativeLowVectorCache {
public:
  const NativeLowVector* Find(std::uint32_t entry) const {
    const auto physical = AddressSpace::ToPhysical(entry);
    if (physical >= 0x1800u || (physical & 0xffu) != 0u) return nullptr;
    const auto& slot = entries_[physical >> 8u];
    return slot ? &*slot : nullptr;
  }
  void Store(std::uint32_t entry, const NativeLowVector& proof) {
    const auto physical = AddressSpace::ToPhysical(entry);
    if (physical < 0x1800u && (physical & 0xffu) == 0u)
      entries_[physical >> 8u] = proof;
  }
  void InvalidateLine(std::uint32_t address) {
    const auto physical = AddressSpace::ToPhysical(address);
    if (physical < 0x1800u) entries_[physical >> 8u].reset();
  }
  void Reset() { for (auto& entry : entries_) entry.reset(); }

private:
  std::array<std::optional<NativeLowVector>, 24u> entries_{};
};

} // namespace GekkoAOT::Native
