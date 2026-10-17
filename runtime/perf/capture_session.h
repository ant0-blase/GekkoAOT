// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "capture.h"
#include "latency.h"
#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <functional>
#include <stdexcept>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#if defined(__linux__)
#include <sys/syscall.h>
#include <unistd.h>
#elif defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace GekkoAOT::Capture {
inline std::uint64_t ThreadId() {
  static thread_local const auto id=[]() -> std::uint64_t {
#if defined(__linux__)
    return syscall(SYS_gettid);
#elif defined(_WIN32)
    return GetCurrentThreadId();
#else
    return std::hash<std::thread::id>{}(std::this_thread::get_id());
#endif
  }();
  return id;
}
struct Event { Kind kind{}; std::uint64_t start=0,end=0,id=0,value=0,tid=0; };
// Fixed, bounded append storage. Closing stops admission and waits for admitted
// writers before reading. Overflow is explicit; it must invalidate analyses
// requiring a complete event sequence. Histograms still count every event.
class Session {
public:
  explicit Session(std::size_t capacity=1u<<20)
      : events_(std::make_unique<Event[]>(capacity)), capacity_(capacity) {}
  Sink GetSink() { return {this,&IsEnabled,&Append}; }
  void Start() { start_=Now(); admission_.store(Running,std::memory_order_release); }
  void Stop() {
    // Close admission atomically with the in-flight count. A writer that
    // observed Running before this CAS cannot enter after closure unnoticed.
    admission_.fetch_and(~Running,std::memory_order_acq_rel);
    while(admission_.load(std::memory_order_acquire)) std::this_thread::yield();
    end_=Now();
  }
  std::size_t Size() const { return std::min(next_.load(),capacity_); }
  std::uint64_t Dropped() const { return dropped_.load(); }
  Perf::Histogram::Summary Summary(Kind kind) const { return histograms_[unsigned(kind)].Read(); }
  const Event& At(std::size_t i) const { return events_[i]; }
  void Write(const std::filesystem::path& prefix) const {
    // Reporting runs on a diagnostic control thread after the window closes.
    // No path names, guest bytes or save contents are captured in these files.
    std::ofstream csv(prefix.string()+".events.csv"), json(prefix.string()+".json.tmp"), trace(prefix.string()+".trace.json");
    if(!csv || !json || !trace) throw std::runtime_error("cannot open capture output");
    csv<<"kind,start_ns,end_ns,id,value,tid\n";
    trace<<"{\"displayTimeUnit\":\"ms\",\"traceEvents\":[";
    bool comma=false;
    for(std::size_t i=0;i<Size();++i) {
      const auto& e=events_[i];
      csv<<Names[unsigned(e.kind)]<<','<<e.start<<','<<e.end<<','<<e.id<<','<<e.value<<','<<e.tid<<'\n';
      if(comma)trace<<',';
      comma=true;
      trace<<"{\"name\":\""<<Names[unsigned(e.kind)]<<"\",\"cat\":\"runtime\",\"ph\":\"X\",\"pid\":1,\"tid\":"<<e.tid
           <<",\"ts\":"<<(e.start-start_)/1000.0<<",\"dur\":"<<(e.end-e.start)/1000.0
           <<",\"args\":{\"id\":"<<e.id<<",\"value\":"<<e.value<<"}}";
    }
    trace<<"]}\n";
    json<<"{\"schema\":1,\"start_ns\":"<<start_<<",\"end_ns\":"<<end_
        <<",\"stored_events\":"<<Size()<<",\"dropped_trace_events\":"<<Dropped()
        <<",\"aot_trace_stride\":256,\"physical_scanout_measured\":false,\"metrics\":[";
    comma=false;
    for(unsigned i=0;i<unsigned(Kind::Count);++i) {
      const auto s=histograms_[i].Read(); if(!s.count)continue;
      if(comma)json<<',';
      comma=true;
      json<<"{\"name\":\""<<Names[i]<<"\",\"count\":"<<s.count<<",\"total_ns\":"<<s.sum
          <<",\"p50_ns\":"<<s.p50<<",\"p95_ns\":"<<s.p95<<",\"p99_ns\":"<<s.p99<<",\"max_ns\":"<<s.max<<'}';
    }
    json<<"]}\n";
    csv.flush(); trace.flush(); json.flush();
    if(!csv || !json || !trace) throw std::runtime_error("cannot write capture output");
    csv.close(); trace.close(); json.close();
    std::filesystem::rename(prefix.string()+".json.tmp",prefix.string()+".json");
  }
private:
  static bool IsEnabled(void* p) { return (static_cast<Session*>(p)->admission_.load(std::memory_order_relaxed)&Running)!=0; }
  static void Append(void* p,Kind kind,std::uint64_t start,std::uint64_t end,std::uint64_t id,std::uint64_t value) {
    if(unsigned(kind)>=unsigned(Kind::Count))return;
    auto& s=*static_cast<Session*>(p);
    auto state=s.admission_.load(std::memory_order_relaxed);
    do {
      if(!(state&Running))return;
    } while(!s.admission_.compare_exchange_weak(state,state+1u,
                std::memory_order_acquire,std::memory_order_relaxed));
    if(start>=s.start_ && end>=start) {
      s.histograms_[unsigned(kind)].Record(end-start);
      // High-frequency AOT/SDK scopes retain all histogram samples but only
      // 1/256 timeline entries. Frame, queue, IO and wait events are not sampled.
      static thread_local std::array<unsigned,unsigned(Kind::Count)> samples{};
      const bool sampled=unsigned(kind)<4 || kind==Kind::PollPark ||
                         kind==Kind::GxParser || kind==Kind::DspWork ||
                         (kind==Kind::ParserQueueAge && end-start<100000u) ||
                         (kind==Kind::GxQueueEmpty && end-start<100000u);
      if(!sampled || (++samples[unsigned(kind)]&255u)==0u) {
        const auto n=s.next_.fetch_add(1,std::memory_order_relaxed);
        if(n<s.capacity_)s.events_[n]={kind,start,end,id,value,ThreadId()};
        else s.dropped_.fetch_add(1,std::memory_order_relaxed);
      }
    }
    s.admission_.fetch_sub(1,std::memory_order_release);
  }
  std::unique_ptr<Event[]> events_;
  std::size_t capacity_;
  static constexpr std::uint32_t Running=0x80000000u;
  std::atomic<std::uint32_t> admission_{0};
  std::atomic<std::size_t> next_{0};
  std::atomic<std::uint64_t> dropped_{0};
  std::uint64_t start_=0,end_=0;
  std::array<Perf::Histogram,unsigned(Kind::Count)> histograms_{};
};

