#include "../../runtime/gx/native_vertex_gather.h"
#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <random>
#include <vector>
#if defined(__unix__)
#include <sys/mman.h>
#include <unistd.h>
#endif
#define check(x) do { if(!(x)) {std::cerr << "failed " << __LINE__ << ": " << #x << '\n';std::abort();} } while(false)
using Gather=GekkoAOT::GX::NativeVertexGather;
int main() {
  std::mt19937 random(318);
  Gather gather;
  Gather::Workspace host;
  Gather::Result result;
  unsigned cases=0;
  for(unsigned fields=1;fields<=4;++fields) for(unsigned n=1;n<=64;++n)
    for(unsigned pass=0;pass<4;++pass) {
      const unsigned direct=pass%5, vertex_stride=direct+2*fields+pass%3;
      std::array<Gather::Field,4> layout;
      std::array<Gather::Source,4> sources;
      std::array<std::vector<std::uint8_t>,4> arrays;
      std::vector<std::uint8_t> vertices(n*vertex_stride);
      for(auto& byte:vertices) byte=random();
      for(unsigned f=0;f<fields;++f) {
        const unsigned element=1+random()%36, stride=element+random()%8;
        layout[f]={static_cast<std::uint16_t>(direct+2*f),static_cast<std::uint16_t>(element)};
        const unsigned last_index=pass==0 ? 65535u : 255u;
        arrays[f].resize(last_index*stride+element);
        for(auto& byte:arrays[f]) byte=random();
        sources[f]={arrays[f].data(),arrays[f].size(),stride};
        for(unsigned v=0;v<n;++v) {
          unsigned index=random()%(last_index+1u);
          if(v==0) index=0;
          if(v==n-1) index=last_index;
          const auto at=v*vertex_stride+layout[f].offset;
          vertices[at]=index>>8;vertices[at+1]=index;
        }
      }
      check(gather.Compile(vertex_stride,{layout.data(),fields}));
      const auto original=vertices;
      check(gather.Execute(vertices,n,{sources.data(),fields},host,result));
      check(original==vertices);
      check(result.bytes<=host.storage.size());
      for(unsigned v=0;v<n;++v) for(unsigned b=0;b<vertex_stride;++b) {
        bool index=false;
        for(unsigned f=0;f<fields;++f) index|=b==layout[f].offset || b==layout[f].offset+1u;
        if(!index) check(host.vertices[v*vertex_stride+b]==vertices[v*vertex_stride+b]);
      }
      for(unsigned f=0;f<fields;++f) for(unsigned v=0;v<n;++v) {
        const auto at=v*vertex_stride+layout[f].offset;
        check(host.vertices[at]==0 && host.vertices[at+1]==v);
        const unsigned index=(unsigned(vertices[at])<<8)|vertices[at+1];
        const auto* cpu=sources[f].data+std::size_t(index)*sources[f].stride;
        const auto* gpu=host.storage.data()+result.ranges[f].offset+v*sources[f].stride;
        check(std::memcmp(cpu,gpu,layout[f].element)==0);
        if(v+1!=n) for(unsigned b=layout[f].element;b<sources[f].stride;++b) check(gpu[b]==0);
      }
      const auto last=(unsigned(vertices[(n-1)*vertex_stride+layout[0].offset])<<8)|vertices[(n-1)*vertex_stride+layout[0].offset+1];
      arrays[0][std::size_t(last)*sources[0].stride]^=0xff;
      check(gather.Execute(vertices,n,{sources.data(),fields},host,result));
      check(host.storage[result.ranges[0].offset+(n-1)*sources[0].stride]==arrays[0][std::size_t(last)*sources[0].stride]);
      --sources[0].available;
      check(!gather.Execute(vertices,n,{sources.data(),fields},host,result));
      ++cases;
    }
  const std::array<Gather::Field,2> overlapping{{{0,12},{1,8}}};
  check(!gather.Compile(8,overlapping));
  check(!gather.Compile(257,{}));
  check(!gather.Compile(1,std::span(overlapping.data(),1)));
#if defined(__unix__)
  const auto page=std::size_t(sysconf(_SC_PAGESIZE));
  auto* data=static_cast<std::uint8_t*>(mmap(nullptr,2*page,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0));
  check(data!=MAP_FAILED);check(mprotect(data+page,page,PROT_NONE)==0);
  const std::array<Gather::Field,1> field{{{0,3}}};
  check(gather.Compile(2,field));
  const std::array<std::uint8_t,2> vertex{{0,0}};
  const std::array<Gather::Source,1> source{{{data+page-3,3,7}}};
  check(gather.Execute(vertex,1,source,host,result));
  check(!gather.Execute(vertex,0,source,host,result));
  check(!gather.Execute(vertex,65,source,host,result));
  check(munmap(data,2*page)==0);
#endif
  std::cout << "native vertex gather PASS cases=" << cases << " sparse=65535 mutations=exact padding=deterministic source_bounds=guarded direct_attributes=unchanged\n";
}
