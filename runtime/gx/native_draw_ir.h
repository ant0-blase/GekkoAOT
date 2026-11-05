// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "native_display_list_cache.h"
#include <bitset>

namespace GekkoAOT::GX {
// Only framing depends on this state. Matrices, array bindings, textures and
// pipeline state remain ordered operations/live bindings at execution time.
// Never retain a host pointer to guest arrays in a compiled list.
struct NativeDrawLayout {
  std::array<std::uint32_t,26> words{};
  std::bitset<26> valid;
  bool operator==(const NativeDrawLayout&) const = default;
  static int Index(unsigned reg) noexcept {
    if(reg==0x50)return 0;
    if(reg==0x60)return 1;
    if(reg>=0x70 && reg<=0x77)return 2+(reg&7)*3;
    if(reg>=0x80 && reg<=0x87)return 3+(reg&7)*3;
    if(reg>=0x90 && reg<=0x97)return 4+(reg&7)*3;
    return -1;
  }
  void Write(unsigned reg,std::uint32_t value) noexcept {
    if(const auto i=Index(reg);i>=0){words[i]=value;valid.set(i);}
  }
  unsigned Stride(unsigned format) const noexcept {
    if(format>=8 || !valid[0] || !valid[1])return 0;
    const unsigned base=2+3*format;
    if(!valid[base])return 0;
    // VAT B/C do not participate in framing when their texcoords are absent
    // or indexed. Retail lists often never initialize those unused banks.
    const auto descriptor=[&](unsigned tex){return (words[1]>>(tex*2))&3u;};
    bool needB=false,needC=false;
    for(unsigned tex=1;tex<=4;++tex)needB|=descriptor(tex)==1;
    for(unsigned tex=5;tex<8;++tex)needC|=descriptor(tex)==1;
    if((needB && !valid[base+1]) || (needC && !valid[base+2]))return 0;
    const auto lo=words[0],hi=words[1],a=words[base],b=words[base+1],c=words[base+2];
    const auto bits=[](unsigned w,unsigned shift,unsigned n){return (w>>shift)&((1u<<n)-1);};
    const auto component=[](unsigned f){return f<2?1u:f<4?2u:4u;};
    const auto attr=[](unsigned descriptor,unsigned direct){return descriptor==0?0u:descriptor==1?direct:descriptor-1;};
    unsigned size=0;for(auto m=lo&511;m;m>>=1)size+=m&1;
    size+=attr(bits(lo,9,2),component(bits(a,1,3))*(bits(a,0,1)+2));
    const auto normal=bits(lo,11,2);const bool nbt=bits(a,9,1),indices3=bits(a,31,1);
    size+=normal==1?component(bits(a,10,3))*(nbt?9:3):
          (normal>=2?(normal-1)*(nbt&&indices3?3:1):0);
    constexpr unsigned colors[]{2,3,4,2,3,4,0,0};
    for(unsigned i=0;i<2;++i)size+=attr(bits(lo,13+2*i,2),colors[bits(a,14+4*i,3)]);
    const unsigned formats[]{bits(a,22,3),bits(b,1,3),bits(b,10,3),bits(b,19,3),bits(b,28,3),bits(c,6,3),bits(c,15,3),bits(c,24,3)};
    const unsigned elements[]{bits(a,21,1),bits(b,0,1),bits(b,9,1),bits(b,18,1),bits(b,27,1),bits(c,5,1),bits(c,14,1),bits(c,23,1)};
    for(unsigned i=0;i<8;++i)size+=attr(bits(hi,2*i,2),component(formats[i])*(elements[i]+1));
    return size;
  }
};
enum class NativeTopology : std::uint8_t {
  Quads=0x80, Quads2=0x88, Triangles=0x90, TriangleStrip=0x98,
  TriangleFan=0xa0, Lines=0xa8, LineStrip=0xb0, Points=0xb8
};
struct NativeDraw {
  NativeTopology topology{};
  std::uint8_t format=0;
  std::uint16_t count=0,stride=0;
  // Range in the plan's owned, immutable byte allocation, including the
  // three-byte original header for compatibility diagnostics only.
  std::uint32_t offset=0,bytes=0;
};
using NativeListOperation=std::variant<NativeOperation,NativeDraw>;

// Runs of back-to-back small draws inside a cached list, merged once
// per plan by the renderer (same algorithm as its FIFO draw runs). `bytes`
// holds each merged command (3-byte header + vertices); a run replaces plan
// operations [first, end) when the renderer state allows merging.
struct NativeDrawRun {std::uint32_t first=0,end=0;NativeDraw merged{};};
struct NativeDrawRuns {
  bool built=false;
  std::vector<std::uint8_t> bytes;
  std::vector<NativeDrawRun> runs;
  // Replay-potential census (GEKKOAOT_GX_PHASE_PROFILE): incoming GX state
  // signature of the previous execution.
  std::uint64_t last_sigma=0;
  std::uint64_t last_parts[5]{};
};

// Exact source and entry-layout validation, including writes without a host
// invalidation callback. Indexed arrays are deliberately revalidated by the
// draw sink on EVERY invocation: a geometry plan is not a geometry snapshot.
class NativeDrawListCache {
public:
  struct Stats {std::uint64_t hits=0,misses=0,changed=0,declined=0,draws=0,reusedDraws=0;} stats;
  struct Plan {
    const std::vector<NativeListOperation>* operations=nullptr;
    std::span<const std::uint8_t> source{};
    std::uint32_t commands=0,draws=0;
    bool cached=false;
    NativeDrawRuns* runs=nullptr;  // owned by the cache entry; reset with it
  };
  void Reset(){lru_.Reset();stats={};governor.Reset();}
  void SetCapacity(std::size_t entries){lru_.SetCapacity(entries);}
  std::size_t Entries() const noexcept { return lru_.Size(); }
  NativeListMissGovernor governor;
  Plan Find(std::uint32_t address,std::span<const std::uint8_t> source,const NativeDrawLayout& entry){
    if(source.empty() || source.size()>NativeDisplayListCache::MaxBytes){++stats.declined;return {};}
    if(governor.Bypass())return {};
    if(auto* e=lru_.Find(address)){
      if(e->layout==entry && e->bytes.size()==source.size() && std::memcmp(e->bytes.data(),source.data(),source.size())==0){
        governor.Note(false);
        ++stats.hits;if(e->compiled)stats.reusedDraws+=e->draws;
        return {e->compiled?&e->operations:nullptr,e->bytes,e->commands,e->draws,true,&e->runs};
      }
      ++stats.changed;governor.Note(true);
    }else governor.Note(false);
    ++stats.misses;auto& slot=lru_.Acquire(address);slot.layout=entry;
    slot.bytes.assign(source.begin(),source.end());slot.operations.clear();slot.commands=slot.draws=0;
    slot.runs=NativeDrawRuns{};
    slot.compiled=Compile(slot);
    if(!slot.compiled){slot.operations.clear();++stats.declined;}
    else stats.draws+=slot.draws;
    lru_.Commit(slot,sizeof(Entry)+slot.bytes.size()+slot.operations.size()*sizeof(NativeListOperation));
    return {slot.compiled?&slot.operations:nullptr,slot.bytes,slot.commands,slot.draws,false,&slot.runs};
  }
private:
  struct Entry {
    std::vector<std::uint8_t> bytes;
    std::vector<NativeListOperation> operations;
    NativeDrawRuns runs;
    NativeDrawLayout layout;
    std::uint32_t address=0,commands=0,draws=0;
    std::size_t cost=0;
    bool compiled=false;
  };
  NativeListLru<Entry> lru_;
  static std::uint32_t Word(const std::uint8_t* p){return std::uint32_t(p[0])<<24|std::uint32_t(p[1])<<16|std::uint32_t(p[2])<<8|p[3];}
  static bool Compile(Entry& e){
    auto layout=e.layout;
    for(std::size_t offset=0;offset<e.bytes.size();){
      const auto* p=e.bytes.data()+offset;const auto remaining=e.bytes.size()-offset;const auto op=p[0]&0xf8u;
      if(e.operations.size()==NativeDisplayListCache::MaxOperations)return false;
      if(op==0){++offset;++e.commands;continue;}
      std::size_t length=0;NativeOperation state;
      if(op>=0x80 && op<=0xb8){
        if(remaining<3)return false;
        const auto stride=layout.Stride(p[0]&7),count=(unsigned(p[1])<<8)|p[2];
        if(!stride)return false;
        length=3+std::size_t(stride)*count;if(length>remaining)return false;
        e.operations.emplace_back(NativeDraw{NativeTopology(op),std::uint8_t(p[0]&7),std::uint16_t(count),std::uint16_t(stride),std::uint32_t(offset),std::uint32_t(length)});++e.draws;
      }else{
        if(op==8 && remaining>=6){
          length=6;if(!LowerNativeCp(p[1],Word(p+2),state))return false;
          layout.Write(p[1],Word(p+2));
        }else if(op==0x10 && remaining>=5){
          const auto header=Word(p+1),words=(header>>16)+1u;length=5+4u*words;
          if(length>remaining || !LowerNativeXf(header&65535,words,{p+5,length-5},state))return false;
        }else if(op==0x60 && remaining>=5){
          length=5;NativePixelState pixel;const auto word=Word(p+1);
          if(!LowerNativeBp(word>>24,word,pixel))return false;state=pixel;
        }else return false;
        if(!ValidNativeOperation(state))return false;
        e.operations.emplace_back(state);
      }
      offset+=length;++e.commands;
    }
    return e.draws!=0;
  }
};
} // namespace GekkoAOT::GX
