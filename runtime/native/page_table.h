#pragma once
// SPDX-License-Identifier: GPL-3.0-or-later
#include "native/address_space.h"

namespace GekkoAOT::Native {

// Independent 32-bit PowerPC hashed-page-table walk. No host allocation or
// linear table scan: at most two groups of eight PTEs are examined.
// References: Gekko User Manual 5.4; PowerPC Programming Environments 7.5–7.6.
struct PageTranslation {
  enum class Status { Mapped, Missing, Protection, NoExecute, DirectStore, InvalidTable };
  Status status = Status::Missing;
  std::uint32_t physical = 0;
};
enum class PageAccess { Read, Write, Execute };

inline PageTranslation TranslatePage(AddressSpace& memory, std::uint32_t ea,
                                     std::uint32_t segment, std::uint32_t sdr1,
                                     bool user, PageAccess access) {
  using Status = PageTranslation::Status;
  if (segment & 0x80000000u) return {Status::DirectStore};
  if (access == PageAccess::Execute && (segment & 0x10000000u))
    return {Status::NoExecute};
  const std::uint32_t mask = sdr1 & 0x1ffu;
  const std::uint32_t base = sdr1 & 0xffff0000u;
  const std::uint32_t table_size = (mask + 1u) << 16u;
  if ((sdr1 & 0xfe00u) || (mask & (mask + 1u)) || (base & (table_size - 1u)) ||
      base > memory.Mem1().size() || table_size > memory.Mem1().size() - base)
    return {Status::InvalidTable};
  const std::uint32_t vsid = segment & 0x00ffffffu;
  const std::uint32_t hash = (vsid & 0x7ffffu) ^ ((ea >> 12u) & 0xffffu);
  const std::uint32_t hash_mask = (mask << 10u) | 0x3ffu;
  const bool key = (segment & (user ? 0x20000000u : 0x40000000u)) != 0;
  for (unsigned secondary = 0; secondary < 2; ++secondary) {
    const std::uint32_t group = base | (((secondary ? ~hash : hash) & hash_mask) << 6u);
    const std::uint32_t match = 0x80000000u | (vsid << 7u) |
                                (secondary << 6u) | ((ea >> 22u) & 0x3fu);
    for (unsigned slot = 0; slot < 8; ++slot) {
      const std::uint32_t pte = group + slot * 8u;
      std::uint32_t upper = 0, lower = 0;
      if (!memory.Read32(pte, &upper) || !memory.Read32(pte + 4u, &lower))
        return {Status::InvalidTable};
      if (upper != match) continue;
      if (access == PageAccess::Execute && (lower & 8u)) return {Status::NoExecute};
      const unsigned pp = lower & 3u;
      const bool readable = !key || pp != 0u;
      const bool writable = pp == 2u || (!key && pp != 3u);
      const bool permitted = readable && (access != PageAccess::Write || writable);
      // Gekko sets R even on page-protection faults, but never C on a fault.
      const std::uint32_t history = 0x100u |
          ((permitted && access == PageAccess::Write) ? 0x80u : 0u);
      if ((lower & history) != history) memory.Write32(pte + 4u, lower | history);
      if (!permitted) return {Status::Protection};
      return {Status::Mapped, (lower & 0xfffff000u) | (ea & 0xfffu)};
    }
  }
  return {Status::Missing};
}

} // namespace GekkoAOT::Native
