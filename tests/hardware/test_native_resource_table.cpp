#include "../../runtime/gx/native_resource_table.h"
#include <cstdlib>
#include <iostream>
#include <random>
#include <vector>

#define check(x) do { if (!(x)) { std::cerr << "failed at " << __LINE__ << ": " << #x << '\n'; std::abort(); } } while (false)
int main() {
  using GekkoAOT::GX::NativeResourceTable;
  static_assert(sizeof(NativeResourceTable) < 20u * 1024u);
  NativeResourceTable resources;
  std::mt19937 random(314);
  std::vector<std::uint8_t> guest(65536), alias(65536), arena;
  for (auto& byte : guest) byte = random();
  std::uint64_t hits = 0, copies = 0;
  for (unsigned step = 0; step < 200000; ++step) {
    if (step % 997 == 0) { arena.clear(); resources.Scope(1 + step / 997, step % 3); }
    const std::size_t start = (random() % 16) * 4096;
    const std::size_t length = step % 2 ? 4096 : 1 + random() % 4096;
    if (step % 3 == 0) guest[start + random() % length] ^= 1;
    const auto* source = guest.data() + start;
    if (auto found = resources.Find(source, length, arena)) {
      ++hits;
      check(*found <= arena.size() && length <= arena.size() - *found);
      check(std::memcmp(source, arena.data() + *found, length) == 0);
    } else {
      ++copies;
      const auto offset = static_cast<std::uint32_t>(arena.size());
      arena.insert(arena.end(), source, source + length);
      check(resources.Remember(source, length, offset, arena));
      check(resources.Find(source, length, arena) == offset);
    }
  }
  // No second shadow budget: continuously rewritten arrays remain cacheable
  // beyond the old 4 MiB limit. Every old upload retains its original bytes.
  resources.Scope(9999, 0); arena.clear();
  for (unsigned version = 0; version < 10000; ++version) {
    guest[0] = version; guest[1] = version >> 8;
    const auto offset = static_cast<std::uint32_t>(arena.size());
    check(!resources.Find(guest.data(), 4096, arena));
    arena.insert(arena.end(), guest.begin(), guest.begin() + 4096);
    check(resources.Remember(guest.data(), 4096, offset, arena));
    check(resources.Find(guest.data(), 4096, arena) == offset);
  }
  for (unsigned version = 0; version < 10000; ++version) {
    check(arena[version * 4096] == std::uint8_t(version));
    check(arena[version * 4096 + 1] == std::uint8_t(version >> 8));
  }
  const auto last = static_cast<std::uint32_t>(arena.size() - 4096);
  guest[2] ^= 1;
  check(!resources.Find(guest.data(), 4096, arena));
  guest[2] ^= 1;
  check(resources.Find(guest.data(), 4096, arena) == last);
  alias = guest;
  check(!resources.Find(alias.data(), 4096, arena));
  check(!resources.Find(guest.data(), 4096, std::span(arena.data(), last + 4095u)));
  check(!resources.Remember(guest.data(), 4096, last + 1u, arena));
  check(!resources.Remember(nullptr, 1, 0, arena));
  check(!resources.Remember(guest.data(), 0, 0, arena));
  check(!resources.Remember(guest.data(), 65537, 0, arena));
  resources.Scope(9999, 1);
  check(!resources.Find(guest.data(), 4096, arena));
  check(resources.Remember(guest.data(), 4096, last, arena));
  resources.Scope(10000, 1);
  check(!resources.Find(guest.data(), 4096, arena));
  resources = {};
  check(!resources.Find(guest.data(), 4096, arena));
  std::cout << "native resource table: 210000 mutation/reallocation/scope cases PASS hits=" << hits << " copies=" << copies << " shadow_bytes=0\n";
}
