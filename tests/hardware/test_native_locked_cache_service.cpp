// SPDX-License-Identifier: GPL-3.0-or-later
#include "native/runtime.h"
#include "hw/lc/native_lc.h"
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <memory>
#define CHECK(x) do { if(!(x)){std::cerr<<__LINE__<<": " #x "\n";std::exit(1);} } while(false)
namespace GekkoAOT::Native {
struct HardwareTestAccess {
  static void Enable(HostRuntime& r,bool enabled){r.native_locked_cache_enabled_=enabled;}
  static auto& Lc(HostRuntime& r){return r.lc_;}
  static auto& Memory(HostRuntime& r){return r.memory_;}
};
}
using namespace GekkoAOT::Native;
int main(){
  auto a=std::make_unique<HostRuntime>(),b=std::make_unique<HostRuntime>();
  const auto& service=HostRuntime::NativeLockedCacheServices();
  CHECK(service.version==1&&service.size==sizeof(service)&&service.get_base);
  CPUState unowned{};CHECK(!service.get_base(nullptr)&&!service.get_base(&unowned));
  auto* first=service.get_base(&a->Cpu());auto* second=service.get_base(&b->Cpu());
  CHECK(first&&second&&first!=second);
  for(auto* r:{a.get(),b.get()}){
    auto& cpu=r->Cpu();CPUState before;std::memcpy(&before,&cpu,sizeof(cpu));
    for(unsigned i=0;i<65536;++i)CHECK(service.get_base(&cpu)==(r==a.get()?first:second));
    CHECK(std::memcmp(&before,&cpu,sizeof(cpu))==0);
    auto& lc=HardwareTestAccess::Lc(*r);auto& memory=HardwareTestAccess::Memory(*r);
    auto* base=service.get_base(&cpu);
    CHECK(lc.Pointer(0xe0000000u,0x40000u)==base);
    CHECK(!lc.Pointer(0xe003ffffu,2)&&!lc.Pointer(0xdfffffffu,1));
    base[0]=0x12;base[1]=0x34;base[0x3ffff]=0x56;
    std::uint64_t v=0;CHECK(lc.Read(0xe0000000u,2,&v)&&v==0x1234u);
    CHECK(lc.Read(0xe003ffffu,1,&v)&&v==0x56u);
    CHECK(lc.Write(0xe0000002u,0x789abcdeu,4));
    CHECK(base[2]==0x78&&base[5]==0xde);
    // DMA and AOT share one physical host buffer, not a copied cache.
    auto* ram=static_cast<unsigned char*>(memory.Resolve(0x80004000u,128));CHECK(ram);
    for(unsigned j=0;j<128;++j)ram[j]=static_cast<unsigned char>(j*7u);
    CHECK(lc.MemoryToLockedCache(memory,0xe0000120u,0x80004000u,4));
    CHECK(std::memcmp(base+0x120,ram,128)==0);
    for(unsigned j=0;j<128;++j)base[0x120+j]^=0x5a;
    CHECK(lc.LockedCacheToMemory(memory,0x80004000u,0xe0000120u,4));
    CHECK(std::memcmp(base+0x120,ram,128)==0);
    memory.SetPagedVmem(true);CHECK(service.get_base(&cpu)==base);
    memory.SetPagedVmem(false);CHECK(service.get_base(&cpu)==base);
    HardwareTestAccess::Enable(*r,false);CHECK(!service.get_base(&cpu));
    CHECK(lc.Read(0xe0000000u,2,&v)&&v==0x1234u); // fallback shares storage
    HardwareTestAccess::Enable(*r,true);CHECK(service.get_base(&cpu)==base);
    r->Reset();CHECK(service.get_base(&r->Cpu())==base);
    CHECK(lc.Read(0xe0000000u,2,&v)&&v==0u);
  }
  std::cout<<"Locked cache: 131072 effect-free owner queries, full-width bounds, DMA coherence, paging, disable and reset passed\n";
}
