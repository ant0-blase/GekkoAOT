// SPDX-License-Identifier: GPL-3.0-or-later
// End-to-end generated-module benchmark of actual GX pixel state, not a stub.
#include "native/runtime.h"
#include "sdk_fixture_words.h"
#include "sdk/gx_pixel_service.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string_view>
using namespace GekkoAOT::Native;
int main(int argc,char** argv){
  if(argc!=4 && argc!=5)return 2;
  const bool recording=argc==5 && std::string_view(argv[4])=="recording";
  const auto fifo=recording?0x40000u:0x20000u;
  auto runtime=std::make_unique<HostRuntime>();auto& r=*runtime;
  if(r.LoadModule(argv[2])!=ModuleStatus::Ok || !r.InitializeNativeGX(argv[1]))return 3;
  auto& cpu=r.Cpu();auto& m=r.Memory();
  const auto write=[&](unsigned address,unsigned value,unsigned size){cpu.external_write(&cpu,address,value,size);};
  const auto pair=[&](unsigned off,unsigned value){write(0xcc000000+off,value,2);write(0xcc000002+off,value>>16,2);};
  pair(0x20,0x20000);pair(0x24,0x2007c);pair(0x34,0x20000);pair(0x38,0x20000);
  write(0xcc00300c,fifo,4);write(0xcc003010,fifo+0x7c,4);write(0xcc003014,fifo,4);write(0xcc000002,0x11,2);
  const auto body=GxPixelFixture(GekkoAOTSdk::GxPixelFamily::AlphaCompare);
  for(unsigned i=0;i<body.size();++i)if(!m.Write32(0x80010000+i*4,body[i]))return 4;
  r.RegisterSimpleHle(SimpleHleKind::GxSetAlphaCompare,0x80010000);
  cpu.gpr[13]=0x80300000;m.Write32(0x802f8b98,0x80050000);m.Write32(0x800501d0,0x41000018);
  const auto dispatch=r.ModuleLibraryHandle().Descriptor()->dispatch;
  const auto call=[&](unsigned i){
    cpu.pc=0x80010000;cpu.lr=0x80011000;cpu.downcount=0;
    cpu.gpr[3]=i&7;cpu.gpr[4]=i&255;cpu.gpr[5]=(i>>3)&3;cpu.gpr[6]=(i>>5)&7;cpu.gpr[7]=(i>>7)&255;
    if(!dispatch(&cpu,cpu.pc)||cpu.pc!=cpu.lr)std::abort();
  };
  for(unsigned i=0;i<4096;++i)call(i);
  const unsigned count=std::strtoul(argv[3],nullptr,0);if(!count)return 5;
  std::uint64_t guest_cycles=0;
  const auto start=std::chrono::steady_clock::now();
  for(unsigned i=0;i<count;++i){call(i);if(cpu.downcount<0)guest_cycles+=std::uint64_t(-cpu.downcount);}
  const auto ns=std::chrono::duration<double,std::nano>(std::chrono::steady_clock::now()-start).count();
  const auto direct=r.NativeSdkDirectCalls()[GekkoAOTSdkSimple_GxSetAlphaCompare];
  std::printf("{\"calls\":%u,\"wall_ns_per_call\":%.3f,\"named_calls_including_warmup\":%llu,\"guest_aot_charged_cycles\":%llu,\"compatibility_fifo_bytes_per_call\":5,\"timing_scope\":\"generated-module-and-real-native-gx-service\"}\n",count,ns/count,(unsigned long long)direct,(unsigned long long)guest_cycles);
}
