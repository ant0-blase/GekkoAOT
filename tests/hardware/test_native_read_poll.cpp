#include "../../runtime/native/native_read_poll.h"
#include <cstdlib>
#include <iostream>
#include <map>
#define check(x) do {if(!(x)){std::cerr << "failed " << __LINE__ << ": " << #x << '\n';std::abort();}} while(false)
using GekkoAOT::Native::NativeReadPoll;
int main() {
  std::map<std::uint32_t,std::uint32_t> code;
  auto read=[&](auto pc,auto* word) {auto it=code.find(pc);if(it==code.end())return false;*word=it->second;return true;};
  std::array<std::uint32_t,32> regs{};
  regs[13]=0x80001000;
  auto ram=[&](auto ea,auto* word) {if(ea!=regs[13]+4)return false;*word=0u;return true;};
  code={{0x100,0x800d0004},{0x104,0x2c000000},{0x108,0x4182fff8}};
  for(auto pc:{0x100u,0x104u,0x108u}) {
    auto plan=NativeReadPoll::Decode(pc,read);check(plan);
    check(plan->Stable(pc,regs,0x20000000u,0u,ram));
    auto changed=regs;changed[0]=1;check(!plan->Stable(pc,changed,0x20000000u,0u,ram));
    auto nonzero=[&](auto ea,auto* word){if(!ram(ea,word))return false;*word=1;return true;};
    check(!plan->Stable(pc,regs,0x20000000u,0u,nonzero));
    check(!plan->Stable(pc,regs,0x20000000u,0u,[](auto,auto*){return false;}));
  }
  // Conditional guard skips a one-shot yield call. Only its pure path parks.
  code={{0x100,0x2c070000},{0x104,0x4082000c},{0x108,0x48000101},
        {0x10c,0x38e00001},{0x110,0x800d0004},{0x114,0x2c000000},{0x118,0x4182ffe8}};
  regs[7]=1;
  auto plan=NativeReadPoll::Decode(0x100,read);check(plan);
  check(plan->Stable(0x100,regs,0x20000000u,0u,ram));
  regs[7]=0;check(!plan->Stable(0x100,regs,0x20000000u,0u,ram));
  // Finite CTR loops, linked branches, stores and arithmetic are excluded.
  code={{0x100,0x800d0004},{0x104,0x2c000000},{0x108,0x4200fff8}};
  check(!NativeReadPoll::Decode(0x100,read));
  code[0x108]=0x4182fff9;check(!NativeReadPoll::Decode(0x100,read));
  code[0x108]=0x4182fff8;
  for(auto bad:{0x900d0004u,0x38000000u,0x7c0002a6u,0x60000001u}) {
    code[0x100]=bad;plan=NativeReadPoll::Decode(0x100,read);
    check(!plan || !plan->Stable(0x100,regs,0x20000000u,0u,ram));
  }
  // Signed/unsigned compares, all CR fields, SO and both branch polarities.
  unsigned cases=0;
  for(unsigned crfield=0;crfield<8;++crfield) for(unsigned logical=0;logical<2;++logical)
    for(auto value:{0u,1u,0x7fffffffu,0x80000000u,0xffffffffu}) for(unsigned so=0;so<2;++so) {
      const auto compare=(31u<<26u)|(crfield<<23u)|(3u<<16u)|(4u<<11u)|(logical?64u:0u);
      const bool less=logical ? value<1u : static_cast<std::int32_t>(value)<1;
      const auto nibble=(less?8u:value==1u?2u:4u)|so;
      regs[3]=value;regs[4]=1;regs[0]=0;
      const unsigned bit=4*crfield+(less?0:value==1?2:1);
      code={{0x100,0x800d0004},{0x104,compare},{0x108,0x41800000u|(bit<<16u)|0xfff8u}};
      plan=NativeReadPoll::Decode(0x100,read);check(plan);
      check(plan->Stable(0x100,regs,nibble<<(28-4*crfield),so<<31u,ram));
      ++cases;
    }
  std::cout << "native RAM poll PASS cases=" << cases << " irq=caller-owned state=unchanged writes+calls+CTR+MMIO=fallback\n";
}
