#include "../../runtime/gx/native_vertex_program.h"
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <random>
#include <vector>
#if defined(__unix__)
#include <sys/mman.h>
#include <unistd.h>
#endif
#define check(x) do { if (!(x)) { std::cerr << "failed at " << __LINE__ << ": " << #x << '\n'; std::abort(); } } while (false)
using Program = GekkoAOT::GX::NativeVertexProgram;
static void reference(const std::uint8_t* data,unsigned count,unsigned stride,unsigned offset,
                      unsigned fields,std::span<Program::Bounds> bounds) {
  for(unsigned i=0;i<fields;++i) bounds[i]={};
  for(unsigned v=0;v<count;++v,data+=stride) for(unsigned f=0;f<fields;++f) {
    const auto* p=data+offset+2*f;
    const auto index=(std::uint32_t(p[0])<<8)|p[1];
    bounds[f].minimum=std::min(bounds[f].minimum,index);
    bounds[f].maximum=std::max(bounds[f].maximum,index);
  }
}
int main() {
  std::mt19937 random(315);
  std::array<std::uint16_t,4> offsets;
  std::array<Program::Bounds,4> expected,actual;
  Program program;
  unsigned cases=0;
  for(unsigned fields=2;fields<=4;++fields) for(unsigned start=0;start<8;++start)
    for(unsigned padding=0;padding<8;++padding) {
      const unsigned stride=start+2*fields+padding;
      for(unsigned f=0;f<fields;++f) offsets[f]=start+2*f;
      check(program.Compile(stride,{offsets.data(),fields}));
      for(unsigned pass=0;pass<512;++pass) {
        const unsigned count=pass<16?pass:random()%256;
        std::vector<std::uint8_t> data(count*stride);
        for(auto& byte:data) byte=random();
        // Explicit unsigned boundary values: signed SIMD comparison must be
        // biased, including exact 0/32767/32768/65535 resource windows.
        if(pass<4) for(unsigned v=0;v<count;++v) for(unsigned f=0;f<fields;++f) {
          const unsigned edge=std::array<unsigned,4>{0,32767,32768,65535}[(pass+f)%4];
          data[v*stride+start+2*f]=edge>>8;data[v*stride+start+2*f+1]=edge;
        }
        reference(data.data(),count,stride,start,fields,expected);
        check(program.Execute(data,count,actual));
        for(unsigned f=0;f<fields;++f)
          check(actual[f].minimum==expected[f].minimum && actual[f].maximum==expected[f].maximum);
        if(!data.empty()) check(!program.Execute(std::span(data.data(),data.size()-1),count,actual));
        ++cases;
      }
    }
  check(!program.Compile(2,{}));
  offsets={0,2,5,6};check(!program.Compile(8,offsets));
  offsets={0,2,4,6};check(!program.Compile(7,offsets));
  check(!program.Compile(65536,offsets));
#if defined(__unix__)
  const auto page=static_cast<std::size_t>(sysconf(_SC_PAGESIZE));
  auto* bytes=static_cast<std::uint8_t*>(mmap(nullptr,page*2,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0));
  check(bytes!=MAP_FAILED);check(mprotect(bytes+page,page,PROT_NONE)==0);
  for(unsigned fields=2;fields<=4;++fields) {
    check(program.Compile(fields*2,{offsets.data(),fields}));
    auto* last=bytes+page-2*fields;
    for(unsigned i=0;i<2*fields;++i) last[i]=random();
    check(program.Execute({last,2*fields},1,actual));
    reference(last,1,fields*2,0,fields,expected);
    for(unsigned f=0;f<fields;++f) check(actual[f].minimum==expected[f].minimum && actual[f].maximum==expected[f].maximum);
  }
  check(munmap(bytes,page*2)==0);
#endif
  // Measure the representative three-field/short-draw program in isolation.
  check(program.Compile(7,{offsets.data(),3}));
  std::array<std::uint8_t,28> data;
  for(auto& byte:data) byte=random();
  volatile std::uint64_t digest=0;
  for(unsigned native=0;native<2;++native) {
    const auto begin=std::chrono::steady_clock::now();
    for(unsigned i=0;i<1000000;++i) {
      data[0]=i;
      if(native) check(program.Execute(data,4,actual));
      else reference(data.data(),4,7,0,3,actual);
      digest=digest+actual[0].maximum+actual[1].minimum;
    }
    const auto ns=std::chrono::duration<double,std::nano>(std::chrono::steady_clock::now()-begin).count();
    std::cout << "native vertex program benchmark native=" << native << " ns_per_draw=" << ns/1000000 << '\n';
  }
  std::cout << "native vertex program PASS cases=" << cases << " exact_unsigned_bounds=1 guarded_tail=1 digest=" << digest << '\n';
}
