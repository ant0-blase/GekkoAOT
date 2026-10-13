// SPDX-License-Identifier: GPL-3.0-or-later
#include "native/guest_write_tracker.h"

#include <algorithm>
#include <cstdlib>
#include <string_view>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__linux__)
#include <fcntl.h>
#include <linux/fs.h>
#include <linux/userfaultfd.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace GekkoAOT::Native {

void* AllocateWatchedGuestRam(std::size_t size) {
#if defined(_WIN32)
  // Committed pages stay zero-filled on demand. Write watching only serves
  // the opt-in idle-loop prover (GEKKOAOT_IDLE_LOOP_PROOF=1); normal runs get
  // a plain allocation, on which the tracker reports itself unavailable.
  const char* proof = std::getenv("GEKKOAOT_IDLE_LOOP_PROOF");
  const bool watch = proof && std::string_view(proof) != "0" && !std::string_view(proof).empty();
  void* bytes = watch ? VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT | MEM_WRITE_WATCH,
                                     PAGE_READWRITE)
                      : nullptr;
  if (!bytes) bytes = VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
  return bytes;
#else
  (void)size;
  return nullptr;
#endif
}

void ReleaseWatchedGuestRam(void* bytes) {
#if defined(_WIN32)
  if (bytes) VirtualFree(bytes, 0, MEM_RELEASE);
#else
  (void)bytes;
#endif
}

GuestWriteTracker::~GuestWriteTracker() { Close(); }

const char* GuestWriteTracker::BackendName() const {
  switch (backend_) {
  case Backend::WriteWatch: return "win32-write-watch";
  case Backend::Uffd: return "uffd-wp-async+pagemap-scan";
  case Backend::SoftDirty: return "linux-soft-dirty";
  default: return "none";
  }
}

bool GuestWriteTracker::Enable(std::initializer_list<std::span<std::uint8_t>> ranges) {
  if (state_ != State::Untried) return Available();
  state_ = State::Failed;
  for (const auto range : ranges) {
    if (range.empty()) continue;
    const auto start = reinterpret_cast<std::uintptr_t>(range.data());
    if ((start & (kPageBytes - 1u)) != 0u || (range.size() & (kPageBytes - 1u)) != 0u) {
      ranges_.clear();
      return false;
    }
    ranges_.push_back({start, start + range.size()});
  }
  if (ranges_.empty()) return false;
#if defined(_WIN32)
  // MEM_WRITE_WATCH is an allocation attribute: ResetWriteWatch fails on a
  // buffer that was not allocated with it.
  for (const auto& range : ranges_) {
    if (ResetWriteWatch(reinterpret_cast<void*>(range.start), range.end - range.start) != 0u) {
      Close();
      return false;
    }
  }
  backend_ = Backend::WriteWatch;
  state_ = State::Ready;
  if (SelfTest()) return true;
  state_ = State::Failed;
  Close();
  return false;
#elif defined(__linux__)
  pagemap_ = open("/proc/self/pagemap", O_RDONLY | O_CLOEXEC);
  if (pagemap_ < 0) { Close(); return false; }
  // GEKKOAOT_WRITE_TRACKER=soft-dirty forces the older-kernel backend (tests).
  const char* forced = std::getenv("GEKKOAOT_WRITE_TRACKER");
  const bool force_soft_dirty = forced && std::string_view(forced) == "soft-dirty";
  if (!force_soft_dirty && EnableUffd()) {
    backend_ = Backend::Uffd;
    state_ = State::Ready;
    if (SelfTest()) return true;
    state_ = State::Failed;
    if (uffd_ >= 0) close(uffd_);
    uffd_ = -1;
  }
  clear_refs_ = open("/proc/self/clear_refs", O_WRONLY | O_CLOEXEC);
  if (clear_refs_ >= 0 && SoftDirtyWorks()) {
    backend_ = Backend::SoftDirty;
    state_ = State::Ready;
    if (SelfTest()) return true;
    state_ = State::Failed;
  }
  Close();
  return false;
#else
  Close();
  return false;
#endif
}

