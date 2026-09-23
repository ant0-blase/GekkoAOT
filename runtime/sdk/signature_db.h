#pragma once
#include <cstdint>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace GekkoAOTSdk {
// Strict DSY reader for HLE admission. Dolphin's debugger naming database may
// rename a checksum match even when its size differs; that is not HLE evidence.
class SdkSignatureDB {
  using Key = std::pair<uint32_t, uint32_t>;
  std::map<Key, std::set<std::string>> names;
  static uint32_t LE(const unsigned char* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
  }
public:
  bool Load(const char* path) {
    names.clear();
    std::ifstream file(path, std::ios::binary);
    unsigned char count_bytes[4];
    if (!file.read(reinterpret_cast<char*>(count_bytes), 4)) return false;
    const uint32_t count = LE(count_bytes);
    if (count > 1000000) return false;
    for (uint32_t i = 0; i < count; ++i) {
      unsigned char bytes[136];
      if (!file.read(reinterpret_cast<char*>(bytes), sizeof(bytes))) { names.clear(); return false; }
      size_t length = 0;
      while (length < 128 && bytes[8 + length]) ++length;
      if (!length || length == 128 || !LE(bytes + 4) || (LE(bytes + 4) & 3)) continue;
      names[{LE(bytes), LE(bytes + 4)}].insert(std::string(reinterpret_cast<char*>(bytes + 8), length));
    }
    return true;
  }
  bool Matches(uint32_t hash, uint32_t size, const std::string& name) const {
    const auto it = names.find({hash, size});
    return it != names.end() && it->second.size() == 1 && *it->second.begin() == name;
  }
};
} // namespace GekkoAOTSdk
