// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <cstdint>
#include <cstring>
#include <optional>

namespace GekkoAOT::Native {
// Analysis of a bounded RAM-reading wait loop, never guest execution. A
// successful proof returns to exactly the same GPR/CR state at the same PC.
// Stores, calls, counter branches, MMIO, SPRs and exceptions are excluded.
// The caller can then wait for a host event and let original AOT resume.
struct NativeReadPoll {
  static constexpr unsigned MaxWords=17;
  std::array<std::uint32_t,MaxWords> words{};
  std::uint32_t begin=0,end=0;
  template<class ReadCode>
  static std::optional<NativeReadPoll> Decode(std::uint32_t pc,ReadCode read) {
    std::uint32_t current=0;
    if((pc&3u) || !read(pc,&current)) return {};
    const auto opcode=current>>26;
    if(opcode!=32u && opcode!=10u && opcode!=11u && opcode!=16u && opcode!=31u && current!=0x60000000u) return {};
    for(unsigned ahead=0;ahead<MaxWords;++ahead) {
      const auto branch_pc=pc+4u*ahead;
      std::uint32_t branch;
      if(!read(branch_pc,&branch)) break;
      if((branch&0xfc000003u)!=0x40000000u || (branch&(4u<<21u))==0u) continue;
      const auto relative=static_cast<std::int16_t>(branch&0xfffcu);
      if(relative>=0 || -int(relative)>int(4u*(MaxWords-1u))) continue;
      const auto start=branch_pc+static_cast<std::uint32_t>(relative);
      if(start>pc || branch_pc<pc) continue;
      NativeReadPoll plan;plan.begin=start;plan.end=branch_pc+4u;
      bool valid=true;
      for(unsigned i=0;i<(plan.end-plan.begin)/4u;++i)
        if(!read(plan.begin+4u*i,&plan.words[i])) {valid=false;break;}
      if(valid) return plan;
    }
    return {};
  }

  template<class ReadRam>
  bool Stable(std::uint32_t entry,const std::array<std::uint32_t,32>& incoming,
              std::uint32_t condition,std::uint32_t xer,ReadRam read) const {
    auto regs=incoming;
    auto cr=condition,pc=entry;
    unsigned reads=0;
    for(unsigned step=0;step<2u*MaxWords;++step) {
      if(pc<begin || pc>=end || (pc&3u)) return false;
      const auto word=words[(pc-begin)/4u];
      const unsigned opcode=word>>26, rt=(word>>21u)&31u,ra=(word>>16u)&31u,rb=(word>>11u)&31u;
      const auto next=pc+4u;
      if(opcode==32u) {
        const auto address=(ra ? regs[ra] : 0u)+static_cast<std::uint32_t>(static_cast<std::int16_t>(word));
        if((address&3u) || !read(address,&regs[rt])) return false;
        ++reads;pc=next;
      } else if(opcode==10u || opcode==11u ||
                (opcode==31u && ((word>>1u)&1023u)==0u && !(word&1u)) ||
                (opcode==31u && ((word>>1u)&1023u)==32u && !(word&1u))) {
        if(word&(1u<<21u)) return false; // 64-bit compare unsupported on Gekko.
        const bool logical=opcode==10u || (opcode==31u && ((word>>1u)&1023u)==32u);
        const std::uint32_t left=regs[ra];
        const std::uint32_t right=opcode==31u ? regs[rb] : logical ? word&0xffffu :
            static_cast<std::uint32_t>(static_cast<std::int16_t>(word));
        const bool less=logical ? left<right : static_cast<std::int32_t>(left)<static_cast<std::int32_t>(right);
        const auto field=(less ? 8u : left==right ? 2u : 4u)|(xer>>31u);
        const auto shift=28u-4u*((word>>23u)&7u);
        cr=(cr&~(15u<<shift))|(field<<shift);pc=next;
      } else if(opcode==16u && !(word&3u)) {
        const unsigned bo=rt,bi=ra;
        if(!(bo&4u)) return false; // Finite CTR delay loops must execute.
        const bool taken=(bo&16u) || bool((cr>>(31u-bi))&1u)==bool(bo&8u);
        pc=taken ? pc+static_cast<std::uint32_t>(static_cast<std::int16_t>(word&0xfffcu)) : next;
      } else if(word==0x60000000u) pc=next;
      else return false;
      if(pc==entry)
        return reads && cr==condition && std::memcmp(regs.data(),incoming.data(),sizeof regs)==0;
    }
    return false;
  }
};
} // namespace GekkoAOT::Native
