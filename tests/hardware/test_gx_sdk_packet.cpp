// SPDX-License-Identifier: GPL-3.0-or-later
#include "native/runtime.h"
#include "sdk/gx_normal_service.h"
#include <array>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <vector>
#define CHECK(x) do{if(!(x)){std::cerr<<__LINE__<<": " #x "\n";std::exit(1);}}while(false)
extern "C" unsigned gekkoaot_test_gx_bytes();
extern "C" unsigned char gekkoaot_test_gx_byte(unsigned);
extern "C" unsigned gekkoaot_test_gx_command_count();
extern "C" unsigned gekkoaot_test_gx_command(unsigned);
namespace GekkoAOT::Native {
struct HardwareTestAccess {
  static void Fast(HostRuntime& r,bool enabled){r.native_sdk_gx_packets_enabled_=enabled;r.track_host_effects_=true;}
  static bool Call(HostRuntime& r,SimpleHleKind kind){return r.DispatchSimpleHle(kind,r.Cpu().pc);}
  static unsigned Pending(HostRuntime& r){return r.cpu_fifo_gather_size_+r.gpu_fifo_gather_size_;}
  static auto HostEffects(HostRuntime& r){return r.host_effects_;}
  static auto Packets(HostRuntime& r){return r.native_sdk_gx_packets_;}
  static bool Flush(HostRuntime& r){return r.native_gx_.FlushWrites();}
};
}
using namespace GekkoAOT::Native;
static void Setup(HostRuntime& r,const char* bridge,bool recording,unsigned residual,bool fast) {
  CHECK(r.InitializeNativeGX(bridge));HardwareTestAccess::Fast(r,fast);auto& c=r.Cpu();
  auto write=[&](unsigned ea,unsigned v,unsigned size){c.external_write(&c,ea,v,size);};
  auto pair=[&](unsigned off,unsigned v){write(0xcc000000+off,v,2);write(0xcc000002+off,v>>16,2);};
  pair(0x20,0x20000);pair(0x24,0x2007c);pair(0x34,0x20000);pair(0x38,0x20000);
  write(0xcc00300c,recording?0x40000:0x20000,4);write(0xcc003010,recording?0x4007c:0x2007c,4);
  write(0xcc003014,recording?0x40000:0x20000,4);write(0xcc000002,0x11,2);
  c.msr|=0x2000u;
  c.pc=0x80010000;c.lr=0x80011000;c.gpr[3]=0x80050000;c.gpr[4]=7;c.gpr[5]=1;
  constexpr std::array<unsigned,12> data{1,0x80000001,0x007fffff,0x80000000,0x7f800001,0xff800011,0x7f800000,0xff800000,0x3f800000,0x7fc12345,0xbf800000,0};
  for(unsigned i=0;i<12;++i)CHECK(r.Memory().Write32(c.gpr[3]+4*i,data[i]));
  for(unsigned i=0;i<residual;++i)write(0xcc008000,0xa0+i,1);
}
struct Result {CPUState cpu;std::array<unsigned char,128> fifo;std::vector<unsigned char> gpu;unsigned pending,pi,cp;};
static Result Run(const char* bridge,SimpleHleKind kind,bool recording,unsigned residual,bool fast) {
  auto r=std::make_unique<HostRuntime>();Setup(*r,bridge,recording,residual,fast);
  const auto before=HardwareTestAccess::Packets(*r);
  CHECK(HardwareTestAccess::Call(*r,kind));CHECK(r->Cpu().pc==r->Cpu().lr);
  CHECK(HardwareTestAccess::Packets(*r)==before+(fast?1:0));
  CHECK(HardwareTestAccess::Flush(*r));
  Result out{};out.cpu=r->Cpu();out.cpu.ram=nullptr;out.cpu.exram=nullptr;out.cpu.external_user_data=nullptr;
  std::memcpy(out.fifo.data(),r->Memory().Resolve(recording?0x40000:0x20000,128),128);
  for(unsigned i=0;i<gekkoaot_test_gx_bytes();++i)out.gpu.push_back(gekkoaot_test_gx_byte(i));
  out.pending=HardwareTestAccess::Pending(*r);out.pi=r->ProcessorInterface().FifoWritePointer();out.cp=r->CommandProcessor().FifoReadPointer();
  // Publish the remaining physical line, verifying subsequent custom stores.
  const auto missing=out.pending?32-out.pending:0;
  for(unsigned i=0;i<missing;++i)r->Cpu().external_write(&r->Cpu(),0xcc008000,0,1);
  CHECK(HardwareTestAccess::Flush(*r));
  std::memcpy(out.fifo.data(),r->Memory().Resolve(recording?0x40000:0x20000,128),128);
  out.gpu.clear();for(unsigned i=0;i<gekkoaot_test_gx_bytes();++i)out.gpu.push_back(gekkoaot_test_gx_byte(i));
  unsigned matrices=0;
  for(unsigned i=0;i<gekkoaot_test_gx_command_count();++i)
    matrices+=gekkoaot_test_gx_command(i)==0x4d415400u;
  if(std::getenv("GEKKOAOT_TEST_DIRECT_MATRICES")) {
    const bool enabled=std::getenv("GEKKOAOT_NATIVE_GX_DIRECT_MATRICES")[0]!='0';
    CHECK(matrices==unsigned(enabled&&fast&&!recording));
  }
  return out;
}
int main(int argc,char** argv) {
  CHECK(argc>=2);
  for(auto kind:{SimpleHleKind::GxLoadPosMtxImm,SimpleHleKind::GxLoadTexMtxImm,SimpleHleKind::GxLoadNrmMtxImm,SimpleHleKind::GxLoadNrmMtxImm3x3})
    for(bool record:{false,true})for(unsigned residual=0;residual<32;++residual) {
      const auto slow=Run(argv[1],kind,record,residual,false),fast=Run(argv[1],kind,record,residual,true);
      CHECK(slow.fifo==fast.fifo&&slow.gpu==fast.gpu);CHECK(slow.pending==fast.pending&&slow.pi==fast.pi&&slow.cp==fast.cp);
      CHECK(std::memcmp(&slow.cpu,&fast.cpu,sizeof(CPUState))==0);
    }
  auto r=std::make_unique<HostRuntime>();Setup(*r,argv[1],true,0,true);
  // Normal payload skips the fourth column, and scalar lfs/stfs preserves
  // subnormals while paired raw loads flush them. All widths are big endian.
  CHECK(HardwareTestAccess::Call(*r,SimpleHleKind::GxLoadNrmMtxImm));
  for(unsigned i=0;i<23;++i)r->Cpu().external_write(&r->Cpu(),0xcc008000,0,1);
  const auto* b=r->Memory().Resolve(0x40000,64);
  CHECK(b[0]==0x10&&b[1]==0&&b[2]==8&&b[3]==4&&b[4]==21);
  CHECK(b[13]==0&&b[14]==0x7f&&b[15]==0xff&&b[16]==0xff);
  for(auto invalid:{0xcc006000u,0x7e001ff0u}) {
    r->Cpu().pc=0x80010000;r->Cpu().gpr[3]=invalid;
    if(invalid==0x7e001ff0u) {auto& c=r->Cpu();c.msr=0x32;c.spr_write(&c,25,0x10000,c.pc);}
    const auto before=r->Cpu();const auto pending=HardwareTestAccess::Pending(*r);
    CHECK(!HardwareTestAccess::Call(*r,SimpleHleKind::GxLoadNrmMtxImm));
    CHECK(std::memcmp(&before,&r->Cpu(),sizeof before)==0&&pending==HardwareTestAccess::Pending(*r));
  }
  r->Cpu().gqr[0]=4;r->Cpu().gpr[3]=0x80050000;const auto before=r->Cpu();
  CHECK(!HardwareTestAccess::Call(*r,SimpleHleKind::GxLoadNrmMtxImm));CHECK(std::memcmp(&before,&r->Cpu(),sizeof before)==0);
  if(argc==3&&std::strcmp(argv[2],"--bench")==0) {
    for(bool fast:{false,true}) {
      r.reset();r=std::make_unique<HostRuntime>();Setup(*r,argv[1],true,0,fast);
      constexpr unsigned count=500000;const auto begin=std::chrono::steady_clock::now();
      const auto effects=HardwareTestAccess::HostEffects(*r);
      for(unsigned i=0;i<count;++i){r->Cpu().pc=0x80010000;CHECK(HardwareTestAccess::Call(*r,SimpleHleKind::GxLoadPosMtxImm));}
      const auto ns=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-begin).count();
      std::cout<<"SDK matrix fast="<<fast<<" ns/call="<<double(ns)/count<<" runtime_effects="<<HardwareTestAccess::HostEffects(*r)-effects<<"\n";
    }
  }
  std::cout<<"512 SDK matrix packets: full CPU differential, RAM/GPU bytes, all partial gathers, CP/PI pointers and VM/GQR fallback passed\n";
}
