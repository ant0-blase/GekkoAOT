// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "native_samples.h"
#include "capture.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <thread>

#if defined(__linux__) && !defined(__ANDROID__) && (defined(__x86_64__) || defined(__aarch64__))
#include <csignal>
#include <pthread.h>
#include <sys/syscall.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

namespace GekkoAOT::Perf {
namespace {
constexpr std::uint32_t Open=0x80000000u;
constexpr std::size_t Capacity=65536, MaxThreads=128;
static_assert(std::atomic<std::uint32_t>::is_always_lock_free);
static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
struct Sample { std::uint64_t ns=0,pc=0; std::uint32_t slot=0,overruns=0; };
struct ThreadTimer {
  timer_t timer{};
  clockid_t clock{};
  std::uint64_t tid=0,cpu_start=0,cpu_end=0;
  bool created=false;
  int error=0;
};
struct State {
  std::atomic<std::uint32_t> admission{0},threads{0},next{0};
  std::atomic<std::uint64_t> dropped{0},overruns{0};
  std::array<ThreadTimer,MaxThreads> timers{};
  std::array<Sample,Capacity> samples{};
  unsigned hz=199;
  int signal=0;
  std::uint64_t start=0,end=0;
};
// Process lifetime is intentional: queued signal handlers must never dereference
// freed storage. Only an explicitly requested profile allocates this ~1.6 MiB.
std::atomic<State*> signal_state{nullptr};
static_assert(std::atomic<State*>::is_always_lock_free);
bool Enter(State& state) noexcept {
  auto value=state.admission.load(std::memory_order_relaxed);
  do { if(!(value&Open))return false; }
  while(!state.admission.compare_exchange_weak(value,value+1u,
             std::memory_order_acquire,std::memory_order_relaxed));
  return true;
}
std::uint64_t ClockNs(clockid_t clock) noexcept {
  timespec ts{};
  if(clock_gettime(clock,&ts)!=0)return 0;
  return std::uint64_t(ts.tv_sec)*1000000000ull+ts.tv_nsec;
}
void OnSample(int, siginfo_t* info, void* context) noexcept {
  const int saved_errno=errno;
  auto* state=signal_state.load(std::memory_order_acquire);
  if(!state || !info || info->si_code!=SI_TIMER) { errno=saved_errno; return; }
  const auto address=reinterpret_cast<std::uintptr_t>(info->si_value.sival_ptr);
  const auto base=reinterpret_cast<std::uintptr_t>(state->timers.data());
  if(address<base || address>=base+sizeof(state->timers) ||
     (address-base)%sizeof(ThreadTimer)!=0 || !Enter(*state)) {
    errno=saved_errno; return;
  }
  const auto slot=static_cast<std::uint32_t>((address-base)/sizeof(ThreadTimer));
  const auto n=state->next.fetch_add(1,std::memory_order_relaxed);
  const auto lost=static_cast<std::uint32_t>(info->si_overrun>0 ? info->si_overrun : 0);
  state->overruns.fetch_add(lost,std::memory_order_relaxed);
  if(n<Capacity) {
    const auto* uc=static_cast<const ucontext_t*>(context);
#if defined(__x86_64__)
    const auto pc=static_cast<std::uint64_t>(uc->uc_mcontext.gregs[REG_RIP]);
#else
    const auto pc=static_cast<std::uint64_t>(uc->uc_mcontext.pc);
#endif
    state->samples[n]={ClockNs(CLOCK_MONOTONIC),pc,slot,lost};
  } else state->dropped.fetch_add(1,std::memory_order_relaxed);
  state->admission.fetch_sub(1,std::memory_order_release);
  errno=saved_errno;
}
// Reuse the already configured renderer diagnostic ABI. Returning false keeps
// expensive frame scopes, clocks, histograms and completion callbacks disabled.
// Each participating thread registers one CPU-time timer on its first boundary.
bool SampleBoundary(void* user) {
  auto& state=*static_cast<State*>(user);
  static thread_local State* registered=nullptr;
  if(registered==&state || !Enter(state))return false;
  registered=&state;
  const auto index=state.threads.fetch_add(1,std::memory_order_relaxed);
  if(index<MaxThreads) {
    auto& timer=state.timers[index];
    timer.tid=static_cast<std::uint64_t>(syscall(SYS_gettid));
    if((timer.error=pthread_getcpuclockid(pthread_self(),&timer.clock))==0) {
      timer.cpu_start=ClockNs(timer.clock);
      sigevent event{};
      event.sigev_notify=SIGEV_THREAD_ID;
      event.sigev_signo=state.signal;
      event.sigev_value.sival_ptr=&timer;
      event._sigev_un._tid=static_cast<pid_t>(timer.tid);
      if(timer_create(timer.clock,&event,&timer.timer)==0) {
        timer.created=true;
        itimerspec interval{};
        interval.it_value.tv_nsec=interval.it_interval.tv_nsec=1000000000u/state.hz;
        if(timer_settime(timer.timer,0,&interval,nullptr)!=0)timer.error=errno;
      } else timer.error=errno;
    }
  }
  state.admission.fetch_sub(1,std::memory_order_release);
  return false;
}
void Maps(const std::filesystem::path& path) {
  std::ifstream in("/proc/self/maps");
  std::ofstream out(path);out<<in.rdbuf();
}
void Start(State& state) {
  for(int sig=SIGRTMAX;sig>=SIGRTMIN+4;--sig) {
    struct sigaction old{};
    if(sigaction(sig,nullptr,&old)!=0 || old.sa_handler!=SIG_DFL)continue;
    struct sigaction action{};
    sigemptyset(&action.sa_mask);
    action.sa_sigaction=&OnSample;
    action.sa_flags=SA_SIGINFO|SA_RESTART;
    signal_state.store(&state,std::memory_order_release);
    if(sigaction(sig,&action,nullptr)==0) {state.signal=sig;break;}
  }
  if(!state.signal)throw std::runtime_error("no unused real-time signal for native sampling");
  state.start=ClockNs(CLOCK_MONOTONIC);
  state.admission.store(Open,std::memory_order_release);
}
void Stop(State& state) {
  state.admission.fetch_and(~Open,std::memory_order_acq_rel);
  while(state.admission.load(std::memory_order_acquire))std::this_thread::yield();
  state.end=ClockNs(CLOCK_MONOTONIC);
  for(std::size_t i=0;i<std::min<std::size_t>(state.threads.load(),MaxThreads);++i)
    {
      auto& timer=state.timers[i];
      if(timer.created) {timer_delete(timer.timer);timer.created=false;}
      timer.cpu_end=ClockNs(timer.clock);
    }
  // Keep our formerly unused signal handler installed until exit: an already
  // queued timer signal is harmless, whereas restoring SIG_DFL could kill us.
}
void Write(State& state,const std::filesystem::path& prefix) {
  std::ofstream csv(prefix.string()+".samples.csv");
  std::ofstream json(prefix.string()+".samples.json.tmp");
  if(!csv || !json)throw std::runtime_error("cannot open native sample output");
  csv<<"timestamp_ns,tid,pc,overruns\n";
  const auto count=std::min<std::size_t>(state.next.load(),Capacity);
  for(std::size_t i=0;i<count;++i) {
    const auto& sample=state.samples[i];
    csv<<sample.ns<<','<<state.timers[sample.slot].tid<<','<<std::hex<<sample.pc<<std::dec<<','<<sample.overruns<<'\n';
  }
  json<<"{\"schema\":1,\"backend\":\"linux-per-thread-cpu-timer\",\"hz\":"<<state.hz
      <<",\"start_ns\":"<<state.start<<",\"end_ns\":"<<state.end<<",\"samples\":"<<count
      <<",\"dropped\":"<<state.dropped.load()<<",\"timer_overruns\":"<<state.overruns.load()<<",\"threads\":[";
  const auto threads=std::min<std::size_t>(state.threads.load(),MaxThreads);
  for(std::size_t i=0;i<threads;++i) {
    const auto& timer=state.timers[i];
    const auto cpu_end=timer.cpu_end;
    if(i)json<<',';
    json<<"{\"tid\":"<<timer.tid<<",\"timer_error\":"<<timer.error
        <<",\"cpu_clock_available\":"<<(cpu_end?"true":"false")
        <<",\"observed_cpu_ns\":"<<(cpu_end>=timer.cpu_start?cpu_end-timer.cpu_start:0)<<'}';
  }
  json<<"],\"thread_limit_exceeded\":"<<(state.threads.load()>MaxThreads?"true":"false")<<"}\n";
  csv.flush();json.flush();
  if(!csv || !json)throw std::runtime_error("cannot write native sample output");
  csv.close();json.close();
  std::filesystem::rename(prefix.string()+".samples.json.tmp",prefix.string()+".samples.json");
  const auto map=std::filesystem::path("/tmp")/("perf-"+std::to_string(getpid())+".map");
  std::error_code ec;
  if(std::filesystem::exists(map,ec))std::filesystem::copy_file(map,prefix.string()+".perf-map",ec);
}
}
struct NativeSamples::Impl {
  State* state=new State; // See process-lifetime signal storage contract above.
  std::filesystem::path prefix;
  std::jthread controller;
};
NativeSamples::NativeSamples() {
  const auto* prefix=std::getenv("GEKKOAOT_NATIVE_SAMPLE");if(!prefix || !*prefix)return;
  if(const auto* capture=std::getenv("GEKKOAOT_PERF_CAPTURE");capture && *capture)
    throw std::runtime_error("native CPU sampling and frame tracing require separate runs");
  impl_=std::make_unique<Impl>();impl_->prefix=prefix;
  if(std::filesystem::exists(impl_->prefix.string()+".start"))
    throw std::runtime_error("native sampling prefix already used");
  (void)setenv("GEKKOAOT_HUGE_TEXT_PERF_MAP","1",0);
  Capture::SetSink({impl_->state,&SampleBoundary,nullptr});
  auto* impl=impl_.get();
  impl_->controller=std::jthread([impl](std::stop_token stop) {
    std::error_code ec;
    while(!stop.stop_requested() && !std::filesystem::exists(impl->prefix.string()+".start",ec))
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    if(stop.stop_requested())return;
    try {
      Maps(impl->prefix.string()+".maps.start");
      Start(*impl->state);
      {std::ofstream ready(impl->prefix.string()+".running");ready<<impl->state->start;}
      unsigned seconds=15;
      if(const auto* value=std::getenv("GEKKOAOT_NATIVE_SAMPLE_SECONDS"))
        seconds=std::clamp(unsigned(std::strtoul(value,nullptr,10)),1u,120u);
      const auto until=std::chrono::steady_clock::now()+std::chrono::seconds(seconds);
      while(!stop.stop_requested() && std::chrono::steady_clock::now()<until &&
            !std::filesystem::exists(impl->prefix.string()+".stop",ec))
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
      Stop(*impl->state);
      Maps(impl->prefix.string()+".maps.end");
      Write(*impl->state,impl->prefix);
      std::fprintf(stderr,"GEKKOAOT_NATIVE_SAMPLES_V1 samples=%u overruns=%llu\n",impl->state->next.load(),
                   static_cast<unsigned long long>(impl->state->overruns.load()));
    } catch(const std::exception& error) {
      Stop(*impl->state);
      std::fprintf(stderr,"GEKKOAOT_NATIVE_SAMPLES_V1 error=\"%s\"\n",error.what());
    }
  });
}
NativeSamples::~NativeSamples() {Finish();}
void NativeSamples::Finish() {
  if(impl_ && impl_->controller.joinable()) {impl_->controller.request_stop();impl_->controller.join();}
}
bool NativeSamples::Running() const {
  return impl_ && (impl_->state->admission.load(std::memory_order_relaxed)&Open);
}
}
#else
namespace GekkoAOT::Perf {
struct NativeSamples::Impl {};
NativeSamples::NativeSamples() {
  if(const auto* p=std::getenv("GEKKOAOT_NATIVE_SAMPLE");p && *p)
    std::fprintf(stderr,"GEKKOAOT_NATIVE_SAMPLES_V1 unsupported=platform-or-architecture\n");
}
NativeSamples::~NativeSamples()=default;
void NativeSamples::Finish() {}
bool NativeSamples::Running() const {return false;}
}
#endif
