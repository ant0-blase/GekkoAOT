// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// GEKKOAOT_FPSCR_STATUS_ELISION_V380: decide whether FPSCR status bits
// (FPRF/FPCC, FI/FR, XX/FX/VX*/FEX) can ever be interpreted by guest code.
//
// Only reads matter: writes (mtfsf/mtfsfi/mtfsb*) replace bits but never
// expose the status the elided arithmetic stopped computing. Status is read
// by mcrfs, record-form FP arithmetic (CR1 copies FX/FEX/VX/OX) and mffs.
// Two mffs uses are verbatim and admitted:
//   * OS FPU-context save: mffs fN; ...; stfd fN,off(rX) with rX != r1
//     (__OSSaveFPUContext, before or after the other FPR stores);
//   * PPCMffs-style getters (mffs fN; stfd fN,off(r1); lwz r3,...) only when
//     every caller merely masks/sets bits of r3 and passes it straight back
//     to a function executing mtfsf (rounding-mode read-modify-write), and no
//     data word or REL relocation can reach the getter.
// Scanning raw words can only reject more code, never admit an unproven use.
#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace GekkoAOT::Build {

struct FpscrStatusProof {
  bool unobservable = true;
  std::uint32_t mffs_saves = 0, mffs_getters = 0, getter_callers = 0;
  std::string reason;
};

struct GuestCodeSection {
  std::uint32_t address = 0;
  std::vector<std::uint32_t> words;
};

namespace detail {

inline bool IsMffs(std::uint32_t w) { return (w >> 26u) == 63u && ((w >> 1u) & 0x3ffu) == 583u; }
inline bool IsMtfsf(std::uint32_t w) { return (w >> 26u) == 63u && ((w >> 1u) & 0x3ffu) == 711u; }
inline bool IsBranch(std::uint32_t w) {
  const std::uint32_t op = w >> 26u, xo = (w >> 1u) & 0x3ffu;
  return op == 16u || op == 18u || (op == 19u && (xo == 16u || xo == 528u || xo == 50u)) || op == 17u;
}
// Conservative: any FP-register field of an FP-touching instruction that names fr.
inline bool TouchesFpr(std::uint32_t w, std::uint32_t fr) {
  const std::uint32_t op = w >> 26u;
  const bool fp = op == 4u || op == 59u || op == 63u || (op >= 48u && op <= 57u) || op == 60u || op == 61u ||
                  (op == 31u && (((w >> 1u) & 0x3ffu) == 535u || ((w >> 1u) & 0x3ffu) == 567u ||
                                 ((w >> 1u) & 0x3ffu) == 599u || ((w >> 1u) & 0x3ffu) == 631u ||
                                 ((w >> 1u) & 0x3ffu) == 663u || ((w >> 1u) & 0x3ffu) == 695u ||
                                 ((w >> 1u) & 0x3ffu) == 727u || ((w >> 1u) & 0x3ffu) == 759u ||
                                 ((w >> 1u) & 0x3ffu) == 983u));
  if (!fp) return false;
  if (op >= 48u && op <= 57u) return ((w >> 21u) & 31u) == fr;  // FP load/store/psq: only frS/frD
  return ((w >> 21u) & 31u) == fr || ((w >> 16u) & 31u) == fr || ((w >> 11u) & 31u) == fr ||
         ((w >> 6u) & 31u) == fr;
}
// Only r3 bit manipulation: rlwinm/ori/oris/andi./andis./and/or/andc with rS=rA=r3.
inline bool IsR3BitOp(std::uint32_t w) {
  const std::uint32_t op = w >> 26u, rs = (w >> 21u) & 31u, ra = (w >> 16u) & 31u;
  if (op == 21u || op == 24u || op == 25u || op == 28u || op == 29u) return rs == 3u && ra == 3u;
  if (op == 31u) {
    const std::uint32_t xo = (w >> 1u) & 0x3ffu;
    return (xo == 28u || xo == 444u || xo == 60u) && rs == 3u && ra == 3u;
  }
  return false;
}
// Conservative: does any register field name r3 (or f3)?
inline bool MentionsR3(std::uint32_t w) {
  return ((w >> 21u) & 31u) == 3u || ((w >> 16u) & 31u) == 3u || ((w >> 11u) & 31u) == 3u;
}

}  // namespace detail

