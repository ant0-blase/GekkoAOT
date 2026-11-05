// SPDX-License-Identifier: GPL-3.0-or-later
#include "gx/native_index_range_cache.h"
#include <array>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <memory>
#define CHECK(x) do{if(!(x)){std::cerr<<__LINE__<<": " #x "\n";std::exit(1);}}while(false)
using Cache=GekkoAOT::GX::NativeIndexRangeCache;
static Cache::Bounds Scan(const std::array<std::uint8_t,128>& bytes) {
  Cache::Bounds out;out.minimum.fill(65535);
  for(unsigned vertex=0;vertex<16;++vertex)for(unsigned field=0;field<4;++field) {
    const auto at=vertex*8+field*2;const unsigned index=(unsigned(bytes[at])<<8)|bytes[at+1];
    if(index<out.minimum[field])out.minimum[field]=index;
    if(index>out.maximum[field])out.maximum[field]=index;
  }
  return out;
}
static bool Equal(const Cache::Bounds& a,const Cache::Bounds& b){return a.minimum==b.minimum&&a.maximum==b.maximum;}
int main() {
  auto cache=std::make_unique<Cache>();std::array<std::uint8_t,112> layout{};
  std::array<std::uint8_t,128> bytes{};std::uint64_t rng=0xace72645;
  auto random=[&](){rng^=rng<<13;rng^=rng>>7;rng^=rng<<17;return rng;};
  for(unsigned iteration=0;iteration<100000;++iteration) {
    for(auto& b:bytes)b=random();layout[17]=random();layout[20]=random();
    const auto bounds=Scan(bytes);
    if(const auto* old=cache->Find(layout.data(),layout.size(),bytes.data(),bytes.size()))CHECK(Equal(*old,bounds));
    cache->Insert(layout.data(),layout.size(),bytes.data(),bytes.size(),bounds);
    const auto* found=cache->Find(layout.data(),layout.size(),bytes.data(),bytes.size());CHECK(found&&Equal(*found,bounds));
    // Same set selector, length and pointer; an interior mutation must miss.
    auto mutated=bytes;mutated[65]^=0x80;
    CHECK(!cache->Find(layout.data(),layout.size(),mutated.data(),mutated.size()));
    auto wrong_layout=layout;wrong_layout[55]^=1;
    CHECK(!cache->Find(wrong_layout.data(),wrong_layout.size(),bytes.data(),bytes.size()));
    // Different host pointer with identical bytes is a legal cache hit.
    auto alias=bytes;found=cache->Find(layout.data(),layout.size(),alias.data(),alias.size());CHECK(found&&Equal(*found,bounds));
  }
  std::array<std::uint8_t,513> large{};
  CHECK(!cache->Find(layout.data(),layout.size(),large.data(),large.size()));
  CHECK(!cache->Find(nullptr,0,bytes.data(),bytes.size()));
  cache->Clear();CHECK(!cache->Find(layout.data(),layout.size(),bytes.data(),bytes.size()));
  std::cout<<"100000 exact index snapshots: payload/layout collisions, pointer aliases, eviction, overflow and reset passed\n";
}
