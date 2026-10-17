#pragma once
// SPDX-License-Identifier: GPL-3.0-or-later
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>

namespace GekkoAOT::Perf {
enum class Metric : unsigned {
  PresentInterval, PresentCall, SurfaceAcquire, PipelineCompile, PipelineWait,
  GxDrain, GxBackpressure, RenderQueue, IoRead, IoWait, PadSample, Count
};
inline bool Enabled() {
  static const bool enabled=[] {const auto* v=std::getenv("GEKKOAOT_LATENCY_PROFILE");return v&&v[0]=='1';}();
  return enabled;
}
inline std::uint64_t Now() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
}
// Exact bins below 512 ns, then 256 bins per power of two. Quantile values
// are upper bounds with <0.4% relative quantisation. No samples are discarded,
// no hot-path allocation or reporting lock; concurrent writers use atomics.
class Histogram {
public:
  static constexpr unsigned Bins=256+56*256;
  static unsigned Index(std::uint64_t n) {
    if(n<256)return unsigned(n);
    const unsigned e=std::bit_width(n)-1;
    return 256+(e-8)*256+unsigned((n>>(e-8))&255);
  }
  static std::uint64_t Upper(unsigned i) {
    if(i<256)return i;
    const unsigned e=(i-256)/256+8, part=(i-256)%256;
    if(e==63&&part==255)return std::numeric_limits<std::uint64_t>::max();
    return (std::uint64_t(257+part)<<(e-8))-1;
  }
  void Record(std::uint64_t n) {
    bins_[Index(n)].fetch_add(1,std::memory_order_relaxed);
    sum_.fetch_add(n,std::memory_order_relaxed);
    auto old=max_.load(std::memory_order_relaxed);
    while(old<n&&!max_.compare_exchange_weak(old,n,std::memory_order_relaxed)){}
  }
  struct Summary {std::uint64_t count=0,sum=0,max=0,p50=0,p95=0,p99=0,p999=0;};
  Summary Read() const {
    Summary s;std::array<std::uint64_t,Bins> bins;
    for(unsigned i=0;i<Bins;++i)s.count+=(bins[i]=bins_[i].load(std::memory_order_relaxed));
    s.sum=sum_.load(std::memory_order_relaxed);s.max=max_.load(std::memory_order_relaxed);
    if(!s.count)return s;
    const std::array ranks{(s.count*500+999)/1000,(s.count*950+999)/1000,
                          (s.count*990+999)/1000,(s.count*999+999)/1000};
    std::array<std::uint64_t*,4> out{&s.p50,&s.p95,&s.p99,&s.p999};
    std::uint64_t cumulative=0;unsigned rank=0;
    for(unsigned i=0;i<Bins&&rank<4;++i){cumulative+=bins[i];while(rank<4&&cumulative>=ranks[rank])*out[rank++]=Upper(i);}
    return s;
  }
private:
  std::array<std::atomic<std::uint64_t>,Bins> bins_{};
  std::atomic<std::uint64_t> sum_{0},max_{0};
};
inline auto& Histograms(){static std::array<Histogram,unsigned(Metric::Count)> histograms;return histograms;}
inline void Record(Metric metric,std::uint64_t ns){if(Enabled())Histograms()[unsigned(metric)].Record(ns);}
class Scope {
public:
  explicit Scope(Metric metric):metric_(metric),start_(Enabled()?Now():0){}
  ~Scope(){if(start_)Record(metric_,Now()-start_);}
private:Metric metric_;std::uint64_t start_;
};
inline void Report(const char* owner) {
  if(!Enabled())return;
  constexpr std::array names{"present_interval","present_call","surface_acquire","pipeline_compile",
    "pipeline_wait","gx_drain","gx_backpressure","render_queue","io_read","io_wait","pad_sample"};
  for(unsigned i=0;i<unsigned(Metric::Count);++i){
    const auto s=Histograms()[i].Read();if(!s.count)continue;
    std::fprintf(stderr,"GEKKOAOT_LATENCY_V1 owner=%s metric=%s count=%llu p50_us=%.3f p95_us=%.3f p99_us=%.3f p999_us=%.3f max_us=%.3f total_ms=%.3f quantiles=upper-bound-0.4pct\n",
      owner,names[i],static_cast<unsigned long long>(s.count),s.p50/1e3,s.p95/1e3,s.p99/1e3,s.p999/1e3,s.max/1e3,s.sum/1e6);
  }
}
}
