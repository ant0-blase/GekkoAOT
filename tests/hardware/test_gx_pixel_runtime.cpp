// SPDX-License-Identifier: GPL-3.0-or-later
#include "native/runtime.h"
#include "sdk_fixture_words.h"
#include "sdk/gx_pixel_service.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>
#define CHECK(x) do{if(!(x)){std::fprintf(stderr,"%d: %s\n",__LINE__,#x);std::exit(1);}}while(false)
extern "C" unsigned gekkoaot_test_gx_bytes();
extern "C" void gekkoaot_test_gx_pixel_capability(unsigned);
extern "C" unsigned char gekkoaot_test_gx_byte(unsigned);
extern "C" unsigned gekkoaot_test_gx_command_count();
extern "C" unsigned gekkoaot_test_gx_command(unsigned);
namespace GekkoAOT::Native {
struct HardwareTestAccess {
  static bool Call(HostRuntime& r,SimpleHleKind kind){return r.DispatchSimpleHle(kind,r.Cpu().pc);}
  static unsigned Pending(HostRuntime& r){return r.cpu_fifo_gather_size_+r.gpu_fifo_gather_size_;}
  static bool Flush(HostRuntime& r){return r.native_gx_.FlushWrites();}
  static void Profile(HostRuntime& r,bool enabled){r.runtime_profile_enabled_=enabled;}
  static const auto& EntryCalls(HostRuntime& r){return r.native_sdk_entry_calls_;}
};
}
using namespace GekkoAOT::Native;
int main(int argc,char** argv){
  CHECK(argc==2 || argc==4);auto runtime=std::make_unique<HostRuntime>();auto& r=*runtime;
  if(argc==4)CHECK(r.LoadModule(argv[2])==ModuleStatus::Ok);
  const auto& services=HostRuntime::NativeSdkServices();
  CHECK(services.version==2 && services.size==sizeof services && services.count==GekkoAOTNativeSdkCount);
  for(auto service:services.entries)CHECK(service && !service(nullptr));
  for(unsigned route=0;route<(argc==4?3u:2u);++route)for(unsigned family=0;family<4;++family)for(unsigned residual=0;residual<32;++residual)for(unsigned transport=0;transport<3;++transport){
    r.ShutdownNativeGX();r.Reset();gekkoaot_test_gx_pixel_capability(transport!=2);
    CHECK(r.InitializeNativeGX(argv[1]));
    const bool recording=transport==1;const unsigned fifo=recording?0x40000:0x20000;
    HardwareTestAccess::Profile(r,route>0);
    auto& cpu=r.Cpu();auto& memory=r.Memory();
    auto write=[&](unsigned address,unsigned value,unsigned bytes){cpu.external_write(&cpu,address,value,bytes);};
    auto pair=[&](unsigned off,unsigned value){write(0xcc000000+off,value,2);write(0xcc000002+off,value>>16,2);};
    pair(0x20,0x20000);pair(0x24,0x2007c);pair(0x34,0x20000);pair(0x38,0x20000);
    write(0xcc00300c,fifo,4);write(0xcc003010,fifo+0x7c,4);write(0xcc003014,fifo,4);write(0xcc000002,0x11,2);
    const auto kind=SimpleHleKind(unsigned(SimpleHleKind::GxSetAlphaCompare)+family);
    // Secondary manifests arrive before their PPC overlay bytes. Admission
    // must defer the complete body proof until execution, never bind to the
    // bootstrap chunk that formerly occupied the same guest address.
    const unsigned entry=0x80010000+0x100*family;
    r.RegisterSimpleHle(kind,entry);
    const auto f=GekkoAOTSdk::GxPixelFamily(family);const auto body=GxPixelFixture(f);
    for(unsigned i=0;i<body.size();++i)CHECK(memory.Write32(entry+4*i,body[i]));
    for(unsigned i=0;i<32;++i){cpu.gpr[i]=0x12345600u+i;cpu.fpr[i]=i+.25;cpu.ps1[i]=-double(i)-.5;}
    cpu.gpr[13]=0x80300000;cpu.gpr[3]=1;cpu.gpr[4]=3;cpu.gpr[5]=1;cpu.gpr[6]=2;cpu.gpr[7]=0x83;
    cpu.cr=0x23456789;cpu.xer=0xe0000000;cpu.fpscr=0x12345678;cpu.pc=entry;cpu.lr=0x80011000;
    CHECK(memory.Write32(0x802f8b98,0x80050000));
    CHECK(memory.Write32(0x80050000,0xfeedabcd));
    const unsigned offset=family==1?0x1d8:0x1d0,cached=family==1?0x40000017:0x41000018;
    CHECK(memory.Write32(0x80050000+offset,cached));
    std::vector<unsigned char> expected;for(unsigned i=0;i<residual;++i){expected.push_back(0xa0+i);write(0xcc008000,expected.back(),1);}
    const auto before=cpu;auto reference=cpu;
    const auto word=GekkoAOTSdk::ExecuteGxPixelLeaf(f,reference.gpr,0x80050000,cached);
    // Invoke the compiled module, rather than Run's entry interception, so
    // this actually exercises the call emitted by LLVM.
    if(route==2){cpu.downcount=-1024;CHECK(r.ModuleLibraryHandle().Descriptor()->dispatch(&cpu,entry));}
    else CHECK(route ? services.entries[GekkoAOTSdkSimple_GxSetAlphaCompare+family](&cpu) : cpu.host_call(&cpu,0xfffc0000u|unsigned(kind)));
    CHECK(std::memcmp(cpu.gpr,reference.gpr,sizeof cpu.gpr)==0);
    CHECK(std::memcmp(cpu.fpr,before.fpr,sizeof cpu.fpr)==0 && std::memcmp(cpu.ps1,before.ps1,sizeof cpu.ps1)==0);
    CHECK(cpu.cr==before.cr && cpu.xer==before.xer && cpu.fpscr==before.fpscr && cpu.pc==cpu.lr && cpu.lr==before.lr);
    unsigned actual=0;CHECK(memory.Read32(0x80050000,&actual) && actual==0xfeed0000);
    CHECK(memory.Read32(0x80050000+offset,&actual) && actual==(family==0?cached:word));
    expected.push_back(0x61);for(int shift=24;shift>=0;shift-=8)expected.push_back(word>>shift);
    const auto pending=HardwareTestAccess::Pending(r);CHECK(pending==expected.size()%32);
    if(pending)for(unsigned i=pending;i<32;++i){expected.push_back(0);write(0xcc008000,0,1);}
    CHECK(HardwareTestAccess::Flush(r));CHECK(gekkoaot_test_gx_bytes()==(recording?0:expected.size()));
    for(unsigned i=0;i<expected.size();++i){if(!recording)CHECK(gekkoaot_test_gx_byte(i)==expected[i]);CHECK(memory.Resolve(0x80000000+fifo,128)[i]==expected[i]);}
    unsigned hints=0;
    for(unsigned i=0;i+2<gekkoaot_test_gx_command_count();++i)if(gekkoaot_test_gx_command(i)==0x50495800){
      CHECK(gekkoaot_test_gx_command(i+1)==word>>24 && gekkoaot_test_gx_command(i+2)==(word&0xffffff));++hints;
    }
    const bool guest_module=argc==4 && std::atoi(argv[3])<0;
    CHECK(hints==unsigned(transport==0 && !(route==2 && guest_module)));
    // Mutations at both ends must invalidate a warm full-body proof before
    // changing registers, memory or FIFO, including raw writes without epochs.
    for(const auto word_index:{0u,unsigned(body.size()-1)}) {
      CHECK(memory.Write32(entry+4*word_index,body[word_index]^1));cpu.pc=entry;const auto unchanged=cpu;
      CHECK(!(route ? services.entries[GekkoAOTSdkSimple_GxSetAlphaCompare+family](&cpu) : cpu.host_call(&cpu,0xfffc0000u|unsigned(kind))));
      CHECK(std::memcmp(&cpu,&unchanged,sizeof cpu)==0 && HardwareTestAccess::Pending(r)==0);
      CHECK(memory.Write32(entry+4*word_index,body[word_index]));
    }
  }
  if(argc==4)for(unsigned i=GekkoAOTSdkSimple_GxSetAlphaCompare;i<=GekkoAOTSdkSimple_GxSetAlphaUpdate;++i){
    const auto calls=r.NativeSdkDirectCalls()[i];
    const auto expected=std::atoi(argv[3])>0?192u:96u;
    if(calls!=expected)std::fprintf(stderr,"named service calls: actual=%llu expected=%u\n",static_cast<unsigned long long>(calls),expected);
    CHECK(calls==expected);
    const auto family=i-GekkoAOTSdkSimple_GxSetAlphaCompare;
    const auto key=(std::uint64_t(0x80010000+256*family)<<32u)|i;
    const auto& entry=HardwareTestAccess::EntryCalls(r).at(key);
    CHECK(entry.accepted==expected && entry.declined==384u);
  }
  std::printf("GX pixel runtime: %u cases, three FIFO transports, all gather offsets, exact CPU/GXData/FIFO, typed metadata and mutation decline passed\n",argc==4?1152u:768u);
}
