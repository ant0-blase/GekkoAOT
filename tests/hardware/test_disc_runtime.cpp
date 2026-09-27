// SPDX-License-Identifier: GPL-3.0-or-later
#include "native/runtime.h"
#include <chrono>
#include <fstream>
#include <iostream>
#include <cstdlib>
#include <cstring>
#define CHECK(x) do { if (!(x)) { std::cerr << __LINE__ << ": " #x "\n"; std::exit(1); } } while(false)
namespace GekkoAOT::Native {
struct HardwareTestAccess {
  static bool Read(HostRuntime& r, unsigned offset, void* out, unsigned size) { return r.ReadDiscImage(offset,out,size); }
  static void Complete(HostRuntime& r) { r.ServiceDiscCompletion(UINT64_MAX); }
  static auto Lookup(HostRuntime& r, const char* path) { return r.native_vfs_.FindPath(path); }
};
}
using namespace GekkoAOT::Native;
int main() {
  struct Temp {
    std::filesystem::path path=std::filesystem::temp_directory_path()/
      ("gekkoaot-disc-test-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+".iso");
    ~Temp() { std::error_code ec; std::filesystem::remove(path,ec); }
  } temp;
  std::vector<std::uint8_t> image(0x10000);
  const auto be=[&](unsigned a,unsigned v) { for(unsigned i=0;i<4;++i) image[a+i]=v>>(24-8*i); };
  be(0x1c,0xc2339f3d); be(0x420,0x2000);
  be(0x424,0x3000); be(0x428,33); be(0x42c,33);
  be(0x2000,0x100); be(0x2048,0x80004000); be(0x2090,4);
  be(0x20e0,0x80004000); be(0x2100,0x4e800020); // synthetic blr
  be(0x3000,0x01000000); be(0x3008,2); // root, two FST entries
  be(0x300c,1); be(0x3010,0x4000); be(0x3014,257);
  const char name[]="\0Tex.Bin"; std::memcpy(image.data()+0x3018,name,sizeof(name));
  for(unsigned i=0x4000;i<image.size();++i) image[i]=(i*37u+11u)&255u;
  { std::ofstream f(temp.path,std::ios::binary); f.write(reinterpret_cast<const char*>(image.data()),image.size()); }
  auto runtime=std::make_unique<HostRuntime>(); auto& r=*runtime;
  CHECK(r.AttachDiscImage(temp.path));
#ifdef GEKKOAOT_HAVE_NOD
  auto entry=HardwareTestAccess::Lookup(r,"/tex.BIN");
  CHECK(entry && entry->disc_offset==0x4000 && entry->size==257);
  CHECK(!HardwareTestAccess::Lookup(r,"missing"));
#endif
  std::array<std::uint8_t,300> out{};
  CHECK(HardwareTestAccess::Read(r,0x4001,out.data(),255));
  CHECK(std::equal(out.begin(),out.begin()+255,image.begin()+0x4001));
  CHECK(HardwareTestAccess::Read(r,0x40ff,out.data(),33)); // rounded EOF/padding raw fallback
  CHECK(std::equal(out.begin(),out.begin()+33,image.begin()+0x40ff));
  CHECK(HardwareTestAccess::Read(r,image.size(),out.data(),0));
  CHECK(!HardwareTestAccess::Read(r,image.size()-1,out.data(),2));
  auto& cpu=r.Cpu();
  const auto write=[&](unsigned off,unsigned val) { cpu.external_write(&cpu,0xcc006000+off,val,4); };
  const auto start=[&](unsigned disc,unsigned dest) {
    write(8,0xa8000000); write(12,disc/4); write(16,32); write(20,dest); write(24,32); write(28,3);
  };
  CHECK(r.Memory().Write32(0x80010000,0xdeadbeef));
  start(0x4000,0x10000);
  unsigned word=0; CHECK(r.Memory().Read32(0x80010000,&word) && word==0xdeadbeef);
  CHECK(r.DiscInterface().DMAControl()&1);
  HardwareTestAccess::Complete(r);
  CHECK(!(r.DiscInterface().DMAControl()&1));
  CHECK(r.DiscInterface().Status()&16);
  CHECK(std::memcmp(r.Memory().Resolve(0xc0010000,32),image.data()+0x4000,32)==0);
  // ACK then cascade a second transfer; cancellation must not publish its bytes.
  write(0,24); start(0x4020,0x10020); write(0,0x29);
  HardwareTestAccess::Complete(r);
  CHECK(r.Memory().Read32(0x80010020,&word) && word==0);
  write(0,0x48); start(0x4040,0x10020); HardwareTestAccess::Complete(r);
  CHECK(std::memcmp(r.Memory().Resolve(0x80010020,32),image.data()+0x4040,32)==0);
  std::cout << "nod exact/unaligned/EOF/FST and deferred DMA visibility/cancel/cascade passed\n";
}