// Whole-program proof. `sections` are executable guest code (DOL text + REL
// exec sections); `data_words` are all non-code words that could hold a code
// pointer; `rel_targets` are absolute DOL addresses named by REL relocations.
inline FpscrStatusProof ProveFpscrStatus(const std::vector<GuestCodeSection>& sections,
                                         std::span<const std::uint32_t> data_words,
                                         std::span<const std::uint32_t> rel_targets) {
  using namespace detail;
  FpscrStatusProof proof;
  const auto reject = [&](const char* why, std::uint32_t address) {
    if (!proof.unobservable) return;
    proof.unobservable = false;
    char text[16];
    std::snprintf(text, sizeof(text), "%08x", address);
    proof.reason = std::string(why) + "@" + text;
  };
  const auto word_at = [&](std::uint32_t address, std::uint32_t* out) {
    for (const auto& s : sections)
      if (address >= s.address && address - s.address < s.words.size() * 4u && !((address - s.address) & 3u)) {
        *out = s.words[(address - s.address) / 4u];
        return true;
      }
    return false;
  };
  std::vector<std::uint32_t> getters;
  for (const auto& s : sections) {
    const auto& w = s.words;
    for (std::size_t i = 0; i < w.size() && proof.unobservable; ++i) {
      const std::uint32_t x = w[i], op = x >> 26u, xo = (x >> 1u) & 0x3ffu;
      const std::uint32_t pc = s.address + std::uint32_t(i) * 4u;
      if (op == 63u && xo == 64u) { reject("mcrfs", pc); break; }
      const bool record = (x & 1u) != 0u;
      if (op == 59u && record) { reject("fp-record-form", pc); break; }
      if (op == 63u && record && xo != 583u) { reject("fp-record-form", pc); break; }
      if (op == 4u && record) {
        const std::uint32_t xo5 = (x >> 1u) & 31u;
        if (!(xo5 == 6u || xo5 == 7u || xo == 38u || xo == 39u || xo == 1014u)) { reject("ps-record-form", pc); break; }
      }
      if (!IsMffs(x)) continue;
      if (record) { reject("mffs-record", pc); break; }
      const std::uint32_t fr = (x >> 21u) & 31u;
      bool settled = false;
      for (std::size_t j = i + 1; j < w.size() && j <= i + 48u && !settled; ++j) {
        const std::uint32_t y = w[j];
        if (IsBranch(y)) break;
        if ((y >> 26u) == 54u && ((y >> 21u) & 31u) == fr) {  // stfd fr,d(rA)
          const std::uint32_t ra = (y >> 16u) & 31u;
          if (ra != 1u) { ++proof.mffs_saves; settled = true; break; }
          // Getter: find the function start (after the previous blr).
          std::size_t start = i;
          while (start > 0 && w[start - 1u] != 0x4e800020u) --start;
          getters.push_back(s.address + std::uint32_t(start) * 4u);
          ++proof.mffs_getters;
          settled = true;
          break;
        }
        if (TouchesFpr(y, fr)) break;
      }
      if (!settled) { reject("mffs-used", pc); break; }
    }
  }
  if (!proof.unobservable || getters.empty()) return proof;
  std::sort(getters.begin(), getters.end());
  getters.erase(std::unique(getters.begin(), getters.end()), getters.end());
  const auto is_getter = [&](std::uint32_t a) { return std::binary_search(getters.begin(), getters.end(), a); };
  for (const auto d : data_words)
    if (is_getter(d)) { reject("mffs-getter-pointer", d); return proof; }
  for (const auto t : rel_targets)
    if (is_getter(t)) { reject("mffs-getter-rel-call", t); return proof; }
  // Every direct call must feed r3 (bit ops only) into a function with mtfsf.
  for (const auto& s : sections) {
    const auto& w = s.words;
    for (std::size_t i = 0; i < w.size(); ++i) {
      const std::uint32_t x = w[i];
      if ((x >> 26u) != 18u || (x & 2u)) continue;  // relative b/bl only
      std::int32_t off = std::int32_t(x & 0x03fffffcu);
      if (off & 0x02000000) off -= 0x04000000;
      const std::uint32_t pc = s.address + std::uint32_t(i) * 4u;
      if (!is_getter(pc + std::uint32_t(off))) continue;
      if (!(x & 1u)) { reject("mffs-getter-tailcall", pc); return proof; }
      ++proof.getter_callers;
      // Every path from the call (following branches, bounded) must reach a
      // call to a function executing mtfsf with r3 only bit-manipulated.
      const auto setter = [&](std::uint32_t target) {
        for (std::uint32_t k = 0; k < 16u; ++k) {
          std::uint32_t body = 0;
          if (!word_at(target + k * 4u, &body)) return false;
          if (IsMtfsf(body)) return true;
          if (body == 0x4e800020u) return false;
        }
        return false;
      };
      struct Path { std::uint32_t pc; std::uint32_t budget; };
      std::vector<Path> work{{pc + 4u, 160u}};
      std::vector<std::uint32_t> visited;
      bool restored = true;
      std::uint32_t explored = 0;
      while (!work.empty() && restored) {
        Path path = work.back();
        work.pop_back();
        if (++explored > 64u) { restored = false; break; }
        bool done = false;
        while (!done) {
          // A pc already proven on another path needs no second walk (loops).
          if (std::find(visited.begin(), visited.end(), path.pc) != visited.end()) break;
          visited.push_back(path.pc);
          std::uint32_t y = 0;
          if (!path.budget-- || !word_at(path.pc, &y)) { restored = false; break; }
          const std::uint32_t op = y >> 26u;
          if (op == 18u) {
            std::int32_t o = std::int32_t(y & 0x03fffffcu);
            if (o & 0x02000000) o -= 0x04000000;
            const std::uint32_t target = (y & 2u) ? std::uint32_t(o) : path.pc + std::uint32_t(o);
            if (y & 1u) { if (!setter(target)) restored = false; done = true; }  // bl: must be the setter
            else path.pc = target;                                               // b: follow
            continue;
          }
          if (op == 16u) {  // bc: explore both edges (condition never reads r3 here)
            if (MentionsR3(y) && ((y >> 16u) & 31u) == 3u) { restored = false; break; }
            std::int32_t o = std::int32_t(y & 0xfffcu);
            if (o & 0x8000) o -= 0x10000;
            work.push_back({(y & 2u) ? std::uint32_t(o) : path.pc + std::uint32_t(o), path.budget});
            path.pc += 4u;
            continue;
          }
          if (IsBranch(y)) { restored = false; break; }  // blr/bctr/rfi: r3 escapes
          if (IsR3BitOp(y)) { path.pc += 4u; continue; }
          if (MentionsR3(y)) { restored = false; break; }  // r3 read, overwritten or reused
          path.pc += 4u;
        }
      }
      if (!restored) { reject("mffs-getter-interpreted", pc); return proof; }
    }
  }
  return proof;
}

