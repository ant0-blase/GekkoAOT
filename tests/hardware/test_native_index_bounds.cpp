// SPDX-License-Identifier: GPL-3.0-or-later
#include "gx/native_index_bounds.h"
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
#define CHECK(x) do {if(!(x)){std::cerr<<__LINE__<<": " #x "\n";std::abort();}}while(false)
#if defined(_MSC_VER)
#define NOINLINE __declspec(noinline)
#else
#define NOINLINE __attribute__((noinline))
#endif
using Plan=GekkoAOT::GX::NativeIndexBounds;
// Independent decoder: the old vertex-major loop, including per-index width
// dispatch. No compiled plans, SIMD, cached bounds or shared implementation.
static NOINLINE void Reference(std::span<const std::uint8_t> data,unsigned count,unsigned stride,
                              std::span<const Plan::Field> fields,unsigned skipped,std::span<Plan::Bounds> out) {
  struct Active {Plan::Field field;unsigned destination;Plan::Bounds bounds;};
  std::array<Active,Plan::MaxFields> active;unsigned size=0;
  for(unsigned f=0;f<fields.size();++f)if(!(skipped&(1u<<f)))active[size++]={fields[f],f,{}};
  for(unsigned v=0;v<count;++v)for(unsigned f=0;f<size;++f) {
    auto& state=active[f];const auto& field=state.field;
    for(unsigned n=0;n<field.count;++n) {
      const auto* p=data.data()+v*stride+field.offset+n*field.width;
      const unsigned index=field.width==1 ? p[0] : (unsigned(p[0])<<8)|p[1];
      state.bounds.minimum=std::min(state.bounds.minimum,index);state.bounds.maximum=std::max(state.bounds.maximum,index);
    }
  }
  for(unsigned f=0;f<size;++f)out[active[f].destination]=active[f].bounds;
}
static NOINLINE bool Native(const Plan& plan,std::span<const std::uint8_t> data,unsigned count,
                           unsigned skipped,std::span<Plan::Bounds> out) {
  return plan.Execute(data,count,skipped,out);
}
static void Compare(const Plan& plan,std::span<const std::uint8_t> data,unsigned count,unsigned stride,
                    std::span<const Plan::Field> fields,unsigned skipped) {
  std::array<Plan::Bounds,Plan::MaxFields> expected,actual;
  for(auto& b:expected)b={1234,5678};actual=expected;
  Reference(data,count,stride,fields,skipped,expected);
  CHECK(Native(plan,data,count,skipped,actual));
  for(unsigned f=0;f<Plan::MaxFields;++f)
    CHECK(actual[f].minimum==expected[f].minimum && actual[f].maximum==expected[f].maximum);
}
int main(int argc,char**) {
  std::mt19937 random(20261018);Plan plan;unsigned cases=0;
  for(unsigned fields=1;fields<=Plan::MaxFields;++fields)for(unsigned pass=0;pass<400;++pass) {
    std::array<Plan::Field,Plan::MaxFields> layout;
    unsigned stride=random()%4;
    for(unsigned f=0;f<fields;++f) {
      const unsigned width=1+random()%2,count=pass%4==0 && f%3==1?3:1;
      layout[f]={static_cast<std::uint16_t>(stride),static_cast<std::uint8_t>(width),static_cast<std::uint8_t>(count)};
      stride+=width*count+random()%3;
    }
    stride+=random()%4;
    CHECK(plan.Compile(stride,{layout.data(),fields}));
    const unsigned count=pass<33?pass:random()%512,skipped=random()&((1u<<fields)-1);
    std::vector<std::uint8_t> data(count*stride);
    for(auto& byte:data)byte=random();
    Compare(plan,data,count,stride,{layout.data(),fields},skipped);
    // Changed packet bytes must be re-read even with identical VCD/VAT.
    for(auto& byte:data)byte=pass&1?0:255;
    Compare(plan,data,count,stride,{layout.data(),fields},0);
    std::array<Plan::Bounds,Plan::MaxFields> invalid_out;
    if(!data.empty())CHECK(!Native(plan,{data.data(),data.size()-1},count,0,invalid_out));
    ++cases;
  }
  const Plan::Field good[]={{1,1,3},{4,2,1},{6,2,3}};
  CHECK(!plan.Compile(0,good));CHECK(!plan.Compile(65536,good));CHECK(!plan.Compile(11,good));
  const Plan::Field bad_width[]={{0,3,1}},bad_count[]={{0,1,2}};
  CHECK(!plan.Compile(8,bad_width));CHECK(!plan.Compile(8,bad_count));CHECK(!plan.Compile(8,{}));
  CHECK(plan.Compile(12,good));
  std::array<Plan::Bounds,3> out;
  CHECK(!plan.Execute({},65536,0,out));CHECK(!plan.Execute({},0,0,{}));
  // Unsigned endpoints and SIMD tails stop exactly at an inaccessible page.
#if defined(__unix__)
  const auto page=static_cast<std::size_t>(sysconf(_SC_PAGESIZE));
  auto* memory=static_cast<std::uint8_t*>(mmap(nullptr,2*page,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0));
  CHECK(memory!=MAP_FAILED);CHECK(!mprotect(memory+page,page,PROT_NONE));
  for(unsigned count=1;count<=40;++count) {
    auto* data=memory+page-count*12;
    for(unsigned i=0;i<count*12;++i)data[i]=random();
    Compare(plan,{data,count*12},count,12,good,0);
  }
  CHECK(!munmap(memory,2*page));
#endif
  std::cout<<"Mixed index bounds PASS cases="<<cases<<" changed-packets=1 nbt3=1 unsigned=1 skipped-fields=1 guarded-tail=1\n";
  if(argc>1) {
    const Plan::Field mixed[]={{1,2,1},{3,1,1},{4,2,1}};
    CHECK(plan.Compile(7,mixed));std::array<Plan::Bounds,3> bounds;
    for(unsigned count:{4u,32u,128u,512u}) {
      std::vector<std::uint8_t> data(count*7);for(auto& byte:data)byte=random();
      volatile std::uint64_t digest=0;
      for(unsigned repeat=0;repeat<5;++repeat)for(unsigned native=0;native<2;++native) {
        const unsigned draws=200000;
        const auto begin=std::chrono::steady_clock::now();
        for(unsigned draw=0;draw<draws;++draw) {
          data[1]=draw;
          if(native)CHECK(Native(plan,data,count,0,bounds));else Reference(data,count,7,mixed,0,bounds);
          digest=digest+bounds[0].maximum+bounds[1].minimum+bounds[2].maximum;
        }
        const auto ns=std::chrono::duration<double,std::nano>(std::chrono::steady_clock::now()-begin).count()/draws;
        std::cout<<"{\"vertices\":"<<count<<",\"native\":"<<native<<",\"repeat\":"<<repeat<<",\"ns_per_draw\":"<<ns<<",\"digest\":"<<digest<<"}\n";
      }
    }
  }
}