// Every backend must observe a real write before it is trusted: rewrite one
// byte of the first tracked page with its own value and expect that page to
// be reported. A backend that misses it is disabled, never trusted.
bool GuestWriteTracker::SelfTest() {
  if (!Reset()) return false;
  auto* probe = reinterpret_cast<volatile std::uint8_t*>(ranges_.front().start);
  const std::uint8_t value = *probe;
  *probe = value;
  std::vector<std::uintptr_t> pages;
  if (!Collect(&pages, 16u)) return false;
  return std::find(pages.begin(), pages.end(), ranges_.front().start) != pages.end();
}

bool GuestWriteTracker::Reset() {
  bool overflow = false;
  return Scan(nullptr, 0u, &overflow);
}

bool GuestWriteTracker::Collect(std::vector<std::uintptr_t>* pages, std::size_t max_pages) {
  bool overflow = false;
  return Scan(pages, max_pages, &overflow) && !overflow;
}

namespace {
void Note(std::uintptr_t page, std::vector<std::uintptr_t>* pages, std::size_t max_pages,
          bool* overflow) {
  if (!pages) return;
  if (pages->size() < max_pages) pages->push_back(page);
  else *overflow = true;
}
} // namespace

bool GuestWriteTracker::Scan(std::vector<std::uintptr_t>* pages, std::size_t max_pages,
                             bool* overflow) {
  if (!Available()) return false;
#if defined(_WIN32)
  for (const auto& range : ranges_) {
    void* base = reinterpret_cast<void*>(range.start);
    const SIZE_T size = range.end - range.start;
    for (;;) {
      ULONG_PTR count = watch_.size();
      ULONG granularity = 0;
      if (GetWriteWatch(WRITE_WATCH_FLAG_RESET, base, size, watch_.data(), &count,
                        &granularity) != 0u)
        return false;
      for (ULONG_PTR i = 0; i < count; ++i) {
        const auto first = reinterpret_cast<std::uintptr_t>(watch_[i]);
        for (std::uintptr_t page = first; page < first + granularity; page += kPageBytes)
          Note(page, pages, max_pages, overflow);
      }
      // A full buffer may leave written pages unreported (and not reset);
      // keep draining until the list comes back short.
      if (count < watch_.size()) break;
    }
  }
  return true;
#elif defined(__linux__)
  if (backend_ == Backend::Uffd) return ScanUffd(pages, max_pages, overflow);
  return ScanSoftDirty(pages, max_pages, overflow);
#else
  (void)pages;
  (void)max_pages;
  (void)overflow;
  return false;
#endif
}

#if defined(__linux__)
bool GuestWriteTracker::EnableUffd() {
#if defined(PAGEMAP_SCAN) && defined(UFFD_FEATURE_WP_ASYNC) && defined(UFFD_USER_MODE_ONLY)
  uffd_ = static_cast<int>(
      syscall(SYS_userfaultfd, O_CLOEXEC | O_NONBLOCK | UFFD_USER_MODE_ONLY));
  if (uffd_ < 0) return false;
  uffdio_api api{};
  api.api = UFFD_API;
  api.features = UFFD_FEATURE_WP_ASYNC | UFFD_FEATURE_WP_UNPOPULATED;
  bool ok = ioctl(uffd_, UFFDIO_API, &api) == 0;
  for (const auto& range : ranges_) {
    if (!ok) break;
    uffdio_register reg{};
    reg.range.start = range.start;
    reg.range.len = range.end - range.start;
    reg.mode = UFFDIO_REGISTER_MODE_WP;
    ok = ioctl(uffd_, UFFDIO_REGISTER, &reg) == 0;
  }
  if (!ok) {
    close(uffd_);  // closing the descriptor also unregisters every range
    uffd_ = -1;
  }
  return ok;
#else
  return false;
#endif
}

