// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <charconv>
#include <cstdint>
#include <istream>
#include <stdexcept>
#include <string>
#include <vector>
namespace GekkoAOT::Build {
inline std::vector<std::uint32_t> ReadNativeHotProfile(std::istream& input,
                                                    const std::string& dol_hash) {
  std::string line;
  if (!std::getline(input, line) ||
      line != "# gekkoaot-native-hot-v285 dol-fnv64=" + dol_hash)
    throw std::runtime_error("native hot-function profile belongs to another DOL or schema");
  std::vector<std::uint32_t> result;
  while (std::getline(input, line)) {
    if (line.empty() || line[0]=='#') continue;
    std::uint32_t address = 0;
    const auto parsed = std::from_chars(line.data(), line.data()+line.size(), address, 16);
    if (parsed.ec != std::errc{} || parsed.ptr != line.data()+line.size() ||
        (address & 3u) || (!result.empty() && address <= result.back()) || result.size() >= 4096)
      throw std::runtime_error("native hot-function addresses must be sorted unique aligned hex words");
    result.push_back(address);
  }
  if (result.empty() || input.bad()) throw std::runtime_error("empty or unreadable native hot-function profile");
  return result;
}
}
