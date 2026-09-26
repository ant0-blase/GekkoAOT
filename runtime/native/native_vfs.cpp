// SPDX-License-Identifier: GPL-3.0-or-later
#include "native/native_vfs.h"

#ifdef GEKKOAOT_HAVE_NOD
#include <nod.h>
#endif

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <system_error>

namespace GekkoAOT::Native {
namespace {

std::uint32_t ReadBE32(const std::uint8_t* p) {
  return (std::uint32_t(p[0]) << 24u) | (std::uint32_t(p[1]) << 16u) |
         (std::uint32_t(p[2]) << 8u) | std::uint32_t(p[3]);
}

bool SafeFstName(std::string_view name) {
  return !name.empty() && name != "." && name != ".." &&
         name.find('/') == std::string_view::npos &&
         name.find('\\') == std::string_view::npos;
}

std::filesystem::path EnvPath(const char* name) {
  const char* value = std::getenv(name);
  if (!value || !*value) return {};
  return std::filesystem::path(value);
}

} // namespace

NativeVfs::~NativeVfs() { Reset(); }

std::string NativeVfs::NormalizeLookup(std::string_view path) {
  while (!path.empty() && (path.front() == '/' || path.front() == '\\'))
    path.remove_prefix(1);
  std::string out;
  out.reserve(path.size());
  bool slash = false;
  for (unsigned char ch : path) {
    if (ch == '/' || ch == '\\') {
      if (!slash && !out.empty()) out.push_back('/');
      slash = true;
      continue;
    }
    slash = false;
    out.push_back(static_cast<char>(std::tolower(ch)));
  }
  while (!out.empty() && out.back() == '/') out.pop_back();
  return out;
}

void NativeVfs::Reset() {
#ifdef GEKKOAOT_HAVE_NOD
  if (partition_) nod_free(static_cast<NodHandle*>(partition_));
#endif
  partition_ = nullptr;
  extracted_root_.clear();
  overlay_root_.clear();
  entries_by_offset_.clear();
  path_to_entry_.clear();
}

bool NativeVfs::Mount(void* nod_disc_handle,
                      const std::filesystem::path& extracted_root,
                      const std::filesystem::path& overlay_root) {
  Reset();
#ifndef GEKKOAOT_HAVE_NOD
  (void)nod_disc_handle;
  (void)extracted_root;
  (void)overlay_root;
  return false;
#else
  auto* disc = static_cast<NodHandle*>(nod_disc_handle);
  if (!disc) return false;

  NodPartitionOptions options{};
  NodHandle* partition = nullptr;
  if (nod_disc_open_partition_kind(disc, NOD_PARTITION_KIND_DATA, &options, &partition) !=
          NOD_RESULT_OK ||
      !partition)
    return false;

  NodPartitionMeta meta{};
  if (nod_partition_meta(partition, &meta) != NOD_RESULT_OK ||
      !meta.raw_fst.data || meta.raw_fst.size < 12u) {
    nod_free(partition);
    return false;
  }

  const auto* fst = static_cast<const std::uint8_t*>(meta.raw_fst.data);
  const std::size_t fst_size = meta.raw_fst.size;
  const std::uint32_t count = ReadBE32(fst + 8u);
  const std::uint64_t nodes_bytes = std::uint64_t(count) * 12u;
  if (count == 0u || nodes_bytes > fst_size) {
    nod_free(partition);
    return false;
  }
  const char* strings = reinterpret_cast<const char*>(fst + nodes_bytes);
  const std::size_t strings_size = fst_size - static_cast<std::size_t>(nodes_bytes);

  struct DirFrame {
    std::uint32_t end = 0;
    std::string path;
  };
  std::vector<DirFrame> dirs;
  dirs.push_back({count, {}});

  std::vector<Entry> entries;
  entries.reserve(count > 0u ? count - 1u : 0u);
  for (std::uint32_t i = 1; i < count; ++i) {
    while (!dirs.empty() && i >= dirs.back().end) dirs.pop_back();
    if (dirs.empty()) {
      nod_free(partition);
      return false;
    }

    const auto* node = fst + std::size_t(i) * 12u;
    const std::uint32_t type_name = ReadBE32(node + 0u);
    const bool is_dir = (type_name & 0xff000000u) != 0u;
    const std::uint32_t name_offset = type_name & 0x00ffffffu;
    if (name_offset >= strings_size) {
      nod_free(partition);
      return false;
    }
    const char* raw_name = strings + name_offset;
    const std::size_t remaining = strings_size - name_offset;
    const void* nul = std::memchr(raw_name, 0, remaining);
    if (!nul) {
      nod_free(partition);
      return false;
    }
    const std::string name(raw_name, static_cast<const char*>(nul));
    if (!SafeFstName(name)) {
      nod_free(partition);
      return false;
    }

    const std::string full = dirs.back().path.empty() ? name : dirs.back().path + "/" + name;
    if (is_dir) {
      const std::uint32_t end = ReadBE32(node + 8u);
      if (end <= i || end > count) {
        nod_free(partition);
        return false;
      }
      dirs.push_back({end, full});
      continue;
    }

    Entry entry;
    entry.index = i;
    entry.disc_offset = ReadBE32(node + 4u);
    entry.size = ReadBE32(node + 8u);
    entry.path = full;
    entries.push_back(std::move(entry));
  }

  std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
    if (a.disc_offset != b.disc_offset) return a.disc_offset < b.disc_offset;
    return a.index < b.index;
  });

  partition_ = partition;
  extracted_root_ = extracted_root.empty() ? EnvPath("GEKKOAOT_VFS_ROOT") : extracted_root;
  overlay_root_ = overlay_root.empty() ? EnvPath("GEKKOAOT_VFS_OVERLAY") : overlay_root;
  entries_by_offset_ = std::move(entries);
  for (std::size_t i = 0; i < entries_by_offset_.size(); ++i)
    path_to_entry_[NormalizeLookup(entries_by_offset_[i].path)] = i;

  std::fprintf(stderr,
               "GEKKOAOT_NATIVE_VFS_V154=1 backend=nod-fst entries=%zu root=\"%s\" overlay=\"%s\" timing=host-native raw-di=fallback\n",
               entries_by_offset_.size(), extracted_root_.string().c_str(),
               overlay_root_.string().c_str());
  return !entries_by_offset_.empty();
