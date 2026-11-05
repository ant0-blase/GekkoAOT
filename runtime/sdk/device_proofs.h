#pragma once
// SPDX-License-Identifier: GPL-3.0-or-later
#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include "word_proof.h"
namespace GekkoAOTSdk {
inline std::uint32_t DeviceWord(const std::uint8_t *p) {
  return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) |
         (std::uint32_t(p[2]) << 8) | p[3];
}
// Structural signatures erase relocated addresses. An AI counter leaf must
// actually load the AI counter, never an unrelated game RAM global.
inline constexpr WordProof kAiCounterLoadProof{"FFF", 0x6a6733c10f093382ull};
inline constexpr WordProof kAiCounterOffsetProof{"FFFF", 0x450837738729a45eull};
inline bool ProveAiCounter(std::span<const std::uint8_t> code) {
  if(code.size()<12)return false;
  if(MatchesWordProof(code.data(),code.size(),kAiCounterLoadProof))return true;
  return code.size()>=16 && MatchesWordProof(code.data(),code.size(),kAiCounterOffsetProof);
}
inline constexpr WordProof kCardControlProof{"FHHFFF", 0x8e37651930ac8847ull};
inline std::optional<std::uint32_t> ProveCardControl(std::span<const std::uint8_t> code) {
  if (code.size() != 24 || !MatchesWordProof(code.data(), code.size(), kCardControlProof))
    return std::nullopt;
  return (DeviceWord(code.data()+4) << 16) + std::uint32_t(std::int16_t(DeviceWord(code.data()+8)));
}
// Recover the software AnalogMode cell only from a proven PAD setter.
// Track argument provenance across preserved-register moves and SDK calls.
inline std::optional<std::int16_t> ProvePadAnalogModeCell(std::span<const std::uint8_t> code) {
  std::array<unsigned,32> value{};value[3]=1;
  std::optional<std::int16_t> cell;
  for(unsigned off=0;off+4<=code.size();off+=4){
    const auto w=DeviceWord(code.data()+off);
    const unsigned op=w>>26,rs=(w>>21)&31,ra=(w>>16)&31,rb=(w>>11)&31;
    if(w==0x4e800020u)return cell;
    if((w&0xfc000003u)==0x48000001u){for(unsigned r=0;r<13;++r)value[r]=0;continue;}
    if(op==31 && ((w>>1)&1023)==444 && rs==rb){value[ra]=value[rs];continue;}
    if(op==14){value[rs]=ra && std::int16_t(w)==0?value[ra]:0;continue;}
    if(op==21){value[ra]=(w&0x0000ffffu)==0x402eu && value[rs]==1?2:0;continue;}
    if(op==36 && ra==13 && value[rs]==2){
      if(cell && *cell!=std::int16_t(w))return std::nullopt;
      cell=std::int16_t(w);continue;
    }
    if(op==15 || (op>=32 && op<=35))value[rs]=0;
    if(op>=24 && op<=29)value[ra]=0;
  }
  return std::nullopt;
}
// Newer SDKs may inline CARDGetDiskID entirely. The public disk setter
// independently proves both 0x110-byte slots and the actual shared NONE ID.
inline constexpr WordProof kCardDiskSetterProof{"FHHFFFFFFFFFFF", 0x67cdfe0f64c996a8ull};
inline std::optional<std::uint32_t> ProveCardDiskSetter(std::span<const std::uint8_t> code) {
  if(code.size()!=56 || !MatchesWordProof(code.data(),code.size(),kCardDiskSetterProof))
    return std::nullopt;
  const auto w=[&](unsigned n){return DeviceWord(code.data()+4*n);};
  return (w(1)<<16)+std::uint32_t(std::int16_t(w(2)));
}
struct LegacyCardControlProof {
  std::uint32_t base, disk_none;
  std::int16_t disk_sda;
};
// SDK 2001 CARDInit + SetDiskID + GetControlBlock are adjacent. Prove the
// shared global, both independently formed control bases, stride and offsets.
inline constexpr WordProof kLegacyCardControlProof{
    "F-----HF---HH--FF-F--FF-F----------FF--HH-FF---------------FHHFF----F--------FF-------F",
    0x286929ab93075740ull};
