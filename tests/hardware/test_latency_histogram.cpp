// SPDX-License-Identifier: GPL-3.0-or-later
#include "perf/latency.h"
#include <cstdlib>
#include <iostream>
#include <limits>
#include <thread>
#define CHECK(x) do{if(!(x)){std::cerr<<__LINE__<<": " #x "\n";std::exit(1);}}while(false)
int main(){
  using H=GekkoAOT::Perf::Histogram;
  H h;CHECK(h.Read().count==0);
  for(unsigned i=0;i<10000;++i)h.Record(i);
  const auto s=h.Read();CHECK(s.count==10000&&s.sum==49995000&&s.max==9999);
  CHECK(s.p50>=4999&&s.p50<5020);CHECK(s.p95>=9499&&s.p95<9540);
  CHECK(s.p99>=9899&&s.p99<9940);CHECK(s.p999>=9989&&s.p999<10030);
  for(unsigned b=0;b<64;++b){const auto n=std::uint64_t(1)<<b;CHECK(H::Upper(H::Index(n))>=n);CHECK(H::Index(n)<H::Bins);}
  CHECK(H::Upper(H::Index(std::numeric_limits<std::uint64_t>::max()))==std::numeric_limits<std::uint64_t>::max());
  H concurrent;auto writer=[&]{for(unsigned i=0;i<10000;++i)concurrent.Record(1234);};
  std::thread a(writer),b(writer);a.join();b.join();CHECK(concurrent.Read().count==20000);
  std::cout<<"Bounded latency histogram: quantiles, overflow and concurrent recording PASS\n";
}
