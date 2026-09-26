#pragma once
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace GekkoAOT::Native {

// Host-native virtual filesystem for the GameCube data partition.
//
// The FST remains the source of truth for disc layout/entry numbers, while file
// payloads may come from (highest priority first):
//   1. a host overlay directory (mods/replacements),
//   2. an extracted host directory,
//   3. the mounted nod data partition.
//
// Raw DI reads that don't resolve to one complete FST file remain owned by the
// existing disc-image backend, preserving compatibility with direct hardware
// access and non-filesystem disc structures.
class NativeVfs final {
public:
  struct Entry {
    std::uint32_t index = 0;
    std::uint64_t disc_offset = 0;
    std::uint32_t size = 0;
    std::string path;
  };

  NativeVfs() = default;
  ~NativeVfs();
  NativeVfs(const NativeVfs&) = delete;
  NativeVfs& operator=(const NativeVfs&) = delete;

  bool Mount(void* nod_disc_handle,
             const std::filesystem::path& extracted_root = {},
             const std::filesystem::path& overlay_root = {});
  void Reset();

  bool Ready() const { return partition_ != nullptr && !entries_by_offset_.empty(); }
  std::size_t EntryCount() const { return entries_by_offset_.size(); }
  const std::filesystem::path& ExtractedRoot() const { return extracted_root_; }
  const std::filesystem::path& OverlayRoot() const { return overlay_root_; }

  std::optional<Entry> FindPath(std::string_view path) const;

  // Read an absolute logical-disc range through the VFS when that range is
  // wholly contained in a single FST file. Returns false for boot/FST/padding,
  // cross-file reads, or when the VFS cannot satisfy the request; callers then
  // fall back to the raw DI/disc-image provider.
  bool ReadDiscRange(std::uint64_t disc_offset, void* destination,
                     std::uint32_t size, const char** source = nullptr);

private:
  const Entry* FindCoveringEntry(std::uint64_t offset, std::uint32_t size) const;
  bool ReadHostFile(const std::filesystem::path& root, const Entry& entry,
                    std::uint64_t file_offset, void* destination,
                    std::uint32_t size) const;
  bool ReadPartitionFile(const Entry& entry, std::uint64_t file_offset,
                         void* destination, std::uint32_t size) const;
  static std::string NormalizeLookup(std::string_view path);

  void* partition_ = nullptr;
  std::filesystem::path extracted_root_;
  std::filesystem::path overlay_root_;
  std::vector<Entry> entries_by_offset_;
  std::unordered_map<std::string, std::size_t> path_to_entry_;
};

} // namespace GekkoAOT::Native
