#include "../../runtime/native/native_counter_poll.h"
#include <cstdlib>
#include <iostream>
#include <map>
#define check(x) do {if(!(x)){std::cerr << "failed " << __LINE__ << ": " << #x << '\n';std::abort();}} while(false)
using GekkoAOT::Native::NativeCounterPoll;
int main() {
  unsigned cases=0;
  std::map<std::uint32_t,std::uint32_t> code;
  auto read=[&](auto pc,auto* word){auto it=code.find(pc);if(it==code.end())return false;*word=it->second;return true;};
  for(auto spr:{953u,954u,957u,958u}) for(unsigned baseline=0;baseline<32;++baseline) {
    if(baseline==3) continue;
    code={{0x100,0x7c6002a6u|((spr&31u)<<16u)|((spr&0x3e0u)<<6u)},
          {0x104,0x4e800020u},{0x200,0x4bffff01u},{0x204,0x800d83b0u},
          {0x208,0x7c601850u|(baseline<<16u)},{0x20c,0x7c030000u},{0x210,0x4081fff0u}};
    auto plan=NativeCounterPoll::Decode(0x100,0x204,read);check(plan);
    check(NativeCounterPoll::DecodeBoundary(0x100,0x204,read));
    check(NativeCounterPoll::DecodeBoundary(0x200,0xdeadbeef,read));
    check(plan->spr==spr && plan->baseline==baseline && plan->offset==-31824);
    for(auto pc:{0x100u,0x104u,0x200u,0x204u,0x208u,0x20cu,0x210u}) {
      const auto original=code[pc];
      code[pc]=0;
      check(!NativeCounterPoll::Decode(0x100,0x204,read));code[pc]=original;
      ++cases;
    }
  }
  for(std::uint32_t base:{0u,1u,0xffffff00u}) for(std::uint32_t elapsed:{0u,1u,100u,0x7fffffffu,0x80000000u})
    for(std::uint32_t limit:{0u,1u,101u,0x7fffffffu,0xffffffffu}) {
      const auto cycles=NativeCounterPoll::Cycles(base+elapsed,base,limit,1,false);
      const bool admitted=limit<=0x7fffffffu && elapsed<=0x7fffffffu && elapsed<=limit;
      check(bool(cycles)==admitted);
      if(cycles) check(*cycles==std::uint64_t(limit-elapsed)+1u);
      check(!NativeCounterPoll::Cycles(base+elapsed,base,limit,0,false));
      check(!NativeCounterPoll::Cycles(base+elapsed,base,limit,3,false));
      check(!NativeCounterPoll::Cycles(base+elapsed,base,limit,1,true));
      ++cases;
    }
  std::cout << "native counter wait PASS cases=" << cases << " code=exact cycle_deadline=wrap-safe unsupported_events=fallback\n";
}
