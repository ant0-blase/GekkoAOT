#include "../../runtime/native/native_spin_probe.h"
#include <cstdlib>
#include <initializer_list>
#include <iostream>
#include <map>
#define check(x) do {if(!(x)){std::cerr << "failed " << __LINE__ << ": " << #x << '\n';std::abort();}} while(false)
using GekkoAOT::Native::SpinProbe;
using GekkoAOT::Native::SpinProbeState;
namespace {
struct Memory {
  std::map<std::uint32_t,std::uint32_t> words;
  std::uint32_t foreign=0;
  bool Read(std::uint32_t ea,unsigned size,std::uint32_t* value) const {
    const auto it=words.find(ea&~3u);
    if(it==words.end() || (ea&3u)+size>4u) return false;
    const unsigned shift=8u*(4u-size-(ea&3u));
    *value=size==4u ? it->second : (it->second>>shift)&((1u<<(8u*size))-1u);
    return true;
  }
  bool Fetch(std::uint32_t ea,std::uint32_t* word) const {return Read(ea,4u,word);}
  bool Foreign(std::uint32_t target) const {return target==foreign;}
};
}  // namespace
int main() {
  unsigned cases=0;
  SpinProbe probe;
  constexpr std::uint64_t kHorizon=40500000u;
  // Driven: do { t = OSGetTime(); } while ((s64)t < (s64)r26:r27);
  Memory driven;
  driven.words={{0x200,0x4bffff01u},{0x204,0x6c658000u},{0x208,0x6f438000u},{0x20c,0x7c1b2010u},
                {0x210,0x7c632910u},{0x214,0x7c652910u},{0x218,0x7c6300d0u},{0x21c,0x2c030000u},
                {0x220,0x4082ffe0u},{0x224,0x4e800020u}};
  SpinProbeState s;
  s.gpr[1]=0x8000u;
  for(std::uint64_t deadline:std::initializer_list<std::uint64_t>{0x10u,0x1234567890u,0x7fffffff00000010u,0x100000000u})
    for(std::uint64_t now:std::initializer_list<std::uint64_t>{deadline-0x30000000u,deadline-5u,deadline-1u,deadline,deadline+7u}) {
      s.gpr[26]=static_cast<std::uint32_t>(deadline>>32u);s.gpr[27]=static_cast<std::uint32_t>(deadline);
      const bool waiting=std::int64_t(now)<std::int64_t(deadline);
      const auto outcome=probe.Run(s,0x204,0x200,0x100,now,true,true,driven);
      check((outcome==SpinProbe::Outcome::Continue)==waiting);
      if(!waiting) continue;
      const auto waits=[&](std::uint64_t t) {
        return probe.Run(s,0x204,0x200,0x100,t,true,false,driven)==SpinProbe::Outcome::Continue;
      };
      const auto exit=SpinProbe::FirstExit(now,kHorizon,waits);
      check(bool(exit)==(deadline-now<=kHorizon));
      if(exit) check(*exit==deadline);
      ++cases;
    }
  // Stream sync: do { pos = elapsed(t) >> 4 (helper with a counted loop); obj->pos = pos; }
  // while (pos < obj->limit);
  Memory stream;
  stream.words={{0x200,0x4bffff01u},  // bl clock
                {0x204,0x7c7b2050u},  // subf r3,r27,r4
                {0x208,0x480000f9u},  // bl 0x300
                {0x20c,0x907f0000u},  // stw r3,0(r31)
                {0x210,0x801f0004u},  // lwz r0,4(r31)
                {0x214,0x7c030040u},  // cmplw r3,r0
                {0x218,0x4180ffe8u},  // blt 0x200
                {0x21c,0x4e800020u},  // blr
                {0x300,0x38000004u},  // li r0,4
                {0x304,0x7c0903a6u},  // mtctr r0
                {0x308,0x5463f87eu},  // srwi r3,r3,1
                {0x30c,0x4200fffcu},  // bdnz 0x308
                {0x310,0x4e800020u},  // blr
                {0x1000,0u},{0x1004,1000u}};
  SpinProbeState t;
  t.gpr[1]=0x8000u;t.gpr[31]=0x1000u;t.gpr[27]=0x5000u;t.lr=0x204u;
  for(std::uint64_t elapsed:{0u,1u,15999u,16000u,16001u}) {
    const std::uint64_t now=0x700000000ull+0x5000u+elapsed;
    const bool waiting=(elapsed>>4u)<1000u;
    check((probe.Run(t,0x204,0x200,0x100,now,true,true,stream)==SpinProbe::Outcome::Continue)==waiting);
    if(!waiting) continue;
    const auto exit=SpinProbe::FirstExit(now,kHorizon,[&](std::uint64_t v) {
      return probe.Run(t,0x204,0x200,0x100,v,true,false,stream)==SpinProbe::Outcome::Continue;
    });
    check(exit && *exit==0x700000000ull+0x5000u+16000u);
    ++cases;
  }
  // The overlay never leaks into guest RAM.
  check(stream.words[0x1000]==0u);
  // Calling an HLE entry, touching unmapped memory or another clock site ends the wait.
  stream.foreign=0x300;
  check(probe.Run(t,0x204,0x200,0x100,0x700005000ull,true,true,stream)==SpinProbe::Outcome::Exit);
  stream.foreign=0;
  t.gpr[31]=0x9000u;
  check(probe.Run(t,0x204,0x200,0x100,0x700005000ull,true,true,stream)==SpinProbe::Outcome::Exit);
  t.gpr[31]=0x1000u;
  check(probe.Run(t,0x204,0x1f0,0x100,0x700005000ull,true,true,stream)==SpinProbe::Outcome::Exit);
  // A different stack frame at the call site is a different activation.
  {
    auto moved=stream;
    moved.words[0x310]=0x38210010u;  // addi r1,r1,16 before returning
    moved.words[0x314]=0x4e800020u;  // blr
    check(probe.Run(t,0x204,0x200,0x100,0x700005000ull,true,true,moved)==SpinProbe::Outcome::Exit);
  }
  // Unsupported state (FP, MSR) ends the probe.
  for(std::uint32_t word:{0xc0030000u /* lfs */,0x7c0000a6u /* mfmsr */,0x7c0c42e6u /* mftb */,0x44000002u /* sc */}) {
    auto bad=stream;bad.words[0x20c]=word;
    check(probe.Run(t,0x204,0x200,0x100,0x700005000ull,true,true,bad)==SpinProbe::Outcome::Exit);
    ++cases;
  }
  // OSGetTick: do { t = OSGetTick(); } while (t - start < delay);
  Memory tick;
  tick.words={{0x200,0x4bffff01u},{0x204,0x7c1f1850u},{0x208,0x7c00f040u},{0x20c,0x4180fff4u},{0x210,0x4e800020u}};
  SpinProbeState k;k.gpr[1]=0x8000u;
  for(std::uint32_t start:{0u,0xfffff000u}) for(std::uint32_t delay:{1u,40500u}) for(std::uint32_t elapsed:{0u,1u,40499u}) {
    k.gpr[31]=start;k.gpr[30]=delay;
    const std::uint64_t now=0x500000000ull+start+elapsed;
    const bool waiting=elapsed<delay;
    check((probe.Run(k,0x204,0x200,0x100,now,false,true,tick)==SpinProbe::Outcome::Continue)==waiting);
    if(!waiting) continue;
    const auto exit=SpinProbe::FirstExit(now,kHorizon,[&](std::uint64_t v) {
      return probe.Run(k,0x204,0x200,0x100,v,false,false,tick)==SpinProbe::Outcome::Continue;
    });
    check(exit && *exit==now+(delay-elapsed));
    ++cases;
  }
  // Driven's second loop reads the clock in the loop and again in a helper:
  // do { t0 = OSGetTime(); } while (helper() == 2); helper() { t = OSGetTime(); ... }
  Memory nested;
  nested.words={{0x200,0x4bffff01u},{0x204,0x7c9d2378u},{0x208,0x480000f9u},{0x20c,0x2c030002u},
                {0x210,0x4182fff0u},{0x214,0x4e800020u},
                {0x300,0x7c0802a6u},{0x304,0x90010004u},{0x308,0x9421fff0u},{0x30c,0x4bfffdf5u},
                {0x310,0x7c7b2050u},{0x314,0x7c03e040u},{0x318,0x38600002u},{0x31c,0x41800008u},
                {0x320,0x38600000u},{0x324,0x38210010u},{0x328,0x80010004u},{0x32c,0x7c0803a6u},
                {0x330,0x4e800020u},{0x7ff0,0u},{0x8004,0u}};
  SpinProbeState n;n.gpr[1]=0x8000u;n.gpr[27]=0x1000u;n.gpr[28]=4050u;n.lr=0x204u;
  for(std::uint32_t elapsed:{0u,1u,4049u,4050u}) {
    const std::uint64_t now=0x300000000ull+0x1000u+elapsed;
    const bool waiting=elapsed<4050u;
    check((probe.Run(n,0x204,0x200,0x100,now,true,true,nested)==SpinProbe::Outcome::Continue)==waiting);
    if(!waiting) continue;
    const auto exit=SpinProbe::FirstExit(now,kHorizon,[&](std::uint64_t v) {
      return probe.Run(n,0x204,0x200,0x100,v,true,false,nested)==SpinProbe::Outcome::Continue;
    });
    check(exit && *exit==0x300000000ull+0x1000u+4050u);
    ++cases;
  }
  check(nested.words[0x7ff0]==0u && nested.words[0x8004]==0u);
  // Equality waits are never parked.
  Memory equal;
  equal.words={{0x200,0x4bffff01u},{0x204,0x7c03f040u},{0x208,0x4082fff8u},{0x20c,0x4e800020u}};
  SpinProbeState e;e.gpr[1]=0x8000u;e.gpr[30]=1000u;
  check(probe.Run(e,0x204,0x200,0x100,10u,false,true,equal)==SpinProbe::Outcome::Continue);
  check(!SpinProbe::FirstExit(10u,kHorizon,[&](std::uint64_t v) {
    return probe.Run(e,0x204,0x200,0x100,v,false,false,equal)==SpinProbe::Outcome::Continue;
  }));
  ++cases;
  // v346 fixed point: Nemo's frame pacing `do { now = VIGetRetraceCount();
  // g_now = now; } while (g_now - start < g_interval);` with a getter leaf.
  Memory pacing;
  pacing.words={{0x400,0x48000101u},  // bl 0x500
                {0x404,0x907e0000u},  // stw r3,0(r30)
                {0x408,0x813e0004u},  // lwz r9,4(r30)
                {0x40c,0x801e0000u},  // lwz r0,0(r30)
                {0x410,0x7c1f0050u},  // subf r0,r31,r0
                {0x414,0x7c004800u},  // cmpw r0,r9
                {0x418,0x4180ffe8u},  // blt 0x400
                {0x500,0x807e0008u},  // lwz r3,8(r30)  (retrace count)
                {0x504,0x4e800020u},  // blr
                {0x1000,10u},{0x1004,2u},{0x1008,10u}};
  SpinProbeState wait;
  wait.gpr[1]=0x8000u;wait.gpr[30]=0x1000u;wait.gpr[31]=9u;
  wait.gpr[3]=10u;wait.gpr[9]=2u;wait.gpr[0]=1u;wait.cr=0x80000000u;wait.lr=0x404u;
  check(probe.FixedPoint(wait,0x400,pacing));
  auto unstored=pacing; unstored.words[0x1000]=9u;   // first iteration still publishes
  check(!probe.FixedPoint(wait,0x400,unstored));
  auto ticked=pacing; ticked.words[0x1008]=11u;      // the awaited retrace arrived
  check(!probe.FixedPoint(wait,0x400,ticked));
  auto entered=wait; entered.gpr[0]=0u;              // registers not yet at the fixed point
  check(!probe.FixedPoint(entered,0x400,pacing));
  pacing.foreign=0x500u;                             // an HLE getter is never interpreted
  check(!probe.FixedPoint(wait,0x400,pacing));
  cases+=5;
  std::cout << "native spin probe PASS cases=" << cases
            << " sandbox=overlay-only exit=verified-threshold equality=aot\n";
}