// Compatibility wrapper for word-only scans (unit tests): one section at 0.
inline void ScanFpscrStatusReaders(std::span<const std::uint32_t> words, FpscrStatusProof& proof) {
  if (!proof.unobservable) return;
  std::vector<GuestCodeSection> sections{{0x80000000u, {words.begin(), words.end()}}};
  const auto result = ProveFpscrStatus(sections, {}, {});
  proof.unobservable = result.unobservable;
  proof.mffs_saves += result.mffs_saves;
  proof.mffs_getters += result.mffs_getters;
  proof.getter_callers += result.getter_callers;
  if (!result.unobservable) proof.reason = result.reason;
}

// GEKKOAOT_NO_RESERVATIONS_V381: lwarx is the only instruction that sets a
// reservation. Without one anywhere in the guest code, stores never have a
// reservation to clear and stwcx. always fails exactly as it would anyway.
struct ReservationProof {
  bool absent = true;
  std::uint32_t lwarx = 0, stwcx = 0;
};

inline void ScanReservationUsers(std::span<const std::uint32_t> words, ReservationProof& proof) {
  for (const std::uint32_t w : words) {
    if ((w >> 26u) != 31u) continue;
    const std::uint32_t xo = (w >> 1u) & 0x3ffu;
    if (xo == 20u) { ++proof.lwarx; proof.absent = false; }
    else if (xo == 150u) ++proof.stwcx;
  }
}

}  // namespace GekkoAOT::Build
