// SPDX-License-Identifier: GPL-3.0-or-later
#include "../../runtime/gx/compact_array_window.h"
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <initializer_list>

#define CHECK(x) do { if(!(x)) { std::fprintf(stderr,"%d: %s\n",__LINE__,#x);std::exit(1); } } while(false)
int main() {
  using namespace GekkoAOT::GX;
  const auto positions=MakeCompactArrayWindow(700,703,12,12,24u*1024u*1024u,8448);
  CHECK(positions.prefix==8400 && positions.bytes==48);
  CHECK(!MakeCompactArrayWindow(0,65535,12,12,786432,786432));
  CHECK(!MakeCompactArrayWindow(65535,65534,12,12,786432,786432));
  CHECK(!MakeCompactArrayWindow(65535,65536,12,12,786444,786444));
  CHECK(!MakeCompactArrayWindow(700,703,12,12,8447,8448));
  CHECK(!MakeCompactArrayWindow(700,703,12,12,8448,8447));
  CHECK(!MakeCompactArrayWindow(65500,65535,0,12,786432,786432));
  std::uint32_t random=0x89125347u;
  const auto next=[&] { random^=random<<13;random^=random>>17;random^=random<<5;return random; };
  std::uint64_t checked=0,wrapping=0;
  for(unsigned trial=0;trial<300000u;++trial) {
    const auto minimum=next()&65535u;
    const auto maximum=minimum+next()%(65536u-minimum);
    const auto stride=1u+next()%255u;
    const auto element=1u+next()%36u;
    const auto end=maximum*stride+element;
    const auto visible=end+next()%4096u;
    const auto window=MakeCompactArrayWindow(minimum,maximum,stride,element,visible,visible);
    if(!window) continue;
    CHECK((window.prefix&3u)==0u);
    CHECK(window.prefix<=minimum*stride);
    CHECK(window.prefix+window.bytes==end);
    const auto staged=(trial&1u) ? (next()%67100000u)&~15u : 0u;
    const auto base=CompactArrayShaderBase(staged,window.prefix);
    if(staged<window.prefix) ++wrapping;
    for(const auto index : {minimum,maximum,minimum+(maximum-minimum)/2u}) {
      for(unsigned component=0;component<element;++component) {
        const std::uint64_t source=std::uint64_t(index)*stride+component;
        const auto reference=std::uint64_t(staged)+source-window.prefix;
        const auto shader=std::uint32_t(base+index*stride+component);
        CHECK(reference==shader);
        CHECK(shader>=staged && shader<staged+window.bytes);
        CHECK((shader>>2u)>=staged/4u);
        ++checked;
      }
    }
  }
  CHECK(checked>1000000u && wrapping>10000u);
  std::printf("Compact array addresses passed checks=%llu wrapping_windows=%llu\n",
              static_cast<unsigned long long>(checked),static_cast<unsigned long long>(wrapping));
}
