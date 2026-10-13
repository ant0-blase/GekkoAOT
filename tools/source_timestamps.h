// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <stdexcept>
#include <vector>

namespace GekkoAOT::Build {

// Source checkouts still get reset and patched on every preparation. Preserve
// timestamps only when the final bytes are identical, so Ninja can reuse its
// objects without trusting a marker that might hide a changed patch or source.
class SourceTimestamps final {
public:
  explicit SourceTimestamps(const std::filesystem::path& root) {
    namespace fs = std::filesystem;
    for (auto it = fs::recursive_directory_iterator(root);
         it != fs::recursive_directory_iterator(); ++it) {
      if (it->is_directory()) {
        if (it->path().filename() == ".git" || fs::exists(it->path() / ".git"))
          it.disable_recursion_pending(); // Submodules have their own checkout.
        continue;
      }
      if (!it->is_regular_file()) continue;
      const auto extension = it->path().extension().string();
      if (extension != ".c" && extension != ".cpp" && extension != ".h" &&
          extension != ".hpp" && extension != ".in" && extension != ".cmake" &&
          it->path().filename() != "CMakeLists.txt") continue;
      entries_.push_back({it->path(), Read(it->path()), it->last_write_time()});
    }
  }

  std::size_t RestoreUnchanged() const {
    std::size_t restored = 0;
    for (const auto& entry : entries_) {
      if (!std::filesystem::is_regular_file(entry.path) ||
          std::filesystem::last_write_time(entry.path) == entry.time) continue;
      if (Read(entry.path) == entry.bytes) {
        std::filesystem::last_write_time(entry.path, entry.time);
        ++restored;
      }
    }
    return restored;
  }

private:
  static std::string Read(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("cannot read source: " + path.string());
    std::string bytes{std::istreambuf_iterator<char>(input), {}};
    if (input.bad()) throw std::runtime_error("cannot read source: " + path.string());
    return bytes;
  }
  struct Entry {
    std::filesystem::path path;
    std::string bytes;
    std::filesystem::file_time_type time;
  };
  std::vector<Entry> entries_;
};
} // namespace GekkoAOT::Build
