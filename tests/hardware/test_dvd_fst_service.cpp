// SPDX-License-Identifier: GPL-3.0-or-later
#include "native/runtime.h"
#include "sdk/dvd_fst_service.h"
#include <array>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <memory>
#define CHECK(x) do{if(!(x)){std::cerr<<__LINE__<<": " #x "\n";std::exit(1);}}while(false)
namespace GekkoAOT::Native {
struct HardwareTestAccess {
  static bool Call(HostRuntime& r){return r.DispatchSimpleHle(SimpleHleKind::DvdFastOpen,r.Cpu().pc);}

  static bool Faulted(HostRuntime& r){return r.fault_.kind!=HostRuntime::FaultKind::None;}
};
}
using namespace GekkoAOT::Native;
constexpr std::array<unsigned,29> code{
  0x2c030000,0x41800038,0x800dffe0,0x7c030040,0x4080002c,
  0x1cc3000c,0x806dffe4,0x7c03302e,0x5400000f,0x4082000c,
  0x38000000,0x48000008,0x38000001,0x2c000000,0x4182000c,
  0x38600000,0x4e800020,0x7c633214,0x80a30004,0x38000000,
  0x38600001,0x90a40030,0x80adffe4,0x7ca53214,0x80a50008,
  0x90a40034,0x90040038,0x9004000c,0x4e800020};
