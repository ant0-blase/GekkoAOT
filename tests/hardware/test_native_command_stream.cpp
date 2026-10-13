// SPDX-License-Identifier: GPL-3.0-or-later
#include "../../runtime/gx/native_command_stream.h"
#include <cstdio>
#include <cstdlib>
#include <vector>
#define CHECK(x) do {if(!(x)){std::fprintf(stderr,"%d: %s\n",__LINE__,#x);std::exit(1);}}while(false)
using Stream=GekkoAOT::GX::NativeCommandStream;
static std::vector<std::uint8_t> Packet(Stream::ArrayBinding b) {
  b.base&=0x3fffffffu;b.stride&=255;
  return {8,std::uint8_t(0xa0+b.index),std::uint8_t(b.base>>24),std::uint8_t(b.base>>16),
    std::uint8_t(b.base>>8),std::uint8_t(b.base),8,std::uint8_t(0xb0+b.index),0,0,0,std::uint8_t(b.stride)};
}
struct Consumer {
  Stream stream;
  Consumer() {
    const auto* value=std::getenv("GEKKOAOT_NATIVE_GX_DIRECT_COMMAND_SPANS");
    stream.SetDirectSpansEnabled(!value||*value!='0');
  }
  std::vector<std::uint8_t> output;
  bool boundary=true;
  bool Feed(const std::uint8_t* p,std::size_t n) {
    return stream.Consume(p,n,
      [&](const std::uint8_t* raw,std::size_t size){output.insert(output.end(),raw,raw+size);return true;},
      [&]{return boundary;},
      [&](Stream::ArrayBinding b){auto packet=Packet(b);output.insert(output.end(),packet.begin(),packet.end());return true;},
      [&](std::uint32_t addr,std::uint32_t words,const std::uint8_t* values){
        output.insert(output.end(),{0x10,0,std::uint8_t(words-1),std::uint8_t(addr>>8),std::uint8_t(addr)});
        output.insert(output.end(),values,values+words*4);return true;
      });
  }
};
int main() {
  unsigned cases=0;
  // Every gather position, fragmentation size and binding. The native callback
  // reconstructs canonical bytes only in the test so ordering has an independent
  // transport oracle. Production never reconstructs these commands.
  for(unsigned before=0;before<32;++before)for(unsigned chunk=1;chunk<=64;++chunk)
    for(unsigned index=0;index<16;++index) {
      Consumer c;Stream::ArrayBinding binding{index,0x81234567u+index,0x10fu+index};
      std::vector<std::uint8_t> bytes(before,0);
      auto packet=Packet(binding);bytes.insert(bytes.end(),packet.begin(),packet.end());
      bytes.insert(bytes.end(),33,0);
      CHECK(c.stream.Stage(binding,before));CHECK(c.output.empty());
      for(std::size_t i=0;i<bytes.size();i+=chunk)CHECK(c.Feed(bytes.data()+i,std::min<std::size_t>(chunk,bytes.size()-i)));
      CHECK(c.output==bytes);CHECK(c.stream.NativeCalls()==1);CHECK(!c.stream.FallbackCalls());++cases;
    }
  // A changed byte or a command starting inside a custom primitive must replay
  // the original stream verbatim. Partial commands remain pending across calls.
  for(unsigned position=0;position<12;++position)for(unsigned bit=0;bit<8;++bit)
    for(unsigned before=0;before<32;++before) {
      Consumer c;Stream::ArrayBinding b{7,0x123456,7};auto packet=Packet(b);packet[position]^=1u<<bit;
      std::vector<std::uint8_t> bytes(before,0);bytes.insert(bytes.end(),packet.begin(),packet.end());
      CHECK(c.stream.Stage(b,before));
      for(auto& byte:bytes)CHECK(c.Feed(&byte,1));
      CHECK(c.output==bytes);CHECK(!c.stream.NativeCalls());CHECK(c.stream.FallbackCalls()==1);++cases;
    }
  for(unsigned before=0;before<32;++before) {
    Consumer c;c.boundary=false;Stream::ArrayBinding b{0,0x10000,12};auto packet=Packet(b);
    std::vector<std::uint8_t> bytes(before,0x80);bytes.insert(bytes.end(),packet.begin(),packet.end());
    CHECK(c.stream.Stage(b,before));CHECK(c.Feed(bytes.data(),bytes.size()));
    CHECK(c.output==bytes);CHECK(c.stream.FallbackCalls()==1);++cases;
  }
  // Three annotations can precede a single gather publication. A fourth is
  // queued after that publication, while the third packet is still incomplete.
  Consumer c;std::vector<std::uint8_t> bytes;
  for(unsigned i=0;i<3;++i){Stream::ArrayBinding b{i,0x1000+32*i,12};CHECK(c.stream.Stage(b,12*i));
    auto p=Packet(b);bytes.insert(bytes.end(),p.begin(),p.end());}
  CHECK(c.Feed(bytes.data(),32));CHECK(c.stream.NativeCalls()==2);
  Stream::ArrayBinding b{4,0x2000,8};CHECK(c.stream.Stage(b,4));auto packet=Packet(b);
  bytes.insert(bytes.end(),packet.begin(),packet.end());CHECK(c.Feed(bytes.data()+32,bytes.size()-32));
  CHECK(c.output==bytes);CHECK(c.stream.NativeCalls()==4);
  CHECK(!c.stream.Stage({16,0,0},0));CHECK(!c.stream.Stage(b,32));
  CHECK(c.stream.Stage(b,0));CHECK(!c.stream.Stage(b,11));CHECK(c.stream.DeclinedHints()==3);
  c.stream.Reset();CHECK(!c.stream.NativeCalls());CHECK(!c.stream.FallbackCalls());CHECK(!c.stream.DeclinedHints());
  std::uint8_t tail[]{1,2,3};c.output.clear();CHECK(c.Feed(tail,3));CHECK(c.output==std::vector<std::uint8_t>({1,2,3}));
  CHECK(!c.Feed(nullptr,1));
  // Upload values are immutable host data. Every fragment boundary, each
  // matrix bank and special float bit patterns survive without a FIFO decoder.
  for(unsigned before=0;before<32;++before)for(unsigned chunk=1;chunk<=64;++chunk)
    for(auto [address,words]:std::array<std::pair<unsigned,unsigned>,7>{{
        {0,12},{244,12},{8,8},{0x400,9},{0x457,9},{0x500,12},{0x5e4,12}}}) {
      Consumer m;std::vector<std::uint8_t> bytes(before,0);
      bytes.insert(bytes.end(),{0x10,0,std::uint8_t(words-1),std::uint8_t(address>>8),std::uint8_t(address)});
      const std::uint32_t patterns[]{0,0x80000000u,1,0x7f800000u,0x7fc12345u,0x3f800000u};
      for(unsigned i=0;i<words;++i)for(unsigned shift:{24,16,8,0})bytes.push_back(patterns[i%6]>>shift);
      bytes.insert(bytes.end(),17,0);
      CHECK(m.stream.StageMatrix(address,words,before));
      for(std::size_t i=0;i<bytes.size();i+=chunk)CHECK(m.Feed(bytes.data()+i,std::min<std::size_t>(chunk,bytes.size()-i)));
      CHECK(m.output==bytes);CHECK(m.stream.MatrixCalls()==1);CHECK(m.stream.NativeCalls()==1);++cases;
    }
  for(unsigned byte=0;byte<5;++byte)for(unsigned bit=0;bit<8;++bit) {
    Consumer m;std::vector<std::uint8_t> bytes(53,0);bytes[0]=0x10;bytes[2]=11;
    bytes[byte]^=1u<<bit;CHECK(m.stream.StageMatrix(0,12,0));
    for(auto value:bytes)CHECK(m.Feed(&value,1));
    CHECK(m.output==bytes);CHECK(!m.stream.MatrixCalls());CHECK(m.stream.FallbackCalls()==1);++cases;
  }
  // A plausible matrix header inside custom vertex data is never a native op.
  Consumer m;m.boundary=false;std::vector<std::uint8_t> matrix_bytes(53,0);matrix_bytes[0]=0x10;matrix_bytes[2]=11;
  CHECK(m.stream.StageMatrix(0,12,0));CHECK(m.Feed(matrix_bytes.data(),matrix_bytes.size()));
  CHECK(m.output==matrix_bytes);CHECK(m.stream.FallbackCalls()==1);CHECK(!m.stream.MatrixCalls());
  for(auto [addr,words]:std::array<std::pair<unsigned,unsigned>,8>{{
      {1,12},{252,8},{0x100,12},{0x401,9},{0x45d,9},{0x500,8},{0x501,12},{0x5f0,12}}})
    CHECK(!m.stream.StageMatrix(addr,words,0));
  CHECK(!m.stream.StageMatrix(0,12,32));
  std::printf("Native command stream: %u fragmentation/mutation cases passed\n",cases);
}
