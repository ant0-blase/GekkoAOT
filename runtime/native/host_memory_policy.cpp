// SPDX-License-Identifier: GPL-3.0-or-later
#include "native/address_space.h"
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#elif defined(__unix__) || defined(__APPLE__)
#include <sys/mman.h>
#endif
namespace GekkoAOT::Native {
namespace {
bool ReadWrite(void* base, std::size_t bytes) {
  if (!bytes) return true;
#if defined(_WIN32)
  DWORD previous = 0;
  return VirtualProtect(base, bytes, PAGE_READWRITE, &previous) != 0;
#elif defined(__unix__) || defined(__APPLE__)
  return mprotect(base, bytes, PROT_READ | PROT_WRITE) == 0;
#else
  (void)base; return false;
#endif
}
}
bool AddressSpace::GrantRamReadWrite() {
  if (!ReadWrite(mem1_.data(), mem1_.size()) ||
      !ReadWrite(mem2_.data(), mem2_.size()) ||
      !ReadWrite(fake_vmem_.data(), fake_vmem_.size())) return false;
  if (auto* base = window_.Base())
    for (const auto alias : {0x80000000u, 0xc0000000u})
      if (!ReadWrite(base + alias, mem1_.size()) ||
          !ReadWrite(base + alias + Mem2PhysicalBase, mem2_.size())) return false;
  return true;
}
} // namespace GekkoAOT::Native
