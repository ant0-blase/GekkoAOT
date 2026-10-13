// SPDX-License-Identifier: GPL-3.0-or-later
#include "gx/host/host_bridge.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <vector>
#define CHECK(x) do{if(!(x)){std::fprintf(stderr,"%d: %s\n",__LINE__,#x);std::exit(1);}}while(false)
extern "C" unsigned gekkoaot_test_gx_bytes();
extern "C" unsigned char gekkoaot_test_gx_byte(unsigned);
extern "C" unsigned gekkoaot_test_gx_command_count();
extern "C" unsigned gekkoaot_test_gx_command(unsigned);
struct Binding{unsigned at,index,base,stride;};
int main(int argc,char** argv) {
  CHECK(argc==2||argc==3);
  GekkoAOT::Native::AddressSpace memory;
  GekkoAOT::GX::HostBridge bridge;CHECK(bridge.Open(argv[1],memory));
  bridge.SetAsyncWritesEnabled(argc==2);
  CHECK(bridge.HasTypedArrayCommands());
  std::vector<unsigned char> bytes(4096,0);std::vector<Binding> bindings;
  for(unsigned i=0;i<96;++i) {
    Binding b{3+21*i,i%16,0x80010000+32*i,12+i};bindings.push_back(b);
    auto* p=bytes.data()+b.at;p[0]=8;p[1]=0xa0+b.index;
    for(unsigned j=0;j<4;++j)p[2+j]=(b.base&0x3fffffffu)>>(24-8*j);
    p[6]=8;p[7]=0xb0+b.index;p[11]=b.stride;
  }
  unsigned next=0;
  for(unsigned off=0;off<bytes.size();off+=32) {
    while(next<bindings.size()&&bindings[next].at<off+32) {
      const auto& b=bindings[next++];CHECK(b.at>=off);
      CHECK(bridge.StageArrayBinding(b.index,b.base,b.stride,b.at-off));
    }
    // A publication before the annotated bytes arrive must retain the hint
    // for the next batch, rather than executing it ahead of preceding commands.
    if(off==32)bridge.NotifyMemoryWrite(0x80200000,32);
    CHECK(bridge.WriteBurst(bytes.data()+off,32,0x80001000+off));
  }
  CHECK(bridge.FlushWrites());CHECK(gekkoaot_test_gx_bytes()==bytes.size());
  for(unsigned i=0;i<bytes.size();++i)CHECK(gekkoaot_test_gx_byte(i)==bytes[i]);
  unsigned position=0,annotations=0,previous=0,bulk=0;
  for(unsigned i=0;i<gekkoaot_test_gx_command_count();) {
    const auto cmd=gekkoaot_test_gx_command(i++);
    if(cmd==0x42555200)position+=gekkoaot_test_gx_command(i++);
    else if(cmd==0x4e415400){CHECK(gekkoaot_test_gx_command(i++)>0);++bulk;}
    else if(cmd==0x4d454d00){CHECK(gekkoaot_test_gx_command(i++)==0x80200000);CHECK(gekkoaot_test_gx_command(i++)==32);}
    else {
      CHECK(cmd==0x41525200);
      auto found=std::find_if(bindings.begin(),bindings.end(),[&](Binding b){return b.at==position;});
      CHECK(found!=bindings.end());CHECK(!annotations||position>previous);
      CHECK(gekkoaot_test_gx_command(i++)==found->index);CHECK(gekkoaot_test_gx_command(i++)==found->base);
      CHECK(gekkoaot_test_gx_command(i++)==found->stride);CHECK(gekkoaot_test_gx_command(i++)==0);
      previous=position;++annotations;
    }
  }
  CHECK(position==bytes.size());CHECK(annotations>=64&&annotations<=96);
  CHECK(bulk>0);
  std::printf("Typed batched commands passed: %u native hints, 4096 exact transport bytes\n",annotations);
}