bool GuestWriteTracker::ScanUffd(std::vector<std::uintptr_t>* pages, std::size_t max_pages,
                                 bool* overflow) {
#if defined(PAGEMAP_SCAN) && defined(UFFD_FEATURE_WP_ASYNC)
  page_region regions[64];
  for (const auto& range : ranges_) {
    std::uintptr_t cursor = range.start;
    while (cursor < range.end) {
      pm_scan_arg arg{};
      arg.size = sizeof(arg);
      arg.flags = PM_SCAN_WP_MATCHING | PM_SCAN_CHECK_WPASYNC;
      arg.start = cursor;
      arg.end = range.end;
      arg.vec = reinterpret_cast<std::uintptr_t>(regions);
      arg.vec_len = 64;
      arg.category_mask = PAGE_IS_WRITTEN;
      arg.return_mask = PAGE_IS_WRITTEN;
      const long count = ioctl(pagemap_, PAGEMAP_SCAN, &arg);
      if (count < 0) return false;
      for (long i = 0; i < count; ++i)
        for (std::uintptr_t page = regions[i].start; page < regions[i].end; page += kPageBytes)
          Note(page, pages, max_pages, overflow);
      // The kernel stops when the region vector is full; keep walking so
      // every written page is re-protected for the next window.
      if (arg.walk_end <= cursor) return false;
      cursor = static_cast<std::uintptr_t>(arg.walk_end);
    }
  }
  return true;
#else
  (void)pages;
  (void)max_pages;
  (void)overflow;
  return false;
#endif
}

bool GuestWriteTracker::ScanSoftDirty(std::vector<std::uintptr_t>* pages,
                                      std::size_t max_pages, bool* overflow) {
  constexpr std::uint64_t kSoftDirty = 1ull << 55;
  std::uint64_t entries[512];
  if (pages) {
    for (const auto& range : ranges_) {
      for (std::uintptr_t page = range.start; page < range.end;) {
        const std::size_t want =
            std::min<std::size_t>(512u, (range.end - page) / kPageBytes);
        const auto bytes = static_cast<ssize_t>(want * sizeof(std::uint64_t));
        if (pread(pagemap_, entries, static_cast<std::size_t>(bytes),
                  static_cast<off_t>((page / kPageBytes) * sizeof(std::uint64_t))) != bytes)
          return false;
        for (std::size_t i = 0; i < want; ++i)
          if (entries[i] & kSoftDirty) Note(page + i * kPageBytes, pages, max_pages, overflow);
        page += want * kPageBytes;
      }
    }
  }
  return write(clear_refs_, "4", 1) == 1;
}

bool GuestWriteTracker::SoftDirtyWorks() {
  // Kernels built without CONFIG_MEM_SOFT_DIRTY never set the bit. Verify on
  // the first tracked page (rewriting its own value) before trusting it.
  auto* probe = reinterpret_cast<volatile std::uint8_t*>(ranges_.front().start);
  if (write(clear_refs_, "4", 1) != 1) return false;
  const std::uint8_t value = *probe;
  *probe = value;
  std::uint64_t entry = 0;
  if (pread(pagemap_, &entry, sizeof(entry),
            static_cast<off_t>((ranges_.front().start / kPageBytes) * sizeof(entry))) !=
      static_cast<ssize_t>(sizeof(entry)))
    return false;
  return (entry & (1ull << 55)) != 0u;
}
#else
bool GuestWriteTracker::EnableUffd() { return false; }
bool GuestWriteTracker::ScanUffd(std::vector<std::uintptr_t>*, std::size_t, bool*) {
  return false;
}
bool GuestWriteTracker::ScanSoftDirty(std::vector<std::uintptr_t>*, std::size_t, bool*) {
  return false;
}
bool GuestWriteTracker::SoftDirtyWorks() { return false; }
#endif

void GuestWriteTracker::Close() {
#if defined(__linux__)
  if (pagemap_ >= 0) close(pagemap_);
  if (clear_refs_ >= 0) close(clear_refs_);
  if (uffd_ >= 0) close(uffd_);
#endif
  pagemap_ = -1;
  clear_refs_ = -1;
  uffd_ = -1;
  ranges_.clear();
  backend_ = Backend::None;
}

} // namespace GekkoAOT::Native
