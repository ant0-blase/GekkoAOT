// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <optional>
#include <vector>
namespace GekkoAOT::Native {
// v334: exact parking of clock waits whose body is more than a compare, e.g.
// Driven's stream sync: `do { pos = ticks_to_units(OSGetTime() - start);
// obj->pos = pos; } while (pos < obj->limit);` with 64-bit divides through
// guest helpers (6M clock calls per second, 95% of a core).
//
// The probe interprets the guest integer code that follows the clock call in
// a sandbox: registers are a copy, loads read guest RAM, stores go to a
// private overlay and are never published. An evaluation "continues" when it
// reaches the same clock call site with the same stack pointer while visiting
// only instructions the observed iteration visited; anything else (FP, MMIO,
// MSR/SPR access, another HLE, new code such as the work after the wait, the
// budget) is an exit. The runtime parks only until the first clock value whose
// evaluation exits, so the guest itself always executes that iteration.
struct SpinProbeState {
  std::array<std::uint32_t,32> gpr{};
  std::uint32_t cr=0,xer=0,lr=0,ctr=0;
};

class SpinProbe {
  // Portable byte reversal (MSVC lacks the GCC byte-swap builtins); compilers
  // still emit a single bswap for this pattern.
  static std::uint32_t Swap32(std::uint32_t v) {
    return (v >> 24u) | ((v >> 8u) & 0xff00u) | ((v << 8u) & 0xff0000u) | (v << 24u);
  }
  static std::uint32_t Swap16(std::uint32_t v) {
    return ((v >> 8u) & 0xffu) | ((v & 0xffu) << 8u);
  }
public:
  static constexpr unsigned kBudget = 4096u;
  enum class Outcome : std::uint8_t {Continue, Exit};

  // Memory: Fetch(ea,&word), Read(ea,size,&value) for guest RAM only,
  // Foreign(target) true for HLE entries and other non-interpretable targets.
  // `clock` is the value every clock call returns in this evaluation: the
  // call that just returned (r3[:r4]) and further reads inside the iteration
  // (Driven reads the clock twice per iteration, in the loop and in a helper).
  template<class Memory>
  Outcome Run(const SpinProbeState& start, std::uint32_t resume, std::uint32_t call_site,
              std::uint32_t clock_entry, std::uint64_t clock, bool wide, bool record,
              Memory& memory) {
    state_=start;
    const auto set_clock=[&] {
      if(wide) {
        state_.gpr[3]=static_cast<std::uint32_t>(clock>>32u);
        state_.gpr[4]=static_cast<std::uint32_t>(clock);
      } else {
        state_.gpr[3]=static_cast<std::uint32_t>(clock);
      }
    };
    set_clock();
    overlay_.clear();
    if(record) visited_.clear();
    std::uint32_t pc=resume;
    const std::uint32_t sp=start.gpr[1];
    unsigned nested_clock_reads=0;
    for(unsigned step=0;step<kBudget;++step) {
      if(record) visited_.push_back(pc);
      else if(!std::binary_search(visited_.begin(),visited_.end(),pc)) return Outcome::Exit;
      std::uint32_t w;
      if(!memory.Fetch(pc,&w)) return Outcome::Exit;
      std::uint32_t next=pc+4u;
      const auto result=Execute(w,pc,&next,memory);
      if(result==Step::Fault) return Outcome::Exit;
      if(result==Step::Call) {
        if(next==clock_entry) {
          if(pc!=call_site || state_.gpr[1]!=sp) {
            // Another clock read inside the same iteration returns the same value.
            if(++nested_clock_reads>4u) return Outcome::Exit;
            set_clock();
            pc=state_.lr;
            continue;
          }
          if(record) {
            std::sort(visited_.begin(),visited_.end());
            visited_.erase(std::unique(visited_.begin(),visited_.end()),visited_.end());
          }
          return Outcome::Continue;
        }
        if(memory.Foreign(next)) return Outcome::Exit;
      }
      pc=next;
    }
    return Outcome::Exit;
  }

