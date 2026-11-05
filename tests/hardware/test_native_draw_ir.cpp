// SPDX-License-Identifier: GPL-3.0-or-later
#include "../../runtime/gx/native_draw_ir.h"
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <ctime>
#include <string_view>
#define CHECK(x) do{if(!(x)){std::fprintf(stderr,"%d: %s\n",__LINE__,#x);std::exit(1);}}while(false)
using namespace GekkoAOT::GX;
static void Word(std::vector<std::uint8_t>& v,unsigned w){for(int s=24;s>=0;s-=8)v.push_back(w>>s);}
static void Cp(std::vector<std::uint8_t>& v,unsigned r,unsigned w){v.push_back(8);v.push_back(r);Word(v,w);}
static void Bp(std::vector<std::uint8_t>& v,unsigned r,unsigned w){v.push_back(0x61);Word(v,(r<<24)|w);}
int main(int argc,char** argv){
  NativeDrawLayout layout;
  layout.Write(0x50,1<<9);layout.Write(0x60,0);
  for(unsigned f=0;f<8;++f){layout.Write(0x70+f,9);layout.Write(0x80+f,0);layout.Write(0x90+f,0);}
  CHECK(layout.Stride(0)==12 && layout.Stride(7)==12 && layout.Stride(8)==0);
  auto missing=layout;missing.valid.reset(3);missing.valid.reset(4);CHECK(missing.Stride(0)==12);
  missing.Write(0x60,1u<<10);CHECK(missing.Stride(0)==0); // direct TEX5 requires VAT C
  missing.Write(0x60,2u<<10);CHECK(missing.Stride(0)==13); // indexed TEX5 does not
  // Matrix indices, NBT3 indices and packed colors affect framing separately.
  auto nbt=layout;nbt.Write(0x50,511|(3<<11)|(1<<13));nbt.Write(0x70,(1u<<31)|(1<<9)|(5<<14));
  CHECK(nbt.Stride(0)==9+6+4);
  NativeDrawListCache cache;
  for(unsigned topology=0x80;topology<=0xb8;topology+=8){
    std::vector<std::uint8_t> bytes;Bp(bytes,0x48,0x1234);
    bytes.insert(bytes.end(),{std::uint8_t(topology|3),0,3});bytes.resize(bytes.size()+36,42);Bp(bytes,0x48,0x5678);
    auto plan=cache.Find(0x1000,bytes,layout);
    CHECK(plan.operations && plan.commands==3 && plan.draws==1 && !plan.cached);
    const auto& draw=std::get<NativeDraw>((*plan.operations)[1]);
    CHECK(draw.topology==NativeTopology(topology) && draw.format==3 && draw.count==3 && draw.stride==12 && draw.offset==5 && draw.bytes==39);
    CHECK(std::get<NativePixelState>(std::get<NativeOperation>(plan.operations->back())).value==0x5678);
    CHECK(cache.Find(0x1000,bytes,layout).cached);
    // CPU changes to the list allocation cannot mutate a published plan.
    bytes[8]=99;CHECK(plan.source[8]==42);
    CHECK(!cache.Find(0x1000,bytes,layout).cached);
    // Same bytes under an incompatible entry VCD must not reuse framing.
    auto indexed=layout;indexed.Write(0x50,3<<9);
    CHECK(!cache.Find(0x1000,bytes,indexed).cached);
  }
  // Local VCD/VAT changes are simulated with no external state side effects.
  std::vector<std::uint8_t> mixed;Cp(mixed,0x50,3<<9);mixed.insert(mixed.end(),{0x90,0,3,0,1,0,2,0,3});
  Cp(mixed,0x50,1<<9);mixed.insert(mixed.end(),{0x98,0,4});mixed.resize(mixed.size()+48,0);
  auto plan=cache.Find(0x2000,mixed,layout);CHECK(plan.operations && plan.draws==2);
  CHECK(std::get<NativeDraw>((*plan.operations)[1]).stride==2);
  CHECK(std::get<NativeDraw>((*plan.operations)[3]).stride==12);
  // A malformed suffix cannot execute an earlier PE token or expose a prefix.
  for(auto suffix:{0xfe,0x40,0x20,0x10,0x90}){
    auto bad=mixed;bad.push_back(suffix);auto declined=cache.Find(0x3000,bad,layout);
    CHECK(!declined.operations && !declined.source.empty());
    CHECK(cache.Find(0x3000,bad,layout).cached);
  }
  std::vector<std::uint8_t> empty{0x90,0,0};CHECK(cache.Find(0x4000,empty,layout).operations);
  // Size changes and LRU eviction must preserve admission guarantees.
  auto truncated=mixed;truncated.pop_back();CHECK(!cache.Find(0x2000,truncated,layout).operations);
  for(unsigned i=0;i<20;++i)CHECK(cache.Find(0x5000+32*i,mixed,layout).operations);
  CHECK(cache.Find(0x2000,mixed,layout).operations);
  CHECK(cache.stats.hits>0 && cache.stats.changed>0 && cache.stats.reusedDraws>0);
  cache.Reset();CHECK(cache.stats.hits==0 && cache.Entries()==0);
  // A stage calls hundreds of lists per frame. All of them stay resident:
  // the second pass hits every list (exact bytes and entry layout again).
  for(unsigned pass=0;pass<2;++pass)
    for(unsigned i=0;i<300;++i){
      auto list=mixed;list.back()=std::uint8_t(i); // distinct vertex payload
      const auto planned=cache.Find(0x100000+0x100*i,list,layout);
      CHECK(planned.operations && planned.cached==(pass==1));
    }
  CHECK(cache.stats.hits==300 && cache.stats.misses==300 && cache.Entries()==300);
  // The byte budget evicts the least recently used list first.
  std::vector<std::uint8_t> large;
  for(unsigned i=0;i<150;++i){large.insert(large.end(),{0x90,0,30});large.resize(large.size()+30*12,7);}
  const auto first=cache.stats.misses;
  for(unsigned i=0;i<1024;++i)CHECK(cache.Find(0x400000+0x10000*i,large,layout).operations);
  CHECK(cache.stats.misses==first+1024 && cache.Entries()<1324);
  CHECK(!cache.Find(0x100000,mixed,layout).cached); // oldest list was evicted
  CHECK(cache.Find(0x400000+0x10000*1023,large,layout).cached);
  cache.Reset();CHECK(cache.Entries()==0);
  if(argc>1 && std::string_view(argv[1])=="--bench"){
    std::vector<std::uint8_t> geometry;
    for(unsigned i=0;i<64;++i){geometry.insert(geometry.end(),{0x90,0,30});geometry.resize(geometry.size()+30*12,42);}
    CHECK(cache.Find(0x1000,geometry,layout).operations);
    constexpr unsigned iterations=10000;
    std::printf("{\"kind\":\"primitive list framing only\",\"bytes\":%zu,\"draws\":64,\"iterations\":%u,\"trials\":[",geometry.size(),iterations);
    for(unsigned trial=0;trial<5;++trial)for(unsigned step=0;step<2;++step){
      const bool cached=(step^(trial&1))!=0;std::uint64_t checksum=0;
      const auto cpu=std::clock();const auto start=std::chrono::steady_clock::now();
      for(unsigned i=0;i<iterations;++i){const auto plan=cache.Find(cached?0x1000:0x2000+32*i,geometry,layout);CHECK(plan.operations);checksum+=plan.draws;}
      CHECK(checksum==64u*iterations);
      std::printf("%s{\"cached\":%s,\"cpu_seconds\":%.6f,\"wall_seconds\":%.6f}",trial||step?",":"",cached?"true":"false",double(std::clock()-cpu)/CLOCKS_PER_SEC,std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count());
    }
    std::puts("]}");return 0;
  }
  {
    // A list rebuilt with new bytes at the same address on every call
    // (EA DMA packets) stops paying copy+translate; stable lists resume.
    NativeDrawListCache churn;
    auto volatile_list=mixed;
    unsigned declined=0;
    for(unsigned i=0;i<256+100;++i){
      volatile_list.back()=std::uint8_t(i);
      const auto plan=churn.Find(0x7000,volatile_list,layout);
      if(!plan.operations && plan.source.empty())++declined;
    }
    CHECK(churn.governor.episodes==1 && declined==100 && churn.governor.bypassed==100);
    for(unsigned i=0;i<NativeListMissGovernor::BypassLookups;++i)churn.Find(0x7000,volatile_list,layout);
    CHECK(churn.Find(0x7000,volatile_list,layout).operations);
    CHECK(churn.Find(0x7000,volatile_list,layout).cached);
  }
  std::puts("Native draw IR: topology, entry layout, local state transitions, immutable source, mutation, malformed suffix, empty draws, 300 resident lists, budget eviction and churn governor passed");
}
