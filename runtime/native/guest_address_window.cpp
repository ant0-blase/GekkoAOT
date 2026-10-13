// SPDX-License-Identifier: GPL-3.0-or-later
#include "native/guest_address_window.h"
#include <cstdlib>
#include <cstring>
#include <string_view>
#if defined(__linux__)
#include <fcntl.h>
#include <linux/falloc.h>
#include <linux/memfd.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace GekkoAOT::Native {
GuestAddressWindow::GuestAddressWindow(std::size_t mem1, std::size_t mem2,
                                     std::size_t vmem) {
#if defined(__linux__) && defined(SYS_memfd_create)
  if constexpr (sizeof(void*) < 8) return;
  const char* mode = std::getenv("GEKKOAOT_NATIVE_ADDRESS_WINDOW");
  if (mode && std::string_view(mode) == "0") return;
  // Keep unusual/custom test layouts on the existing independent allocations.
  // These bounds also prevent overlapping fixed views and arithmetic wrapping.
  const long page = sysconf(_SC_PAGESIZE);
  if (page <= 0 || !mem1 || mem1 > 0x10000000u || mem2 > 0x30000000u ||
      vmem > 0x02000000u || mem1 % page || mem2 % page || vmem % page) return;
  backing_size_ = mem1 + mem2 + vmem;
  const int fd = static_cast<int>(syscall(SYS_memfd_create, "GekkoAOT guest RAM", MFD_CLOEXEC));
  if (fd < 0) return;
  if (ftruncate(fd, static_cast<off_t>(backing_size_)) != 0) { close(fd); return; }
  void* reservation = mmap(nullptr, Size, PROT_NONE,
                           MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
  if (reservation == MAP_FAILED) { close(fd); return; }
  auto* base = static_cast<std::uint8_t*>(reservation);
  const auto map = [&](std::uint32_t guest, std::size_t size, std::size_t offset) {
    return !size || mmap(base + guest, size, PROT_READ | PROT_WRITE,
                         MAP_SHARED | MAP_FIXED, fd, static_cast<off_t>(offset)) == base + guest;
  };
  // MAP_FIXED replaces only portions of this process-owned reservation.
  if (!map(0x00000000u, mem1, 0) || !map(0x80000000u, mem1, 0) ||
      !map(0xc0000000u, mem1, 0) || !map(0x10000000u, mem2, mem1) ||
      !map(0x90000000u, mem2, mem1) || !map(0xd0000000u, mem2, mem1) ||
      !map(0x7e000000u, vmem, mem1 + mem2)) {
    munmap(base, Size); close(fd); return;
  }
  base_ = base;
  backing_ = fd;
#else
  (void)mem1; (void)mem2; (void)vmem;
#endif
}

GuestAddressWindow::~GuestAddressWindow() {
#if defined(__linux__)
  if (base_) munmap(base_, Size);
  if (backing_ >= 0) close(backing_);
#endif
}

bool GuestAddressWindow::Clear() {
#if defined(__linux__)
  // Anonymous shared memfd pages are discarded once for all RAM aliases.
  return backing_ >= 0 && fallocate(backing_, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE,
                                    0, static_cast<off_t>(backing_size_)) == 0;
#else
  return false;
#endif
}
} // namespace GekkoAOT::Native