  // v346: one iteration of a loop entered at `entry` that changes nothing:
  // it comes back to `entry` in the same frame with every register equal and
  // stores only bytes RAM already holds (Nemo's `while (VIGetRetraceCount() -
  // start < n)` re-stores the count it just read). Such a loop can only stop
  // when an interrupt, DMA or another thread changes memory. Calls are
  // followed; HLE entries, MMIO, SPRs other than LR/CTR/XER and FP end it.
  // `why` (optional) receives the failing PC/word: {pc, word, reason} with
  // reason 1 fetch, 2 unsupported/fault, 3 HLE call, 4 registers, 5 store, 6 budget.
  template<class Memory>
  bool FixedPoint(const SpinProbeState& start, std::uint32_t entry, Memory& memory,
                  std::array<std::uint32_t,3>* why=nullptr) {
    state_=start;
    overlay_.clear();
    std::uint32_t pc=entry;
    const auto fail=[&](std::uint32_t at,std::uint32_t word,std::uint32_t reason) {
      if(why) *why={at,word,reason};
      return false;
    };
    // A wait iteration is short; anything longer is real work (and the probe
    // runs at boundaries, so it must stay cheap).
    constexpr unsigned kFixedPointBudget=512u;
    for(unsigned step=0;step<kFixedPointBudget;++step) {
      std::uint32_t w;
      if(!memory.Fetch(pc,&w)) return fail(pc,0u,1u);
      std::uint32_t next=pc+4u;
      const auto result=Execute(w,pc,&next,memory);
      if(result==Step::Fault) return fail(pc,w,2u);
      if(result==Step::Call && memory.Foreign(next)) return fail(pc,next,3u);
      pc=next;
      if(pc!=entry || state_.gpr[1]!=start.gpr[1]) continue;
      if(state_.gpr!=start.gpr || state_.cr!=start.cr || state_.lr!=start.lr ||
         state_.ctr!=start.ctr || state_.xer!=start.xer || overlay_.size()>256u) {
        std::uint32_t reg=0u;
        while(reg<32u && state_.gpr[reg]==start.gpr[reg]) ++reg;
        return fail(reg,state_.cr^start.cr,4u);
      }
      for(std::size_t i=overlay_.size();i-->0;) {
        bool later=false;  // only the last store to a byte is visible
        for(std::size_t j=i+1;j<overlay_.size() && !later;++j) later=overlay_[j].ea==overlay_[i].ea;
        std::uint32_t value;
        if(!later && (!memory.Read(overlay_[i].ea,1u,&value) || value!=overlay_[i].value))
          return fail(overlay_[i].ea,value,5u);
      }
      return true;
    }
    return fail(pc,0u,6u);
  }

  // First clock value after `now` whose evaluation stops waiting: an
  // exponential bracket (every probed value before it waits), a binary
  // search, then threshold samples after it. Nothing when the wait does not
  // end inside the horizon or is not a threshold (e.g. an equality wait).
  template<class Waits>
  static std::optional<std::uint64_t> FirstExit(std::uint64_t now,std::uint64_t horizon,Waits waits) {
    std::uint64_t lo=0u,hi=1u;
    while(hi<=horizon && waits(now+hi)) {lo=hi;hi*=2u;}
    if(hi>horizon) return {};
    while(hi-lo>1u) {
      const auto mid=lo+(hi-lo)/2u;
      (waits(now+mid) ? lo : hi)=mid;
    }
    for(std::uint64_t step=1u;step<=hi;step*=2u)
      if(waits(now+hi+step)) return {};
    return now+hi;
  }

private:
  enum class Step : std::uint8_t {Next, Call, Fault};
  struct Byte {std::uint32_t ea; std::uint8_t value;};
  SpinProbeState state_{};
  std::vector<Byte> overlay_;
  std::vector<std::uint32_t> visited_;

