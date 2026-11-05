#include "../../runtime/gx/native_snapshot_atlas.h"
#include "../../runtime/gx/compact_array_window.h"
#include <cstdlib>
#include <iostream>
#include <random>
#include <vector>

#define check(condition) do { if (!(condition)) { std::cerr << "check failed at " << __LINE__ << ": " << #condition << "\n"; std::abort(); } } while (false)
int main() {
  using namespace GekkoAOT::GX;
  NativeSnapshotAtlas atlas;
  std::vector<std::uint8_t> guest(65536), staging, alias(65536);
  std::mt19937 rng(301);
  for (auto& byte : guest) byte = rng();
  atlas.Scope(1, 0);
  std::uint64_t hits = 0;
  for (unsigned step = 0; step < 200000; ++step) {
    if (step % 997 == 0) { staging.clear(); atlas.Scope(2 + step / 997, step % 3); }
    const std::size_t start = (rng() % 16) * 4096;
    const std::size_t length = step % 2 ? 4096 : 1 + rng() % 4096;
    if (step % 3 == 0) guest[start + rng() % length] ^= 1;
    auto offset = atlas.Find(guest.data() + start, length, staging.size());
    if (offset) {
      ++hits;
      check(*offset <= staging.size() && length <= staging.size() - *offset);
      check(std::memcmp(guest.data() + start, staging.data() + *offset, length) == 0);
    } else {
      const auto oldSize = staging.size();
      const std::uint8_t first = oldSize ? staging.front() : 0, last = oldSize ? staging.back() : 0;
      const auto at = static_cast<std::uint32_t>(staging.size());
      staging.insert(staging.end(), guest.begin() + start, guest.begin() + start + length);
      check(staging.size() == oldSize + length);
      if (oldSize) check(staging.front() == first && staging[oldSize - 1] == last);
      atlas.Remember(guest.data() + start, length, at, atlas.Capture(guest.data() + start, length));
      offset = atlas.Find(guest.data() + start, length, staging.size());
      check(offset && *offset == at); // Includes reallocations and collisions.
    }
  }
  // Reusing the source address after a write must allocate a fresh snapshot;
  // restoring it can reuse the original immutable version, when still cached.
  staging.assign(guest.begin(), guest.begin() + 4096); atlas.Scope(900, 0);
  atlas.Remember(guest.data(), 4096, 0, atlas.Capture(guest.data(), 4096));
  check(atlas.Find(guest.data(), 4096, staging.size()) == 0);
  guest[300] ^= 7;
  check(!atlas.Find(guest.data(), 4096, staging.size()));
  guest[300] ^= 7;
  check(atlas.Find(guest.data(), 4096, staging.size()) == 0);
  alias = guest;
  check(!atlas.Find(alias.data(), 4096, staging.size()));
  check(!atlas.Find(guest.data(), 4096, 4095));
  atlas.Scope(900, 1);
  check(!atlas.Find(guest.data(), 4096, staging.size()));
  atlas.Remember(guest.data(), 4096, 0, atlas.Capture(guest.data(), 4096)); atlas.Scope(901, 1);
  check(!atlas.Find(guest.data(), 4096, staging.size()));
  check(!atlas.Find(nullptr, 1, staging.size()));
  check(!atlas.Find(guest.data(), 0, staging.size()));
  atlas.Scope(950, 0);
  const auto captured = atlas.Capture(guest.data(), 4096);
  check(bool(captured));
  const auto capturedValue = captured.bytes[23];
  guest[23] ^= 1; // Writes after capture cannot change the uploaded snapshot.
  check(captured.bytes[23] == capturedValue);
  atlas.Remember(guest.data(), 4096, 0, captured);
  check(!atlas.Find(guest.data(), 4096, 4096));
  guest[23] ^= 1;
  check(atlas.Find(guest.data(), 4096, 4096) == 0);
  check(!atlas.Capture(guest.data(), 65537));
  NativeSnapshotAtlas tiny(8192);
  tiny.Scope(1, 0);
  check(bool(tiny.Capture(guest.data(), 4096)));
  check(bool(tiny.Capture(guest.data(), 4096)));
  check(!tiny.Capture(guest.data(), 1));
  tiny.Scope(2, 0);
  check(bool(tiny.Capture(guest.data(), 8192)));
  // Recycling never changes an earlier GPU-visible snapshot. Exercise a
  // repeatedly rewritten guest scratch array well beyond the shadow budget,
  // then restore older contents: evicted versions must miss, not alias.
  NativeSnapshotAtlas recycling(16384, true);
  NativeSnapshotAtlas legacy(16384, false);
  recycling.Scope(1,0);legacy.Scope(1,0);staging.clear();
  for (unsigned step=0;step<10000;++step) {
    guest[0]=step;guest[1]=step>>8;
    const auto at=static_cast<std::uint32_t>(staging.size());
    const auto snapshot=recycling.Capture(guest.data(),4096);
    check(bool(snapshot));
    staging.insert(staging.end(),snapshot.bytes,snapshot.bytes+4096);
    recycling.Remember(guest.data(),4096,at,snapshot);
    check(recycling.Find(guest.data(),4096,staging.size())==at);
    const auto old=legacy.Capture(guest.data(),4096);
    if(step<4) {check(bool(old));legacy.Remember(guest.data(),4096,at,old);}
    else check(!old);
    check(recycling.ShadowBytes()==std::min(step+1,4u)*4096);
  }
  check(recycling.RecycledSnapshots()==9996);
  check(recycling.BudgetDeclines()==0&&legacy.BudgetDeclines()==9996);
  for(unsigned step=0;step<10000;++step) {
    check(staging[step*4096]==std::uint8_t(step)&&staging[step*4096+1]==std::uint8_t(step>>8));
  }
  guest[0]=0;guest[1]=0;
  check(!recycling.Find(guest.data(),4096,staging.size()));
  recycling.Scope(2,0);check(!recycling.Find(guest.data(),4096,staging.size()));
  const auto low = MakeCompactArrayWindow(1, 4, 12, 12, 8192, 4096, true);
  check(bool(low) && low.prefix == 12 && low.bytes == 48);
  // Stable tiles can extend the draw high-water, but never the mapped RAM
  // span. This makes nearby triangles share a native base even when SDK array
  // bindings reset their visible size before every draw.
  const auto firstTile = TileCompactArrayWindow({8400, 48}, 24000, 8448);
  const auto nextTile = TileCompactArrayWindow({8424, 24}, 24000, 8448);
  check(firstTile.prefix == nextTile.prefix && firstTile.bytes == nextTile.bytes);
  check(firstTile.prefix + firstTile.bytes <= 24000);
  const auto edge = TileCompactArrayWindow({8400, 48}, 8448, 8448);
  check(edge.prefix + edge.bytes == 8448);
  for (unsigned i = 0; i < 100000; ++i) {
    const std::uint32_t prefix = rng() % 65536, bytes = 1 + rng() % 8192;
    const auto limit = prefix + bytes + rng() % 8192;
    const CompactArrayWindow raw{prefix, bytes};
    const auto tiled = TileCompactArrayWindow(raw, limit, limit);
    check(tiled.prefix <= raw.prefix);
    check(std::uint64_t(tiled.prefix) + tiled.bytes >= std::uint64_t(raw.prefix) + raw.bytes);
    check(std::uint64_t(tiled.prefix) + tiled.bytes <= limit);
    for (auto source : {raw.prefix, raw.prefix + raw.bytes - 1})
      check(CompactArrayShaderBase(0, tiled.prefix) + source == source - tiled.prefix);
  }
  std::cout << "Native snapshots: 200000 mutation/reallocation/collision checks, "
            << hits << " hits, 100000 bounded tile checks passed\n";
}
