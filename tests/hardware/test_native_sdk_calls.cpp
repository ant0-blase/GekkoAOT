// SPDX-License-Identifier: GPL-3.0-or-later
// Whole utility-family ABI and side-effect parity across the old token route
// and generated native bindings. No game-specific addresses or renderer needed.
#include "native/runtime.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#define CHECK(x) do{if(!(x)){std::fprintf(stderr,"%u: %s\n",__LINE__,#x);std::exit(1);}}while(false)
namespace GekkoAOT::Native {
struct HardwareTestAccess {
 static void Track(HostRuntime& r){r.track_host_effects_=true;}
 static auto Effects(const HostRuntime& r){return r.host_effects_;}
};
}
using namespace GekkoAOT::Native;
int main(){
  auto legacy=std::make_unique<HostRuntime>(),named=std::make_unique<HostRuntime>();
  constexpr unsigned dst=0x80010000,src=0x80011000,needle=0x80012000,entry=0x80080000;
  using K=SimpleHleKind;
  struct Case {K kind;unsigned a,b,c;};
  const Case cases[]={
   {K::OsGetTime,0,0,0},{K::OsGetTick,0,0,0},
   {K::Memcpy,dst,src,32},{K::Memmove,dst,src,32},{K::Memset,dst,0x5a,32},
   {K::Memcmp,dst,src,32},{K::Bzero,dst,32,0},{K::Strlen,src,0,0},
   {K::Strcmp,dst,src,0},{K::Strncmp,dst,src,4},{K::Strcpy,dst,src,0},
   {K::Strncpy,dst,src,12},{K::Strcat,dst,src,0},{K::Strchr,src,'e',0},
   {K::Strrchr,src,'t',0},{K::Memchr,src,'t',32},{K::Strstr,src,needle,0},
   {K::Strncat,dst,src,3},{K::Strnlen,src,4,0},{K::Strcspn,src,needle,0},{K::Strpbrk,src,needle,0}};
  for(const auto& test:cases){
    for(auto* r:{legacy.get(),named.get()}){
      r->Reset();HardwareTestAccess::Track(*r);
      auto& c=r->Cpu();for(unsigned i=0;i<32;++i)c.gpr[i]=0x12340000+i;
      c.gpr[3]=test.a;c.gpr[4]=test.b;c.gpr[5]=test.c;c.pc=entry;c.lr=0x80090000;c.timebase=0x123456789abcdefull;
      CHECK(r->Memory().Write32(entry,0x4e800020));r->RegisterSimpleHle(test.kind,entry);
      std::memset(r->Memory().Resolve(dst,0x3000),0x43,0x3000);
      std::memcpy(r->Memory().Resolve(dst,6),"alpha",6);
      std::memcpy(r->Memory().Resolve(src,7),"letter",7);
      std::memcpy(r->Memory().Resolve(needle,3),"et",3);
    }
    const auto before_legacy=HardwareTestAccess::Effects(*legacy),before_named=HardwareTestAccess::Effects(*named);
    auto& a=legacy->Cpu();auto& b=named->Cpu();
    CHECK(a.host_call(&a,0xfffc0000u|unsigned(test.kind)));
    CHECK(HostRuntime::NativeSdkServices().entries[unsigned(test.kind)](&b));
    CHECK(!std::memcmp(a.gpr,b.gpr,sizeof a.gpr));
    CHECK(!std::memcmp(a.fpr,b.fpr,sizeof a.fpr));CHECK(!std::memcmp(a.ps1,b.ps1,sizeof a.ps1));
    CHECK(a.pc==b.pc && a.lr==b.lr && a.cr==b.cr && a.xer==b.xer && a.msr==b.msr && a.fpscr==b.fpscr);
    CHECK(a.external_write_count==b.external_write_count);
    CHECK(HardwareTestAccess::Effects(*legacy)-before_legacy==HardwareTestAccess::Effects(*named)-before_named);
    CHECK(!std::memcmp(legacy->Memory().Resolve(dst,0x3000),named->Memory().Resolve(dst,0x3000),0x3000));
    std::printf("NativeSDK utility parity kind=%u passed\n",unsigned(test.kind));
  }
  std::puts("21 SDK utility/time services: GPR/FPR/PS, guest memory, epochs and host-effect parity passed");
}