  template<class Memory>
  bool Load(Memory& memory, std::uint32_t ea, unsigned size, std::uint32_t* out) {
    std::uint32_t value=0;
    if(!memory.Read(ea,size,&value)) return false;
    for(unsigned i=0;i<size;++i) {
      const auto byte_ea=ea+i;
      for(auto it=overlay_.rbegin();it!=overlay_.rend();++it) {
        if(it->ea==byte_ea) {
          const unsigned shift=8u*(size-1u-i);
          value=(value&~(0xffu<<shift))|(std::uint32_t(it->value)<<shift);
          break;
        }
      }
    }
    *out=value;
    return true;
  }
  template<class Memory>
  bool Store(Memory& memory, std::uint32_t ea, unsigned size, std::uint32_t value) {
    std::uint32_t probe;
    if(!memory.Read(ea,size,&probe) || overlay_.size()>4096u) return false;  // RAM only
    for(unsigned i=0;i<size;++i)
      overlay_.push_back({ea+i,static_cast<std::uint8_t>(value>>(8u*(size-1u-i)))});
    return true;
  }
  bool CrBit(unsigned bit) const {return (state_.cr>>(31u-bit))&1u;}
  void SetCrBit(unsigned bit,bool value) {
    const auto mask=1u<<(31u-bit);
    state_.cr=value ? state_.cr|mask : state_.cr&~mask;
  }
  void SetCrField(unsigned field,std::uint32_t bits) {
    const auto shift=28u-4u*field;
    state_.cr=(state_.cr&~(0xfu<<shift))|((bits&0xfu)<<shift);
  }
  void Record(std::uint32_t value) {
    const auto v=static_cast<std::int32_t>(value);
    SetCrField(0,(v<0 ? 8u : v>0 ? 4u : 2u)|((state_.xer>>31u)&1u));
  }
  void Compare(unsigned field,bool less,bool greater) {
    SetCrField(field,(less ? 8u : greater ? 4u : 2u)|((state_.xer>>31u)&1u));
  }
  std::uint32_t Ca() const {return (state_.xer>>29u)&1u;}
  void SetCa(bool carry) {state_.xer=carry ? state_.xer|0x20000000u : state_.xer&~0x20000000u;}
  std::uint32_t Add(std::uint32_t a,std::uint32_t b,std::uint32_t c,bool set_ca) {
    const std::uint64_t sum=std::uint64_t(a)+b+c;
    if(set_ca) SetCa((sum>>32u)!=0u);
    return static_cast<std::uint32_t>(sum);
  }
  static std::uint32_t Mask(unsigned mb,unsigned me) {
    const std::uint32_t begin=0xffffffffu>>mb;
    const std::uint32_t end=me==31u ? 0xffffffffu : ~(0xffffffffu>>(me+1u));
    return mb<=me ? begin&end : begin|end;
  }
  static std::uint32_t Rotl(std::uint32_t v,unsigned n) {n&=31u;return n ? (v<<n)|(v>>(32u-n)) : v;}
  bool BranchTaken(unsigned bo,unsigned bi) {
    if(!(bo&4u)) {
      --state_.ctr;
      const bool zero=state_.ctr==0u;
      if(((bo&2u)!=0u)!=zero) return false;
    }
    return (bo&16u) || CrBit(bi)==((bo&8u)!=0u);
  }

