#pragma once
// SPDX-License-Identifier: GPL-3.0-or-later

#include "native/address_space.h"
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace GekkoAOT::Native {
// Data-only Gecko RAM codes. Unencrypted Action Replay and Gecko assembly
// hooks are deliberately not guessed: translated host machine code cannot be
// modified by patching the source PowerPC instruction bytes.
class CheatEngine final {
public:
  struct Write {
    std::uint32_t address = 0;
    std::vector<std::uint8_t> data;
  };
  bool Load(const std::filesystem::path& file, std::string* error = nullptr);
  void Clear() { writes_.clear(); file_.clear(); }
  // Reload settings changed by the GUI without stopping the running game.
  // PPC instruction changes still require a fresh AOT compilation.
  bool ReloadIfChanged(std::string* error = nullptr);
  std::size_t Count() const noexcept { return writes_.size(); }
  // Invoke on the guest CPU owner thread at a VI frame boundary. No locking,
  // filesystem I/O or allocations happen here. Callback is called on changes.
  std::uint32_t Apply(AddressSpace& memory,
                      const std::function<void(std::uint32_t, std::uint32_t)>& notify = {}) const;
private:
  std::vector<Write> writes_;
  std::filesystem::path file_;
  std::filesystem::file_time_type file_time_{};
};
} // namespace GekkoAOT::Native
