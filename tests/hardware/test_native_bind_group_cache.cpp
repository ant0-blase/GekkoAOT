// SPDX-License-Identifier: GPL-3.0-or-later
#include "../../runtime/gx/native_bind_group_cache.h"
#include <array>
#include <cstdio>
#include <cstdlib>

#define CHECK(x) do { if (!(x)) std::abort(); } while (false)
int main() {
  using GekkoAOT::GX::NativeBindGroupCache;
  NativeBindGroupCache cache;
  struct State { std::uintptr_t handle = 0; std::uint32_t offset = 0; };
  std::array<State,3> reference{}, applied{};
  std::uint32_t random = 0x75615923;
  auto next = [&] { random ^= random << 13; random ^= random >> 17;
                   random ^= random << 5; return random; };
  unsigned skipped = 0, resets = 0;
  for (unsigned step = 0; step < 200000; ++step) {
    const auto event = next() % 100;
    if (event < 3) {
      // Begin-pass and arbitrary custom draws can change every binding.
      for (auto& s : reference) s = {next(), next()};
      applied = reference;
      cache.Reset();
      ++resets;
    } else {
      const unsigned group = 1 + next() % 3;
      const auto old = reference[group - 1];
      const State wanted{event < 75 && old.handle ? old.handle : 1u + next()%64u,
                         group == 1 ? (event < 75 ? old.offset : next()%8u*256u) : 0u};
      reference[group - 1] = wanted;
      if (cache.NeedsBind(group, wanted.handle, wanted.offset))
        applied[group - 1] = wanted;
      else ++skipped;
      // Every draw must see exactly the uncached API's complete state.
      for (unsigned i = 0; i < 3; ++i) {
        CHECK(reference[i].handle == applied[i].handle);
        CHECK(reference[i].offset == applied[i].offset);
      }
    }
  }
  CHECK(skipped > 100000 && resets > 1000);
  CHECK(cache.NeedsBind(1,0)); CHECK(cache.NeedsBind(1,0));
  CHECK(cache.NeedsBind(0,123)); CHECK(cache.NeedsBind(4,123));
  cache.Reset(); CHECK(cache.NeedsBind(1,123,256));
  CHECK(!cache.NeedsBind(1,123,256)); CHECK(cache.NeedsBind(1,123,512));
  cache.Reset(); CHECK(cache.NeedsBind(1,123,512));

  // v300: index buffer slot, interleaved with bind groups and resets.
  cache.Reset();
  std::uintptr_t index_reference = 0, index_applied = 0;
  unsigned index_skipped = 0;
  for (unsigned step = 0; step < 100000; ++step) {
    const auto event = next() % 100;
    if (event < 3) {
      index_reference = 1u + next() % 4u;  // a custom encoder may bind anything
      index_applied = index_reference;
      cache.Reset();
    } else if (event < 50) {
      (void)cache.NeedsBind(1 + next() % 3, 1u + next() % 64u);
    } else {
      const std::uintptr_t wanted = event < 95 && index_reference ? index_reference : 1u + next() % 4u;
      index_reference = wanted;
      if (cache.NeedsIndexBuffer(wanted)) index_applied = wanted;
      else ++index_skipped;
      CHECK(index_applied == index_reference);
    }
  }
  CHECK(index_skipped > 10000);
  CHECK(cache.NeedsIndexBuffer(0)); CHECK(cache.NeedsIndexBuffer(0));
  cache.Reset(); CHECK(cache.NeedsIndexBuffer(7)); CHECK(!cache.NeedsIndexBuffer(7));
  CHECK(cache.NeedsBind(1,9)); CHECK(!cache.NeedsIndexBuffer(7));
  cache.Reset(); CHECK(cache.NeedsIndexBuffer(7));
  std::printf("Native bind groups passed draws=200000 skipped=%u encoder_resets=%u dynamic_offsets=exact null_calls=retained\n",skipped,resets);
}