inline std::optional<LegacyCardControlProof> ProveLegacyCardControl(std::span<const std::uint8_t> code) {
  if(code.size()!=348)return std::nullopt;
  const auto w=[&](unsigned n){return DeviceWord(code.data()+4*n);};
  // Word 41 stores to the SDA cell word 6 loads; word 53 repeats word 6.
  if(!MatchesWordProof(code.data(),code.size(),kLegacyCardControlProof) ||
     w(41)!=(0x906d0000u|(w(6)&0xffffu)) || w(53)!=w(6))return std::nullopt;
  const auto base=(w(11)<<16)+std::uint32_t(std::int16_t(w(12)));
  if(base!=(w(60)<<16)+std::uint32_t(std::int16_t(w(61))))return std::nullopt;
  return LegacyCardControlProof{base,(w(39)<<16)+std::uint32_t(std::int16_t(w(40))),std::int16_t(w(6))};
}
// These public SRAM adapters are generic nine-instruction call wrappers.
// Their body hash alone also matches ordinary game allocators/constructors.
// Require both adjacent adapters, their identical actual callee, exact mode
// constants and the following SDK control getter, before replacing either.
inline constexpr WordProof kSramUnlockFamilyProof{"FFFFBFFFFFFFFBFFFFHHFF", 0x68c50de68797f7a4ull};
inline bool ProveSramUnlockFamily(std::span<const std::uint8_t> code,
                                 std::uint32_t normal_pc) {
  if(code.size()!=88 || !MatchesWordProof(code.data(),code.size(),kSramUnlockFamilyProof))return false;
  const auto w=[&](unsigned n){return DeviceWord(code.data()+4*n);};
  const auto target=[&](unsigned n){return normal_pc+4*n+(std::uint32_t(std::int32_t(w(n)<<6)>>6)&~3u);};
  return target(4)==target(13) && target(4)<normal_pc && normal_pc-target(4)<=0x1000;
}
inline constexpr WordProof kSiPolicyProof{"FFFFFFFBHHFFFFFFFBFFFFFF", 0xf52d2e126c065749ull};
inline constexpr WordProof kSiPolicyStoreProof{"FFFFFFFBHHFFFFFFFFFFBFFFFFF", 0x24593b97396c8833ull};
inline std::optional<std::uint32_t> ProveSiPolicy(std::span<const std::uint8_t> code) {
  const auto* proof=code.size()==96 ? &kSiPolicyProof : code.size()==108 ? &kSiPolicyStoreProof : nullptr;
  if(!proof || !MatchesWordProof(code.data(),code.size(),*proof))return std::nullopt;
  const auto w=[&](unsigned n){return DeviceWord(code.data()+4*n);};
  return (w(8)<<16)+std::uint32_t(std::int16_t(w(9)))+4;
}
struct MemoryInitProof {
  std::uint32_t handler = 0, reset_info = 0;
};
// Recover only ABI data from an independently identified SDK initialiser.
// All five handler registrations must target the named setter with explicit
// channel constants, and the reset registration must target its named API.
inline std::optional<MemoryInitProof> ProveMemoryInit(
    std::span<const std::uint8_t> code, std::uint32_t pc,
    std::uint32_t set_handler, std::uint32_t register_reset) {
  std::array<std::optional<std::uint32_t>, 32> value{};
  MemoryInitProof result;
  unsigned channels = 0, resets = 0;
  for (unsigned off = 0; off + 4 <= code.size(); off += 4) {
    const auto w = DeviceWord(code.data() + off);
    const unsigned op = w >> 26, rt = (w >> 21) & 31, ra = (w >> 16) & 31,
                   rb = (w >> 11) & 31;
    if (w == 0x4e800020u)
      return channels == 31 && resets == 1 ? std::optional(result) : std::nullopt;
    if ((w & 0xfc000003u) == 0x48000001u) {
      const auto target = pc + off + (std::uint32_t(std::int32_t(w << 6) >> 6) & ~3u);
      if (target == set_handler) {
        if (!value[3] || *value[3] >= 5 || !value[4] ||
            (channels & (1u << *value[3]))) return std::nullopt;
        if (result.handler && result.handler != *value[4]) return std::nullopt;
        result.handler = *value[4]; channels |= 1u << *value[3];
      }
      if (target == register_reset) {
        if (!value[3] || ++resets != 1) return std::nullopt;
        result.reset_info = *value[3];
      }
      for (unsigned i = 0; i < 13; ++i) value[i].reset();
    } else if (op == 15 || op == 14) {
      if (ra && !value[ra]) value[rt].reset();
      else value[rt] = (ra ? *value[ra] : 0u) +
          (op == 15 ? std::uint32_t(std::int16_t(w)) << 16 : std::uint32_t(std::int16_t(w)));
    } else if (op == 31 && ((w >> 1) & 1023) == 444 && rt == rb) {
      value[ra] = value[rt];
    } else if (op >= 32 && op <= 35) value[rt].reset();
  }
  return std::nullopt;
}
// A callback setter's code shape is shared by many SDKs. Prove that it
// returns the old callback, stores its argument, and that the exact SDA slot
// is read by the independently identified audio handler. Fail closed for
// absolute-address and unfamiliar compiler variants.
inline std::optional<unsigned>
ProveAiCallbackSetter(std::span<const std::uint8_t> code,
                      std::span<const std::uint8_t> handler) {
  constexpr unsigned Unknown = 0x10000;
  std::array<unsigned, 32> old;
  old.fill(Unknown);
  std::uint32_t input = 1u << 3;
  unsigned slot = Unknown, calls = 0;
  for (unsigned pc = 0; pc + 4 <= code.size(); pc += 4) {
    const auto w = DeviceWord(code.data() + pc);
    const auto op = w >> 26, rt = (w >> 21) & 31, ra = (w >> 16) & 31,
               rb = (w >> 11) & 31;
    if (w == 0x4e800020u) {
      if (calls != 2 || slot == Unknown || old[3] != slot)
        return std::nullopt;
      for (unsigned p = 0; p + 4 <= handler.size(); p += 4) {
        const auto load = DeviceWord(handler.data() + p);
        if ((load >> 26) == 32 && ((load >> 16) & 31) == 13 &&
            (load & 0xffff) == slot)
          return pc + 4;
      }
      return std::nullopt;
    }
    if ((w & 0xfc000003u) == 0x48000001u) {
      ++calls;
      input &= 0xffffc000u;
      for (unsigned r = 0; r < 14; ++r)
        old[r] = Unknown;
      continue;
    }
    if (op == 31 && ((w >> 1) & 1023) == 444 && rt == rb) {
      input = (input & ~(1u << ra)) | ((input & (1u << rt)) ? (1u << ra) : 0);
      old[ra] = old[rt];
      continue;
    }
    if (op == 32) {
      input &= ~(1u << rt);
      old[rt] = ra == 13 ? (w & 0xffff) : Unknown;
      continue;
    }
    if (op == 36 && ra == 13 && (input & (1u << rt))) {
      const auto target = w & 0xffff;
      bool loaded = false;
      for (auto source : old)
        loaded |= source == target;
      if (!loaded || slot != Unknown)
        return std::nullopt;
      slot = target;
      continue;
    }
    // Stack saves/restores and pointer-independent prologues are admitted;
    // unfamiliar writes to argument/old-value registers invalidate provenance.
    if (op == 14 || op == 15) {
      input &= ~(1u << rt);
      old[rt] = Unknown;
    }
  }
  return std::nullopt;
}
} // namespace GekkoAOTSdk