// Arm at startup; touch PREFIX.start only after visually verifying gameplay.
// One bounded capture per process. Startup and shader warmup remain excluded.
class Controller {
public:
  Controller() {
    const char* path=std::getenv("GEKKOAOT_PERF_CAPTURE"); if(!path||!*path)return;
    prefix_=path;
    if(std::filesystem::exists(prefix_.string()+".start") ||
       std::filesystem::exists(prefix_.string()+".json")) {
      std::fprintf(stderr,"GEKKOAOT_CAPTURE_V1 phase=refused reason=existing-capture-prefix\n");
      return;
    }
    session_=std::make_unique<Session>();
    SetSink(session_->GetSink());
    worker_=std::jthread([this](std::stop_token stop) {
      std::error_code ec;
      while(!stop.stop_requested()&&!std::filesystem::exists(prefix_.string()+".start",ec))
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
      if(stop.stop_requested())return;
      session_->Start();
      std::fprintf(stderr,"GEKKOAOT_CAPTURE_V1 phase=start prefix=\"%s\"\n",prefix_.string().c_str());
      unsigned seconds=15;
      if(const char* n=std::getenv("GEKKOAOT_PERF_CAPTURE_SECONDS"))
        seconds=std::clamp(unsigned(std::strtoul(n,nullptr,10)),1u,120u);
      const auto until=std::chrono::steady_clock::now()+std::chrono::seconds(seconds);
      while(!stop.stop_requested()&&std::chrono::steady_clock::now()<until&&
            !std::filesystem::exists(prefix_.string()+".stop",ec))
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
      session_->Stop();
      { std::ofstream done(prefix_.string()+".window-end"); done<<Now(); }
      try {
        session_->Write(prefix_);
        std::fprintf(stderr,"GEKKOAOT_CAPTURE_V1 phase=written events=%zu dropped=%llu\n",session_->Size(),
                     static_cast<unsigned long long>(session_->Dropped()));
      } catch(const std::exception& error) {
        std::fprintf(stderr,"GEKKOAOT_CAPTURE_V1 phase=error detail=\"%s\"\n",error.what());
      }
    });
  }
  void Finish() { if(worker_.joinable()){worker_.request_stop();worker_.join();} }
  ~Controller() { Finish(); SetSink({}); }
  Sink GetSink() { return session_?session_->GetSink():Sink{}; }
private:
  std::filesystem::path prefix_;
  std::unique_ptr<Session> session_;
  std::jthread worker_;
};
} // namespace GekkoAOT::Capture
