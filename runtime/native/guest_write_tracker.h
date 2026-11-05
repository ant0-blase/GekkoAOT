// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <utility>
#include <vector>

namespace GekkoAOT::Native {

// Exact page-granular write tracking for guest RAM buffers, used by the
// idle-loop prover to show that one loop iteration left guest memory
// byte-identical. Backends (guest_memory.cpp), in order of preference:
//
//  * Windows: the buffers are allocated with MEM_WRITE_WATCH (GuestRam), and
//    GetWriteWatch(WRITE_WATCH_FLAG_RESET) returns and clears written pages.
//  * Linux >= 6.7: userfaultfd write-protect in WP_ASYNC mode (the kernel
//    resolves write faults itself, so no signal handler is involved and
//    syscalls such as read() into guest RAM keep working) plus PAGEMAP_SCAN,
//    which reports written pages and re-protects exactly those.
//  * Older Linux: the kernel soft-dirty bits (/proc/self/clear_refs and
//    /proc/self/pagemap bit 55). Slower (the clear is process-wide) but exact.
//
// Every backend reports a superset of the pages written since the previous
// window. A host with no backend reports Available() == false and the prover
// never parks, which is exactly the pre-v274 behaviour.
class GuestWriteTracker {
public:
  static constexpr std::uintptr_t kPageBytes = 4096u;

  GuestWriteTracker() = default;
  GuestWriteTracker(const GuestWriteTracker&) = delete;
  GuestWriteTracker& operator=(const GuestWriteTracker&) = delete;
  ~GuestWriteTracker();

  // Registers the buffers once; later calls return the cached result.
  bool Enable(std::initializer_list<std::span<std::uint8_t>> ranges);
  bool Available() const { return state_ == State::Ready; }
  const char* BackendName() const;

  // Forgets earlier writes; subsequent writes are reported by Collect().
  bool Reset();

  // Appends every page written since the previous Reset()/Collect() to
  // `pages` and starts a new tracking window. Returns false on an OS error
  // or when more than `max_pages` pages were written.
  bool Collect(std::vector<std::uintptr_t>* pages, std::size_t max_pages);

  // v389 selective mode (Linux userfaultfd only): `range` is registered but
  // nothing is protected until Protect() arms page-aligned [start, end).
  // WrittenRuns() appends the runs of [start, end) that are not protected:
  // armed pages written since they were armed, and pages never armed (the
  // caller keeps track of which pages it armed). Does not re-protect.
  bool EnableSelective(std::span<std::uint8_t> range);
  bool Protect(std::uintptr_t start, std::uintptr_t end);
  bool WrittenRuns(std::uintptr_t start, std::uintptr_t end,
                   std::vector<std::pair<std::uintptr_t, std::uintptr_t>>* runs);

private:
  enum class State : std::uint8_t { Untried, Ready, Failed };
  enum class Backend : std::uint8_t { None, WriteWatch, Uffd, SoftDirty };
  struct Range { std::uintptr_t start, end; };

  bool Scan(std::vector<std::uintptr_t>* pages, std::size_t max_pages, bool* overflow);
  bool EnableUffd();
  bool ScanUffd(std::vector<std::uintptr_t>* pages, std::size_t max_pages, bool* overflow);
  bool ScanSoftDirty(std::vector<std::uintptr_t>* pages, std::size_t max_pages, bool* overflow);
  bool SoftDirtyWorks();
  bool SelfTest();
  void Close();

  State state_ = State::Untried;
  Backend backend_ = Backend::None;
  std::vector<Range> ranges_;
  int uffd_ = -1;
  int pagemap_ = -1;
  int clear_refs_ = -1;
  std::array<void*, 512> watch_{};
};

// Windows allocation hooks for GuestRam (MEM_WRITE_WATCH); see guest_memory.cpp.
void* AllocateWatchedGuestRam(std::size_t size);
void ReleaseWatchedGuestRam(void* bytes);

} // namespace GekkoAOT::Native
