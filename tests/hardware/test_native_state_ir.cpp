// SPDX-License-Identifier: GPL-3.0-or-later
#include "../../runtime/gx/native_display_list_cache.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <new>
#include <string_view>
#define CHECK(x) do{if(!(x)){std::fprintf(stderr,"%d: %s\n",__LINE__,#x);std::exit(1);}}while(false)
using namespace GekkoAOT::GX;
static bool track=false;static unsigned allocations=0;
void* operator new(std::size_t n){if(track)++allocations;if(auto* p=std::malloc(n?n:1))return p;throw std::bad_alloc();}
void operator delete(void* p)noexcept{std::free(p);}void operator delete(void* p,std::size_t)noexcept{std::free(p);}
static void Word(std::vector<std::uint8_t>& bytes,unsigned value){for(int s=24;s>=0;s-=8)bytes.push_back(value>>s);}
static void Cp(std::vector<std::uint8_t>& bytes,unsigned reg,unsigned value){bytes.push_back(8);bytes.push_back(reg);Word(bytes,value);}
static void Bp(std::vector<std::uint8_t>& bytes,unsigned reg,unsigned value){bytes.push_back(0x61);Word(bytes,(reg<<24)|(value&0xffffff));}
static void Xf(std::vector<std::uint8_t>& bytes,unsigned reg,std::initializer_list<unsigned> values){
  bytes.push_back(0x10);Word(bytes,((values.size()-1)<<16)|reg);for(auto value:values)Word(bytes,value);
}
int main(int argc,char** argv){
  NativeOperation operation;
  unsigned admitted=0;
  for(unsigned reg=0;reg<256;++reg){
    const bool valid=reg==0x30||reg==0x40||reg==0x50||reg==0x60||
        (reg>=0x70&&reg<=0x77)||(reg>=0x80&&reg<=0x87)||(reg>=0x90&&reg<=0x97)||(reg>=0xa0&&reg<=0xbf);
    CHECK(LowerNativeCp(reg,0xfedcba98,operation)==valid);
    if(valid){CHECK(ValidNativeOperation(operation));++admitted;
      if(const auto* v=std::get_if<NativeVertexState>(&operation))CHECK(NativeVertexRegister(*v)==reg);}
  }
  CHECK(admitted==60);
  std::vector<std::uint8_t> list;
  Cp(list,0x50,0x12345678);Cp(list,0x73,0x4d00);
  // Mask is not folded into the following state: the runtime must consume it
  // against its CURRENT BP shadow on every invocation, including cache hits.
  Bp(list,0xfe,0x0000ff);Bp(list,0x40,0x001237);Bp(list,0x48,0xabcd);list.push_back(0);
  NativeDisplayListCache cache;auto plan=cache.Find(0x1000,list);
  CHECK(plan.operations&&plan.operations->size()==5&&plan.commands==6&&!plan.cached);
  const auto& mask=std::get<NativePixelState>((*plan.operations)[2]);CHECK(mask.domain==NativePixelDomain::WriteMask&&mask.value==255);
  const auto& token=std::get<NativePixelState>((*plan.operations)[4]);CHECK(token.domain==NativePixelDomain::TokenInterrupt&&token.value==0xabcd);
  track=true;allocations=0;
  for(unsigned i=0;i<100;++i){auto hit=cache.Find(0x1000,list);CHECK(hit.operations==plan.operations&&hit.cached);}
  track=false;CHECK(allocations==0&&cache.stats.hits==100);
  list[5]^=1;CHECK(cache.Find(0x1000,list).operations&&cache.stats.changed==1);
  // Never publish a translated prefix if a later draw, nested list, malformed
  // command or unsupported XF block requires state-dependent compatibility.
  for(auto suffix:std::vector<std::uint8_t>{0x80,0x40,0xff,0x10}){
    auto bad=list;bad.push_back(suffix);CHECK(!cache.Find(0x2000,bad).operations);
    CHECK(!cache.Find(0x2000,bad).operations);
  }
  CHECK(cache.stats.negative_hits==4);
  // Primitive lists still use state-dependent decoding, but the bytes exposed
  // to that decoder must survive a CPU rewrite of their source allocation.
  auto primitive=list;primitive.push_back(0x80);
  const auto expected=primitive;
  auto deferred=cache.Find(0x2400,primitive);
  CHECK(!deferred.operations && !deferred.source.empty());
  primitive.front()=0xfe;
  CHECK(std::equal(deferred.source.begin(),deferred.source.end(),expected.begin(),expected.end()));
  deferred=cache.Find(0x2400,primitive);
  CHECK(!deferred.operations && deferred.source.front()==0xfe && !deferred.cached);
  auto unchanged=cache.Find(0x2400,primitive);
  CHECK(unchanged.cached && unchanged.source.data()==deferred.source.data());
  for(unsigned i=0;i<20;++i)CHECK(cache.Find(0x3000+i*128,list).operations);
  CHECK(cache.Find(0x1000,list).operations);cache.Reset();CHECK(cache.stats.hits==0);
  std::vector<std::uint8_t> words;
  for(unsigned i=0;i<7;++i)Word(words,i==6?0:0x7fc12345);
  CHECK(LowerNativeXf(0x1020,7,words,operation)&&std::holds_alternative<NativeProjection>(operation));
  CHECK(std::get<NativeProjection>(operation).values[0]==0x7fc12345);
  words.back()=2;CHECK(!LowerNativeXf(0x1020,7,words,operation));
  words.resize(24);CHECK(LowerNativeXf(0x101a,6,words,operation)&&std::holds_alternative<NativeViewport>(operation));
  CHECK(!LowerNativeXf(0x101b,6,words,operation));
  words.resize(64);CHECK(LowerNativeXf(0x600,16,words,operation)&&ValidNativeOperation(operation));
  CHECK(!LowerNativeXf(0x601,16,words,operation));CHECK(!LowerNativeXf(0x1050,16,words,operation));
  CHECK(!ValidNativeOperation(NativeVertexState{NativeVertexStateKind::DescriptorLow,1,0}));
  CHECK(!ValidNativeOperation(NativePixelState{NativePixelDomain::Copy,0xfe,255}));
  CHECK(!ValidNativeOperation(NativePixelState{NativePixelDomain::WriteMask,0xfe,0x1000000}));
  NativePixelState pixel;
  CHECK(LowerNativeBp(0xf3,0,pixel)&&pixel.domain==NativePixelDomain::DepthBlendAlpha);
  CHECK(LowerNativeBp(0xf5,0,pixel)&&pixel.domain==NativePixelDomain::Geometry);
  CHECK(LowerNativeBp(0x06,0,pixel)&&pixel.domain==NativePixelDomain::Tev);
  // Exercise adjacent variant transitions in a complete mixed list. All
  // block types must be cached together, not just a BP copy/token suffix.
  std::vector<std::uint8_t> mixed;
  Cp(mixed,0x50,512);
  Xf(mixed,0x1020,{0x3f800000,0,0x3f800000,0,0x3f800000,0,1});
  Xf(mixed,0x103f,{1});Xf(mixed,0x1040,{0x280});
  Xf(mixed,0x101a,{0x42000000,0xc2000000,0x4b7fffff,0x43bb0000,0x43bb0000,0x4b7fffff});
  Xf(mixed,0x600,{0,0,0xff112233,0x3f800000});Bp(mixed,0x48,0x1234);
  auto mixed_plan=cache.Find(0x40000,mixed);
  CHECK(mixed_plan.operations&&mixed_plan.operations->size()==7&&!mixed_plan.cached);
  CHECK(cache.Find(0x40000,mixed).cached);
  CHECK(std::get<NativeTransformState>((*mixed_plan.operations)[2]).address==0x103f);
  // Every admitted ordinary XF block preserves its address/count/scalars,
  // including single-word channel/texgen writes following a projection.
  for(unsigned address=0x1000;address<=0x1057;++address)for(unsigned count=1;count<=16;++count){
    if(!NativeTransformStateRange(address,count))continue;
    words.clear();for(unsigned i=0;i<count;++i)Word(words,0x7fc01234u+i);
    CHECK(LowerNativeXf(address,count,words,operation)&&ValidNativeOperation(operation));
    const auto& state=std::get<NativeTransformState>(operation);
    CHECK(state.address==address&&state.words==count);
    for(unsigned i=0;i<count;++i)CHECK(state.values[i]==0x7fc01234u+i);
  }
  std::vector<std::uint8_t> oversized;
  for(unsigned i=0;i<=NativeDisplayListCache::MaxOperations;++i)Bp(oversized,0x40,0);
  CHECK(!cache.Find(0x80000,oversized).operations);
  if(argc<2 || std::string_view(argv[1])!="--bench"){
    std::puts("Extended native state: CP/VAT/VCD, projection/viewport/lights, BP effects, exact display-list mutation, eviction, negative cache and allocation-free hits passed");return 0;
  }
  list.clear();for(unsigned i=0;i<512;++i){Cp(list,0x70+(i&7),i*7);Bp(list,0xc0+(i&31),i*13);}
  constexpr unsigned iterations=20000;std::vector<NativeOperation> decoded;unsigned commands=0;
  CHECK(NativeDisplayListCache::Translate(list,decoded,commands));CHECK(cache.Find(0x1000,list).operations);
  std::printf("{\"kind\":\"state-list translation microbenchmark\",\"bytes\":%zu,\"operations\":%u,\"iterations\":%u,\"trials\":[",list.size(),commands,iterations);
  for(unsigned trial=0;trial<5;++trial)for(unsigned step=0;step<2;++step){
    const bool cached=(step^(trial&1))!=0;std::uint64_t checksum=0;allocations=0;track=true;
    const auto cpu=std::clock();const auto begin=std::chrono::steady_clock::now();
    for(unsigned i=0;i<iterations;++i){
      if(cached){const auto p=cache.Find(0x1000,list);CHECK(p.operations);checksum+=p.operations->size();}
      else{CHECK(NativeDisplayListCache::Translate(list,decoded,commands));checksum+=decoded.size();}
    }
    const double wall=std::chrono::duration<double>(std::chrono::steady_clock::now()-begin).count();
    const double elapsed_cpu=double(std::clock()-cpu)/CLOCKS_PER_SEC;track=false;
    CHECK(checksum==std::uint64_t(iterations)*1024&&allocations==0);
    std::printf("%s{\"cached\":%s,\"cpu_seconds\":%.6f,\"wall_seconds\":%.6f,\"allocations\":%u}",trial||step?",":"",cached?"true":"false",elapsed_cpu,wall,allocations);
  }
  std::puts("]}");
}
