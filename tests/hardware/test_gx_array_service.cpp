// SPDX-License-Identifier: GPL-3.0-or-later
#include "native/runtime.h"
#include "sdk/gx_array_service.h"
#include <array>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <vector>
#define CHECK(x) do{if(!(x)){std::cerr<<__LINE__<<": " #x "\n";std::exit(1);}}while(false)
extern "C" unsigned gekkoaot_test_gx_bytes();
extern "C" unsigned char gekkoaot_test_gx_byte(unsigned);
namespace GekkoAOT::Native {
struct HardwareTestAccess {
  static bool Call(HostRuntime& r){return r.DispatchSimpleHle(SimpleHleKind::GxSetArray,r.Cpu().pc);}
  static unsigned Pending(HostRuntime& r){return r.cpu_fifo_gather_size_+r.gpu_fifo_gather_size_;}
  static bool Flush(HostRuntime& r){return r.native_gx_.FlushWrites();}
};
}
using namespace GekkoAOT::Native;
constexpr unsigned pc=0x80010000;
static void Setup(HostRuntime& r,const char* bridge,bool record,unsigned residual,
                  unsigned attribute,unsigned base,unsigned stride,bool leaf=false) {
  // Reuse the owner allocation, but reset all hardware and memory between
  // cases. Each comparison still starts with independent FIFO/CPU state.
  r.ShutdownNativeGX();r.Reset();
  CHECK(r.InitializeNativeGX(bridge));auto& c=r.Cpu();
  auto write=[&](unsigned ea,unsigned v,unsigned size){c.external_write(&c,ea,v,size);};
  auto pair=[&](unsigned off,unsigned v){write(0xcc000000+off,v,2);write(0xcc000002+off,v>>16,2);};
  pair(0x20,0x20000);pair(0x24,0x2007c);pair(0x34,0x20000);pair(0x38,0x20000);
  write(0xcc00300c,record?0x40000:0x20000,4);write(0xcc003010,record?0x4007c:0x2007c,4);
  write(0xcc003014,record?0x40000:0x20000,4);write(0xcc000002,0x11,2);
  if(leaf) {
    for(unsigned i=0;i<17;++i)CHECK(r.Memory().Write32(pc+4*i,GekkoAOTSdk::kGxSetArrayLeafBody[i]));
  } else {
    for(unsigned i=0;i<35;++i)CHECK(r.Memory().Write32(pc+4*i,GekkoAOTSdk::kGxSetArrayBody[i]));
  }
  for(unsigned i=0;i<32;++i){c.gpr[i]=0x12340000+i;c.fpr[i]=i+.5;c.ps1[i]=-double(i)-.25;}
  c.pc=pc;c.lr=0x80011000;c.gpr[3]=attribute;c.gpr[4]=base;c.gpr[5]=stride;
  c.cr=0x23456789;c.xer=0xe0000000;c.fpscr=0x12345678;
  for(unsigned i=0;i<residual;++i)write(0xcc008000,0xa0+i,1);
}
// Execute the actual PPC fixture independently, including volatile GPRs,
// CR0, XER carry and ordered scalar stores. No native packet formula is used.
static void GuestReference(HostRuntime& r) {
  auto& c=r.Cpu();unsigned steps=0;
  const auto compare=[&](int a,int b){c.cr=(c.cr&0x0fffffffu)|((a<b?8u:a>b?4u:2u)<<28)|((c.xer&0x80000000u)>>3);};
  while(c.pc!=c.lr) {
    CHECK(++steps<40);unsigned w=0;CHECK(r.Memory().Read32(c.pc,&w));
    const auto op=w>>26,rd=(w>>21)&31,ra=(w>>16)&31;
    const auto imm=static_cast<std::int16_t>(w);const auto next=c.pc+4;
    switch(op) {
    case 11:compare(static_cast<std::int32_t>(c.gpr[ra]),imm);break;
    case 13:{const auto sum=std::uint64_t(c.gpr[ra])+static_cast<unsigned>(imm);c.gpr[rd]=static_cast<unsigned>(sum);
      c.xer=(c.xer&~0x20000000u)|(sum>>32?0x20000000u:0u);compare(static_cast<std::int32_t>(c.gpr[rd]),0);break;}
    case 14:c.gpr[rd]=(ra?c.gpr[ra]:0u)+static_cast<unsigned>(imm);break;
    case 15:c.gpr[rd]=(ra?c.gpr[ra]:0u)+(static_cast<unsigned>(imm)<<16);break;
    case 16:{const bool bit=(c.cr>>(31-ra))&1;CHECK(rd==4||rd==12);if(bit==(rd==12)){c.pc+=static_cast<std::int16_t>(w&0xfffc);continue;}break;}
    case 19:{const bool unconditional=w==0x4e800020;const bool bit=(c.cr>>(31-ra))&1;
      CHECK(unconditional||rd==4||rd==12);if(unconditional||bit==(rd==12)){c.pc=c.lr;continue;}break;}
    case 21:{const auto sh=(w>>11)&31,mb=(w>>6)&31,me=(w>>1)&31;unsigned mask=0;
      for(unsigned b=0;b<32;++b)if(mb<=me?(b>=mb&&b<=me):(b>=mb||b<=me))mask|=1u<<(31-b);
      const auto value=c.gpr[rd];c.gpr[ra]=(sh?((value<<sh)|(value>>(32-sh))):value)&mask;break;}
    case 24:c.gpr[ra]=c.gpr[rd]|(w&0xffffu);break;
    case 36:case 38:c.external_write(&c,(ra?c.gpr[ra]:0u)+static_cast<unsigned>(imm),c.gpr[rd],op==36?4:1);break;
    default:CHECK(false);
    }
    c.pc=next;
  }
}
struct Result {CPUState cpu;std::array<unsigned char,128> fifo;std::vector<unsigned char> gpu;unsigned pending,pi,cp;};
static Result Run(HostRuntime& r,const char* bridge,bool native,bool record,unsigned residual,unsigned attr,unsigned base,unsigned stride,bool leaf) {
  Setup(r,bridge,record,residual,attr,base,stride,leaf);
  if(native)CHECK(HardwareTestAccess::Call(r));else GuestReference(r);
  Result out{};out.cpu=r.Cpu();out.cpu.ram=nullptr;out.cpu.exram=nullptr;out.cpu.external_user_data=nullptr;
  out.pending=HardwareTestAccess::Pending(r);out.pi=r.ProcessorInterface().FifoWritePointer();out.cp=r.CommandProcessor().FifoReadPointer();
  const auto missing=out.pending?32-out.pending:0;
  for(unsigned i=0;i<missing;++i)r.Cpu().external_write(&r.Cpu(),0xcc008000,0,1);
  CHECK(HardwareTestAccess::Flush(r));std::memcpy(out.fifo.data(),r.Memory().Resolve(record?0x40000:0x20000,128),128);
  for(unsigned i=0;i<gekkoaot_test_gx_bytes();++i)out.gpu.push_back(gekkoaot_test_gx_byte(i));
  return out;
}
int main(int argc,char** argv) {
  CHECK(argc==2);auto r=std::make_unique<HostRuntime>();Setup(*r,argv[1],true,0,9,0x80050000,12);
  const auto* body=r->Memory().Resolve(pc,140);CHECK(GekkoAOTSdk::ProveGxSetArray(body,140));CHECK(!GekkoAOTSdk::ProveGxSetArray(body,139));
  for(unsigned i=0;i<35;++i)for(unsigned bit=0;bit<32;++bit) {
    CHECK(r->Memory().Write32(pc+4*i,GekkoAOTSdk::kGxSetArrayBody[i]^(1u<<bit)));
    CHECK(!GekkoAOTSdk::ProveGxSetArray(body,140));CHECK(r->Memory().Write32(pc+4*i,GekkoAOTSdk::kGxSetArrayBody[i]));
  }
  CHECK(r->Memory().Write32(pc+60,0x806d8fe8));CHECK(r->Memory().Write32(pc+120,0x806d8fe8));CHECK(GekkoAOTSdk::ProveGxSetArray(body,140));
  CHECK(r->Memory().Write32(pc+60,0x8062a778));CHECK(r->Memory().Write32(pc+120,0x8062a778));CHECK(GekkoAOTSdk::ProveGxSetArray(body,140));
  CHECK(r->Memory().Write32(pc+120,0x806da778));CHECK(!GekkoAOTSdk::ProveGxSetArray(body,140));
  Setup(*r,argv[1],true,0,9,0x80050000,12,true);
  body=r->Memory().Resolve(pc,68);bool leaf=false;
  CHECK(GekkoAOTSdk::ProveGxSetArray(body,68,&leaf)&&leaf);
  CHECK(!GekkoAOTSdk::ProveGxSetArray(body,67));
  for(unsigned i=0;i<17;++i)for(unsigned bit=0;bit<32;++bit) {
    CHECK(r->Memory().Write32(pc+4*i,GekkoAOTSdk::kGxSetArrayLeafBody[i]^(1u<<bit)));
    CHECK(!GekkoAOTSdk::ProveGxSetArray(body,68));
    CHECK(r->Memory().Write32(pc+4*i,GekkoAOTSdk::kGxSetArrayLeafBody[i]));
  }
  const bool enabled=!std::getenv("GEKKOAOT_NATIVE_GX_ARRAY_SERVICE")||std::getenv("GEKKOAOT_NATIVE_GX_ARRAY_SERVICE")[0]!='0';
  if(enabled) {
    for(bool short_leaf:{false,true})for(unsigned attr:{9u,10u,20u,21u,24u,25u})for(unsigned base:{0x80050000u,0xffffffffu})for(unsigned stride:{12u,0xffffffffu})
      for(bool record:{false,true})for(unsigned residual=0;residual<32;++residual) {
        if(!short_leaf&&(attr==21u||attr==24u))continue;
        const auto native=Run(*r,argv[1],true,record,residual,attr,base,stride,short_leaf),reference=Run(*r,argv[1],false,record,residual,attr,base,stride,short_leaf);
        CHECK(native.fifo==reference.fifo&&native.gpu==reference.gpu&&native.pending==reference.pending&&native.pi==reference.pi&&native.cp==reference.cp);
        CHECK(std::memcmp(&native.cpu,&reference.cpu,sizeof(CPUState))==0);
      }
  }
  for(bool short_leaf:{false,true})for(unsigned attr:{0u,8u,21u,24u,26u,0xffffffffu,9u}) {
    if(enabled&&(attr==9u||(short_leaf&&(attr==21u||attr==24u))))continue;
    Setup(*r,argv[1],true,0,attr,0x80050000,12,short_leaf);const auto before=r->Cpu();
    CHECK(!HardwareTestAccess::Call(*r));CHECK(std::memcmp(&before,&r->Cpu(),sizeof before)==0&&HardwareTestAccess::Pending(*r)==0);
  }
  std::cout<<"GXSetArray: both SDK bodies, mutation proof, relocated SDA, 2560 full CPU/PPC differential cases, all partial gathers, CPU/GPU FIFOs and independent fallback passed\n";
}