constexpr unsigned pc=0x80010000,sda=0x80020020,fst=0x80030000,info=0x80040000;
static void Setup(HostRuntime& r,unsigned entry,unsigned dst=info) {
  auto& m=r.Memory();auto& c=r.Cpu();c.pc=pc;c.lr=0x80011000;c.gpr[13]=sda;c.gpr[3]=entry;c.gpr[4]=dst;
  for(unsigned i=0;i<code.size();++i)CHECK(m.Write32(pc+4*i,code[i]));
  CHECK(m.Write32(sda-32,8));CHECK(m.Write32(sda-28,fst));
  for(unsigned i=0;i<8;++i){CHECK(m.Write32(fst+12*i,i==0||i==4?0x01000000:i));CHECK(m.Write32(fst+12*i+4,0x101000+i*0x83));CHECK(m.Write32(fst+12*i+8,0x301+i*0x55));}
  std::memset(m.Resolve(info,64),0x6b,64);
}
static std::array<unsigned char,64> Output(HostRuntime& r,unsigned ea=info) {
  std::array<unsigned char,64> out{};std::memcpy(out.data(),r.Memory().Resolve(ea,64),64);return out;
}
// Small independent PPC reference for the fixture, executing its loads,
// branches and stores in order rather than sharing the native FST algorithm.
static void GuestReference(HostRuntime& r) {
  auto& c=r.Cpu();auto& m=r.Memory();unsigned steps=0;
  auto compare=[&](auto a,auto b){c.cr=(c.cr&0x0fffffffu)|((a<b?8u:a>b?4u:2u)<<28);};
  while(c.pc!=c.lr) {
    CHECK(++steps<64);unsigned w=0;CHECK(m.Read32(c.pc,&w));
    const auto op=w>>26,rd=(w>>21)&31,ra=(w>>16)&31,rb=(w>>11)&31;
    const auto imm=static_cast<std::int16_t>(w);const auto next=c.pc+4;
    switch(op) {
    case 11:compare(static_cast<std::int32_t>(c.gpr[ra]),static_cast<std::int32_t>(imm));break;
    case 7:c.gpr[rd]=static_cast<std::uint32_t>(std::int64_t(static_cast<std::int32_t>(c.gpr[ra]))*imm);break;
    case 14:c.gpr[rd]=(ra?c.gpr[ra]:0u)+static_cast<std::uint32_t>(imm);break;
    case 16:{const auto bo=(w>>21)&31,bi=(w>>16)&31;const bool bit=(c.cr>>(31-bi))&1;
      CHECK(bo==12||bo==4);if(bit==(bo==12)){c.pc+=static_cast<std::int16_t>(w&0xfffc);continue;}break;}
    case 18:{auto d=static_cast<std::int32_t>(w&0x03fffffc);if(d&0x02000000)d|=0xfc000000;c.pc+=d;continue;}
    case 19:CHECK(w==0x4e800020);c.pc=c.lr;continue;
    case 21:CHECK(w==0x5400000f);c.gpr[0]&=0xff000000;compare(static_cast<std::int32_t>(c.gpr[0]),0);break;
    case 32:CHECK(m.Read32((ra?c.gpr[ra]:0u)+static_cast<std::uint32_t>(imm),&c.gpr[rd]));break;
    case 36:CHECK(m.Write32((ra?c.gpr[ra]:0u)+static_cast<std::uint32_t>(imm),c.gpr[rd]));break;
    case 31:switch((w>>1)&1023) {
      case 32:compare(c.gpr[ra],c.gpr[rb]);break;
      case 23:CHECK(m.Read32((ra?c.gpr[ra]:0u)+c.gpr[rb],&c.gpr[rd]));break;
      case 266:c.gpr[rd]=c.gpr[ra]+c.gpr[rb];break;
      default:CHECK(false);
    }break;
    default:CHECK(false);
    }
    c.pc=next;
  }
}
int main() {
  auto fast=std::make_unique<HostRuntime>(),reference=std::make_unique<HostRuntime>();
  Setup(*fast,1);const auto* body=fast->Memory().Resolve(pc,116);
  CHECK(GekkoAOTSdk::ProveDvdFastOpen(body,116));CHECK(!GekkoAOTSdk::ProveDvdFastOpen(body,115));
  for(unsigned i=0;i<29;++i)for(unsigned bit=0;bit<32;++bit) {
    CHECK(fast->Memory().Write32(pc+i*4,code[i]^(1u<<bit)));
    const bool relocation=(i==2&&bit<16);
    CHECK(bool(GekkoAOTSdk::ProveDvdFastOpen(body,116))==relocation);
    CHECK(fast->Memory().Write32(pc+i*4,code[i]));
  }
  for(unsigned entry:{0u,1u,2u,3u,4u,5u,7u,8u,0x7fffffffu,0xffffffffu}) {
    Setup(*fast,entry);Setup(*reference,entry);
    const char* flag=std::getenv("GEKKOAOT_NATIVE_DVD_FST");
    if(flag&&flag[0]=='0') {
      const auto before=fast->Cpu();const auto out=Output(*fast);
      CHECK(!HardwareTestAccess::Call(*fast));CHECK(std::memcmp(&before,&fast->Cpu(),sizeof before)==0);CHECK(out==Output(*fast));continue;
    }
    CHECK(HardwareTestAccess::Call(*fast));
    GuestReference(*reference);
    CHECK(!HardwareTestAccess::Faulted(*reference));
    CHECK(fast->Cpu().gpr[3]==reference->Cpu().gpr[3]);CHECK(fast->Cpu().pc==fast->Cpu().lr);
    CHECK(Output(*fast)==Output(*reference));
  }
  if(const char* flag=std::getenv("GEKKOAOT_NATIVE_DVD_FST");flag&&flag[0]=='0') {std::cout<<"DVD FST independent disable preserves CPU/output\n";return 0;}
  // Live SDK globals can change without recompilation; no low-memory FST guess.
  Setup(*fast,2);CHECK(fast->Memory().Write32(sda-28,fst+12));CHECK(HardwareTestAccess::Call(*fast));
  unsigned word=0;CHECK(fast->Memory().Read32(info+0x30,&word)&&word==0x101000+3*0x83);
  // Admission failures leave every output byte and architectural CPU field intact.
  for(unsigned dst:{0xffffffffu,0xcc006000u,fst+12-0x30,0xc003000cu-0x30}) {
    Setup(*fast,1,dst);const auto before=fast->Cpu();const auto out=Output(*fast);
    CHECK(!HardwareTestAccess::Call(*fast));CHECK(std::memcmp(&before,&fast->Cpu(),sizeof before)==0);CHECK(out==Output(*fast));CHECK(!HardwareTestAccess::Faulted(*fast));
  }
  Setup(*fast,1);CHECK(fast->Memory().Write32(pc,0x60000000));const auto before=fast->Cpu();
  CHECK(!HardwareTestAccess::Call(*fast));CHECK(std::memcmp(&before,&fast->Cpu(),sizeof before)==0);
  // Nonresident VM output declines; a resident BAT view writes the real frame.
  Setup(*fast,1,0x7e001ff0);auto& c=fast->Cpu();c.msr=0x32;c.spr_write(&c,25,0x10000,pc);
  const auto missing_before=c;CHECK(!HardwareTestAccess::Call(*fast));CHECK(std::memcmp(&missing_before,&c,sizeof c)==0);
  c.spr_write(&c,540,0x7e000002,pc);c.spr_write(&c,541,0x00300002,pc);
  CHECK(HardwareTestAccess::Call(*fast));CHECK(fast->Memory().Read32(0x302020,&word)&&word==0x101083);
  CHECK(fast->Memory().Read32(0x302024,&word)&&word==0x356);
  std::cout<<"DVD FST: PPC differential, whole-leaf proof, live SDA/FST, physical aliases, VM fallback and independent disable passed\n";
}