  template<class Memory>
  Step Execute(std::uint32_t w,std::uint32_t pc,std::uint32_t* next,Memory& memory) {
    auto& r=state_.gpr;
    const unsigned op=w>>26u,d=(w>>21u)&31u,a=(w>>16u)&31u,b=(w>>11u)&31u;
    const std::uint32_t uimm=w&0xffffu;
    const std::uint32_t simm=static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<std::int16_t>(uimm)));
    const std::uint32_t base=a ? r[a] : 0u;
    const bool rc=(w&1u)!=0u;
    std::uint32_t value;
    switch(op) {
    case 7u: r[d]=r[a]*simm; return Step::Next;
    case 8u: r[d]=Add(~r[a],simm,1u,true); return Step::Next;
    case 10u: Compare(d>>2u,r[a]<uimm,r[a]>uimm); return (d&1u) ? Step::Fault : Step::Next;
    case 11u: Compare(d>>2u,std::int32_t(r[a])<std::int32_t(simm),std::int32_t(r[a])>std::int32_t(simm));
      return (d&1u) ? Step::Fault : Step::Next;
    case 12u: r[d]=Add(r[a],simm,0u,true); return Step::Next;
    case 13u: r[d]=Add(r[a],simm,0u,true); Record(r[d]); return Step::Next;
    case 14u: r[d]=base+simm; return Step::Next;
    case 15u: r[d]=base+(uimm<<16u); return Step::Next;
    case 16u: {
      const auto bd=static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<std::int16_t>(w&0xfffcu)));
      const bool taken=BranchTaken(d,a);
      if(w&1u) state_.lr=pc+4u;
      if(taken) *next=((w&2u) ? 0u : pc)+bd;
      return (taken && (w&1u)) ? Step::Call : Step::Next;
    }
    case 18u: {
      const auto li=static_cast<std::uint32_t>(static_cast<std::int32_t>((w&0x03fffffcu)<<6u)>>6u);
      *next=((w&2u) ? 0u : pc)+li;
      if(w&1u) {state_.lr=pc+4u;return Step::Call;}
      return Step::Next;
    }
    case 19u: {
      const unsigned xo=(w>>1u)&0x3ffu;
      if(xo==16u || xo==528u) {
        if(xo==528u && !(d&4u)) return Step::Fault;
        const std::uint32_t target=(xo==16u ? state_.lr : state_.ctr)&~3u;
        const bool taken=BranchTaken(d,a);
        if(w&1u) state_.lr=pc+4u;
        if(taken) *next=target;
        return (taken && (w&1u)) ? Step::Call : Step::Next;
      }
      if(xo==0u) {  // mcrf
        if((d&3u) || (a&3u)) return Step::Fault;
        SetCrField(d>>2u,(state_.cr>>(28u-4u*(a>>2u)))&0xfu);
        return Step::Next;
      }
      if(xo==150u) return Step::Next;  // isync
      const bool x=CrBit(a),y=CrBit(b);
      switch(xo) {
      case 257u: SetCrBit(d,x&&y); return Step::Next;
      case 129u: SetCrBit(d,x&&!y); return Step::Next;
      case 289u: SetCrBit(d,x==y); return Step::Next;
      case 225u: SetCrBit(d,!(x&&y)); return Step::Next;
      case 33u: SetCrBit(d,!(x||y)); return Step::Next;
      case 449u: SetCrBit(d,x||y); return Step::Next;
      case 417u: SetCrBit(d,x||!y); return Step::Next;
      case 193u: SetCrBit(d,x!=y); return Step::Next;
      }
      return Step::Fault;
    }
    case 20u: {  // rlwimi
      const auto m=Mask((w>>6u)&31u,(w>>1u)&31u);
      r[a]=(Rotl(r[d],b)&m)|(r[a]&~m);
      if(rc) Record(r[a]);
      return Step::Next;
    }
    case 21u: r[a]=Rotl(r[d],b)&Mask((w>>6u)&31u,(w>>1u)&31u); if(rc) Record(r[a]); return Step::Next;
    case 23u: r[a]=Rotl(r[d],r[b]&31u)&Mask((w>>6u)&31u,(w>>1u)&31u); if(rc) Record(r[a]); return Step::Next;
    case 24u: r[a]=r[d]|uimm; return Step::Next;
    case 25u: r[a]=r[d]|(uimm<<16u); return Step::Next;
    case 26u: r[a]=r[d]^uimm; return Step::Next;
    case 27u: r[a]=r[d]^(uimm<<16u); return Step::Next;
    case 28u: r[a]=r[d]&uimm; Record(r[a]); return Step::Next;
    case 29u: r[a]=r[d]&(uimm<<16u); Record(r[a]); return Step::Next;
    case 32u: case 33u: case 34u: case 35u: case 40u: case 41u: case 42u: case 43u: {
      const bool update=op&1u;
      if(update && (a==0u || a==d)) return Step::Fault;
      const unsigned size=op<36u ? (op<34u ? 4u : 1u) : 2u;
      const std::uint32_t ea=base+simm;
      if(!Load(memory,ea,size,&value)) return Step::Fault;
      if(op>=42u) value=static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<std::int16_t>(value)));
      r[d]=value;
      if(update) r[a]=ea;
      return Step::Next;
    }
    case 36u: case 37u: case 38u: case 39u: case 44u: case 45u: {
      const bool update=op&1u;
      if(update && a==0u) return Step::Fault;
      const unsigned size=op<38u ? 4u : op<40u ? 1u : 2u;
      const std::uint32_t ea=base+simm;
      if(!Store(memory,ea,size,r[d])) return Step::Fault;
      if(update) r[a]=ea;
      return Step::Next;
    }
    case 46u: {  // lmw
      std::uint32_t ea=base+simm;
      if(a>=d && a!=0u) return Step::Fault;
      for(unsigned reg=d;reg<32u;++reg,ea+=4u) {
        if(!Load(memory,ea,4u,&value)) return Step::Fault;
        r[reg]=value;
      }
      return Step::Next;
    }
    case 47u: {  // stmw
      std::uint32_t ea=base+simm;
      for(unsigned reg=d;reg<32u;++reg,ea+=4u)
        if(!Store(memory,ea,4u,r[reg])) return Step::Fault;
      return Step::Next;
    }
    case 31u: return ExecuteX(w,memory);
    }
    return Step::Fault;
  }

  template<class Memory>
  Step ExecuteX(std::uint32_t w,Memory& memory) {
    auto& r=state_.gpr;
    const unsigned d=(w>>21u)&31u,a=(w>>16u)&31u,b=(w>>11u)&31u;
    const unsigned xo=(w>>1u)&0x3ffu;
    const bool rc=(w&1u)!=0u;
    const std::uint32_t base=a ? r[a] : 0u;
    std::uint32_t value;
    const auto done=[&](unsigned reg) {if(rc) Record(r[reg]);return Step::Next;};
    // Indexed loads/stores (no record forms).
    struct Access {unsigned xo,size;bool store,update,algebraic,reverse;};
    static constexpr Access kAccess[]={
        {23u,4u,false,false,false,false},{55u,4u,false,true,false,false},
        {87u,1u,false,false,false,false},{119u,1u,false,true,false,false},
        {279u,2u,false,false,false,false},{311u,2u,false,true,false,false},
        {343u,2u,false,false,true,false},{375u,2u,false,true,true,false},
        {534u,4u,false,false,false,true},{790u,2u,false,false,false,true},
        {151u,4u,true,false,false,false},{183u,4u,true,true,false,false},
        {215u,1u,true,false,false,false},{247u,1u,true,true,false,false},
        {407u,2u,true,false,false,false},{439u,2u,true,true,false,false},
        {662u,4u,true,false,false,true},{918u,2u,true,false,false,true}};
    for(const auto& access:kAccess) {
      if(access.xo!=xo) continue;
      if(rc || (access.update && (a==0u || (!access.store && a==d)))) return Step::Fault;
      const std::uint32_t ea=base+r[b];
      if(access.store) {
        value=r[d];
        if(access.reverse)
          value=access.size==4u ? Swap32(value) : Swap16(value);
        if(!Store(memory,ea,access.size,value)) return Step::Fault;
      } else {
        if(!Load(memory,ea,access.size,&value)) return Step::Fault;
        if(access.reverse)
          value=access.size==4u ? Swap32(value) : Swap16(value);
        if(access.algebraic)
          value=static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<std::int16_t>(value)));
        r[d]=value;
      }
      if(access.update) r[a]=ea;
      return Step::Next;
    }
    switch(xo) {
    case 0u: case 32u:
      if(rc || (d&3u)) return Step::Fault;
      if(xo==0u) Compare(d>>2u,std::int32_t(r[a])<std::int32_t(r[b]),std::int32_t(r[a])>std::int32_t(r[b]));
      else Compare(d>>2u,r[a]<r[b],r[a]>r[b]);
      return Step::Next;
    case 19u: if(rc) return Step::Fault; r[d]=state_.cr; return Step::Next;  // mfcr
    case 144u: {  // mtcrf
      if(rc) return Step::Fault;
      const unsigned crm=(w>>12u)&0xffu;
      std::uint32_t mask=0u;
      for(unsigned f=0;f<8u;++f) if(crm&(0x80u>>f)) mask|=0xf0000000u>>(4u*f);
      state_.cr=(r[d]&mask)|(state_.cr&~mask);
      return Step::Next;
    }
    case 339u: case 467u: {  // mfspr/mtspr: LR, CTR, XER only
      if(rc) return Step::Fault;
      const unsigned spr=((w>>16u)&31u)|(((w>>11u)&31u)<<5u);
      std::uint32_t* reg=spr==8u ? &state_.lr : spr==9u ? &state_.ctr : spr==1u ? &state_.xer : nullptr;
      if(!reg) return Step::Fault;
      if(xo==339u) r[d]=*reg; else *reg=r[d];
      return Step::Next;
    }
    case 54u: case 86u: case 278u: case 246u: case 598u: case 854u:  // dcbst/dcbf/dcbt/dcbtst/sync/eieio
      return rc ? Step::Fault : Step::Next;
    case 24u: r[a]=(r[b]&32u) ? 0u : r[d]<<(r[b]&31u); return done(a);
    case 536u: r[a]=(r[b]&32u) ? 0u : r[d]>>(r[b]&31u); return done(a);
    case 792u: {  // sraw
      const unsigned n=r[b]&63u;
      const auto v=static_cast<std::int32_t>(r[d]);
      if(n>31u) {r[a]=v<0 ? 0xffffffffu : 0u;SetCa(v<0);}
      else {
        r[a]=static_cast<std::uint32_t>(v>>n);
        SetCa(v<0 && n && (r[d]&((1u<<n)-1u)));
      }
      return done(a);
    }
    case 824u: {  // srawi
      const unsigned n=b;
      const auto v=static_cast<std::int32_t>(r[d]);
      r[a]=static_cast<std::uint32_t>(v>>n);
      SetCa(v<0 && n && (r[d]&((1u<<n)-1u)));
      return done(a);
    }
    case 26u: r[a]=static_cast<std::uint32_t>(std::countl_zero(r[d])); return done(a);
    case 28u: r[a]=r[d]&r[b]; return done(a);
    case 60u: r[a]=r[d]&~r[b]; return done(a);
    case 124u: r[a]=~(r[d]|r[b]); return done(a);
    case 284u: r[a]=~(r[d]^r[b]); return done(a);
    case 316u: r[a]=r[d]^r[b]; return done(a);
    case 412u: r[a]=r[d]|~r[b]; return done(a);
    case 444u: r[a]=r[d]|r[b]; return done(a);
    case 476u: r[a]=~(r[d]&r[b]); return done(a);
    case 922u: r[a]=static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<std::int16_t>(r[d]))); return done(a);
    case 954u: r[a]=static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<std::int8_t>(r[d]))); return done(a);
    }
    if(w&0x400u) return Step::Fault;  // OE forms
    switch(xo&0x1ffu) {
    case 8u: r[d]=Add(~r[a],r[b],1u,true); return done(d);
    case 10u: r[d]=Add(r[a],r[b],0u,true); return done(d);
    case 11u: r[d]=static_cast<std::uint32_t>((std::uint64_t(r[a])*r[b])>>32u); return done(d);
    case 40u: r[d]=r[b]-r[a]; return done(d);
    case 75u: r[d]=static_cast<std::uint32_t>(static_cast<std::uint64_t>(
                   std::int64_t(std::int32_t(r[a]))*std::int32_t(r[b]))>>32u); return done(d);
    case 104u: if(b) return Step::Fault; r[d]=0u-r[a]; return done(d);
    case 136u: r[d]=Add(~r[a],r[b],Ca(),true); return done(d);
    case 138u: r[d]=Add(r[a],r[b],Ca(),true); return done(d);
    case 200u: if(b) return Step::Fault; r[d]=Add(~r[a],0u,Ca(),true); return done(d);
    case 202u: if(b) return Step::Fault; r[d]=Add(r[a],0u,Ca(),true); return done(d);
    case 232u: if(b) return Step::Fault; r[d]=Add(~r[a],0xffffffffu,Ca(),true); return done(d);
    case 234u: if(b) return Step::Fault; r[d]=Add(r[a],0xffffffffu,Ca(),true); return done(d);
    case 235u: r[d]=r[a]*r[b]; return done(d);
    case 266u: r[d]=r[a]+r[b]; return done(d);
    case 459u:  // divwu: undefined quotient on /0 is not modelled
      if(r[b]==0u) return Step::Fault;
      r[d]=r[a]/r[b]; return done(d);
    case 491u: {
      const auto x=static_cast<std::int32_t>(r[a]),y=static_cast<std::int32_t>(r[b]);
      if(y==0 || (x==INT32_MIN && y==-1)) return Step::Fault;
      r[d]=static_cast<std::uint32_t>(x/y); return done(d);
    }
    }
    return Step::Fault;
  }
};
} // namespace GekkoAOT::Native