#endif
}

std::optional<NativeVfs::Entry> NativeVfs::FindPath(std::string_view path) const {
  const auto found = path_to_entry_.find(NormalizeLookup(path));
  if (found == path_to_entry_.end() || found->second >= entries_by_offset_.size())
    return std::nullopt;
  return entries_by_offset_[found->second];
}

const NativeVfs::Entry* NativeVfs::FindCoveringEntry(std::uint64_t offset,
                                                      std::uint32_t size) const {
  if (entries_by_offset_.empty()) return nullptr;
  const std::uint64_t end = offset + size;
  if (end < offset) return nullptr;
  const auto it = std::upper_bound(
      entries_by_offset_.begin(), entries_by_offset_.end(), offset,
      [](std::uint64_t value, const Entry& entry) { return value < entry.disc_offset; });
  if (it == entries_by_offset_.begin()) return nullptr;
  const Entry& entry = *std::prev(it);
  const std::uint64_t file_end = entry.disc_offset + entry.size;
  return offset >= entry.disc_offset && end <= file_end ? &entry : nullptr;
}

bool NativeVfs::ReadHostFile(const std::filesystem::path& root, const Entry& entry,
                             std::uint64_t file_offset, void* destination,
                             std::uint32_t size) const {
  if (root.empty() || !destination) return false;
  const auto path = root / std::filesystem::path(entry.path);
  std::error_code ec;
  const std::uint64_t host_size = std::filesystem::file_size(path, ec);
  if (ec || file_offset > host_size || size > host_size - file_offset) return false;
  if (file_offset > static_cast<std::uint64_t>(std::numeric_limits<std::streamoff>::max()))
    return false;
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  in.seekg(static_cast<std::streamoff>(file_offset), std::ios::beg);
  if (!in) return false;
  in.read(static_cast<char*>(destination), static_cast<std::streamsize>(size));
  return in.gcount() == static_cast<std::streamsize>(size);
}

bool NativeVfs::ReadPartitionFile(const Entry& entry, std::uint64_t file_offset,
                                  void* destination, std::uint32_t size) const {
#ifndef GEKKOAOT_HAVE_NOD
  (void)entry;
  (void)file_offset;
  (void)destination;
  (void)size;
  return false;
#else
  if (!partition_ || !destination || file_offset > entry.size || size > entry.size - file_offset)
    return false;
  NodHandle* file = nullptr;
  if (nod_partition_open_file(static_cast<NodHandle*>(partition_), entry.index, &file) !=
          NOD_RESULT_OK ||
      !file)
    return false;
  const auto close = [&]() { nod_free(file); };
  if (file_offset > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
    close();
    return false;
  }
  const auto sought = nod_seek(file, static_cast<std::int64_t>(file_offset), 0);
  if (sought < 0 || static_cast<std::uint64_t>(sought) != file_offset) {
    close();
    return false;
  }
  auto* out = static_cast<std::uint8_t*>(destination);
  std::size_t done = 0;
  while (done < size) {
    const std::int64_t got = nod_read(file, out + done, size - done);
    if (got <= 0) {
      close();
      return false;
    }
    done += static_cast<std::size_t>(got);
  }
  close();
  return true;
#endif
}

bool NativeVfs::ReadDiscRange(std::uint64_t disc_offset, void* destination,
                              std::uint32_t size, const char** source) {
  if (source) *source = nullptr;
  if (!Ready() || !destination) return false;
  if (size == 0u) {
    if (source) *source = "vfs-empty";
    return true;
  }
  const Entry* entry = FindCoveringEntry(disc_offset, size);
  if (!entry) return false;
  const std::uint64_t file_offset = disc_offset - entry->disc_offset;

  if (ReadHostFile(overlay_root_, *entry, file_offset, destination, size)) {
    if (source) *source = "overlay";
    return true;
  }
  if (ReadHostFile(extracted_root_, *entry, file_offset, destination, size)) {
    if (source) *source = "host-root";
    return true;
  }
  if (ReadPartitionFile(*entry, file_offset, destination, size)) {
    if (source) *source = "nod-vfs";
    return true;
  }
  return false;
}

} // namespace GekkoAOT::Native
