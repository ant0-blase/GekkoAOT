// SPDX-License-Identifier: GPL-3.0-or-later
#include "native/cheats.h"

#include <charconv>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string_view>
#include <utility>

namespace GekkoAOT::Native {
namespace {
bool ParseHexWord(std::string_view s, std::uint32_t* out) {
  if (s.size() != 8) return false;
  const auto [end, error] = std::from_chars(s.data(), s.data() + s.size(), *out, 16);
  return error == std::errc{} && end == s.data() + s.size();
}
bool ParsePair(const std::string& line, std::uint32_t* lhs, std::uint32_t* rhs) {
  std::istringstream tokens(line);
  std::string a, b, extra;
  return (tokens >> a >> b) && !(tokens >> extra) &&
         ParseHexWord(a, lhs) && ParseHexWord(b, rhs);
}
void AppendBigEndian(std::vector<std::uint8_t>& bytes, std::uint32_t word) {
  for (unsigned shift = 24; ; shift -= 8) {
    bytes.push_back(static_cast<std::uint8_t>(word >> shift));
    if (shift == 0) break;
  }
}
} // namespace

bool CheatEngine::Load(const std::filesystem::path& file, std::string* error) {
  std::ifstream source(file);
  if (!source) {
    if (error) *error = "Cannot open cheat file: " + file.string();
    return false;
  }
  std::vector<Write> parsed;
  std::string line;
  std::uint32_t lineNumber = 0;
  const auto fail = [&](const char* why) {
    if (error) *error = file.string() + ":" + std::to_string(lineNumber) + ": " + why;
    return false;
  };
  while (std::getline(source, line)) {
    ++lineNumber;
    const auto first = line.find_first_not_of(" \t\r");
    if (first == std::string::npos) continue;
    line.erase(0, first);
    if (line[0] == '#' || line[0] == ';' || line[0] == '$' || line[0] == '[') continue;
    const auto comment = line.find_first_of("#;");
    if (comment != std::string::npos) line.resize(comment);
    std::uint32_t head = 0, value = 0;
    if (!ParsePair(line, &head, &value)) return fail("Expected 8-digit Gecko code pair");
    const std::uint32_t type = head >> 24;
    const std::uint32_t address = 0x80000000u | (head & 0x01ffffffu);
    Write write;
    write.address = address;
    if (type == 0x00 || type == 0x02 || type == 0x04) {
      const unsigned count = type == 0x00 ? 1 : type == 0x02 ? 2 : 4;
      for (unsigned i = count; i != 0; --i)
        write.data.push_back(static_cast<std::uint8_t>(value >> ((i - 1u) * 8u)));
    } else if (type == 0x06) {
      // Gecko 06: second word is byte length. Following lines contain raw
      // big-endian 64-bit payload, padded to whole lines in the file.
      if (value == 0 || value > (1u << 20)) return fail("06 payload length must be 1..1048576 bytes");
      write.data.reserve((value + 7u) & ~7u);
      for (std::uint32_t i = 0; i < (value + 7u) / 8u; ++i) {
        if (!std::getline(source, line)) return fail("Truncated Gecko 06 payload");
        ++lineNumber;
        std::uint32_t a = 0, b = 0;
        if (!ParsePair(line, &a, &b)) return fail("Malformed Gecko 06 payload");
        AppendBigEndian(write.data, a);
        AppendBigEndian(write.data, b);
      }
      write.data.resize(value);
    } else {
      return fail("Unsupported code type (only 00, 02, 04, 06 data writes)");
    }
    // Reject all MMIO, uncached aliases, MEM2 and invalid/out-of-range data
    // at load time. This initial implementation is deliberately MEM1-only.
    const auto physical = AddressSpace::ToPhysical(address);
    if (physical >= AddressSpace::RetailMem1Size ||
        write.data.size() > AddressSpace::RetailMem1Size - physical)
      return fail("Cheat target outside retail MEM1 or crosses RAM boundary");
    parsed.push_back(std::move(write));
  }
  if (source.bad()) return fail("Failed reading cheat file");
  writes_ = std::move(parsed);
  file_ = file;
  std::error_code ec;
  file_time_ = std::filesystem::last_write_time(file_, ec);
  if (error) error->clear();
  return true;
}

bool CheatEngine::ReloadIfChanged(std::string* error) {
  if (file_.empty()) return true;
  std::error_code ec;
  const auto stamp=std::filesystem::last_write_time(file_, ec);
  if(ec) {
    if(error) *error="Cannot stat cheat file: "+file_.string();
    return false;
  }
  if(stamp==file_time_) return true;
  // Load() only publishes a complete validated file.  Failed loads keep the
  // old cheat set and are retried next polling interval.
  return Load(file_, error);
}

std::uint32_t CheatEngine::Apply(AddressSpace& memory,
                                const std::function<void(std::uint32_t, std::uint32_t)>& notify) const {
  std::uint32_t changed = 0;
  for (const Write& write : writes_) {
    auto* destination = memory.Resolve(write.address, write.data.size());
    if (!destination || std::memcmp(destination, write.data.data(), write.data.size()) == 0) continue;
    std::memcpy(destination, write.data.data(), write.data.size());
    ++changed;
    if (notify) notify(write.address, static_cast<std::uint32_t>(write.data.size()));
  }
  return changed;
}
} // namespace GekkoAOT::Native
