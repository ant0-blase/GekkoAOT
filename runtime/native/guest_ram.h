// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <new>
#include <vector>
#if defined(_WIN32)
#include "native/guest_write_tracker.h"
#elif defined(__unix__) || defined(__APPLE__)
#include <sys/mman.h>
#endif

namespace GekkoAOT::Native {
// Stable, contiguous guest RAM. Anonymous mappings supply zero-filled pages
// on demand instead of committing every compatibility aperture at startup.
class GuestRam {
public:
  explicit GuestRam(std::size_t size, std::uint8_t* shared_view = nullptr)
      : size_(size), bytes_(shared_view), owns_(shared_view == nullptr) {
    if (shared_view) return;
#if defined(_WIN32)
    // v274: with the opt-in idle-loop prover, MEM_WRITE_WATCH lets
    // GuestWriteTracker prove that an idle guest loop left RAM unchanged.
    // Committed pages are still zero-filled lazily.
    if (size == 0u) return;
    void* mapping = AllocateWatchedGuestRam(size);
    if (!mapping) throw std::bad_alloc();
    bytes_ = static_cast<std::uint8_t*>(mapping);
#elif defined(__unix__) || defined(__APPLE__)
    if (size == 0u) return;
    void* mapping = mmap(nullptr, size, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mapping == MAP_FAILED) throw std::bad_alloc();
    bytes_ = static_cast<std::uint8_t*>(mapping);
#else
    backing_.resize(size);
    bytes_ = backing_.data();
#endif
  }
  ~GuestRam() {
    if (!owns_) return;
#if defined(_WIN32)
    ReleaseWatchedGuestRam(bytes_);
#elif defined(__unix__) || defined(__APPLE__)
    if (bytes_) munmap(bytes_, size_);
#endif
  }
  GuestRam(const GuestRam&) = delete;
  GuestRam& operator=(const GuestRam&) = delete;
  std::uint8_t* data() { return bytes_; }
  const std::uint8_t* data() const { return bytes_; }
  std::size_t size() const { return size_; }
  bool empty() const { return size_ == 0u; }
  std::uint8_t* begin() { return bytes_; }
  const std::uint8_t* begin() const { return bytes_; }
  std::uint8_t* end() { return size_ ? bytes_ + size_ : bytes_; }
  const std::uint8_t* end() const { return size_ ? bytes_ + size_ : bytes_; }
  std::uint8_t& operator[](std::size_t offset) { return bytes_[offset]; }
  const std::uint8_t& operator[](std::size_t offset) const { return bytes_[offset]; }
  void Clear() {
    if (size_ == 0u) return;
#if defined(__linux__)
    // Linux private anonymous MADV_DONTNEED guarantees zeroes on subsequent
    // reads and keeps the virtual address (including cached CPU pointers).
    if (owns_ && madvise(bytes_, size_, MADV_DONTNEED) == 0) return;
#endif
    std::fill(begin(), end(), 0u);
  }
private:
  std::size_t size_ = 0;
  std::uint8_t* bytes_ = nullptr;
  bool owns_ = true;
#if !defined(_WIN32) && !defined(__unix__) && !defined(__APPLE__)
  std::vector<std::uint8_t> backing_;
#endif
};
} // namespace GekkoAOT::Native
