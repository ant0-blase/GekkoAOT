// SPDX-License-Identifier: GPL-3.0-or-later
#include "perf/native_samples.h"
#include "perf/capture.h"
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>
#include <string>
#if defined(__linux__) && !defined(__ANDROID__) && (defined(__x86_64__) || defined(__aarch64__))
#include <time.h>
#include <sys/syscall.h>
#include <unistd.h>
#define CHECK(x) do {if(!(x)) {std::cerr<<__LINE__<<": " #x "\n";std::exit(1);}}while(false)
static std::uint64_t CpuTime() {timespec t{};clock_gettime(CLOCK_THREAD_CPUTIME_ID,&t);return std::uint64_t(t.tv_sec)*1000000000+t.tv_nsec;}
static void Busy() {
  const auto end=CpuTime()+150000000;
  std::uint64_t value=1;
  while(CpuTime()<end) {for(unsigned i=0;i<256;++i)value=value*1664525+1013904223;std::atomic_signal_fence(std::memory_order_seq_cst);}
  if(value==0)std::cout<<value;
}
int main() {
  using namespace GekkoAOT;
  unsetenv("GEKKOAOT_PERF_CAPTURE");unsetenv("GEKKOAOT_NATIVE_SAMPLE");
  {Perf::NativeSamples off;CHECK(!off.Running());CHECK(!Capture::Enabled());}
  const auto path=std::filesystem::temp_directory_path()/
      ("gekkoaot-native-samples-"+std::to_string(getpid())+"-"+std::to_string(Capture::Now()));
  std::filesystem::create_directory(path);
  const auto prefix=(path/"test").string();
  setenv("GEKKOAOT_NATIVE_SAMPLE",prefix.c_str(),1);
  setenv("GEKKOAOT_NATIVE_SAMPLE_SECONDS","2",1);
  Perf::NativeSamples profiler;
  CHECK(!profiler.Running());
  {std::ofstream start(prefix+".start");start<<"reviewed unit workload";}
  const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
  while(!profiler.Running()) {CHECK(std::chrono::steady_clock::now()<deadline);std::this_thread::sleep_for(std::chrono::milliseconds(5));}
  const auto main_tid=syscall(SYS_gettid);
  std::atomic<long> idle_tid{0},worker_tid{0};
  std::thread idle([&] {
    idle_tid=syscall(SYS_gettid);CHECK(!Capture::Enabled());
    std::this_thread::sleep_for(std::chrono::milliseconds(350));
  });
  std::thread worker([&] {worker_tid=syscall(SYS_gettid);CHECK(!Capture::Enabled());Busy();});
  CHECK(!Capture::Enabled());Busy();worker.join();idle.join();
  profiler.Finish();CHECK(!profiler.Running());
  CHECK(std::filesystem::is_regular_file(prefix+".samples.json"));
  std::ifstream csv(prefix+".samples.csv");std::string line;std::getline(csv,line);
  unsigned main_count=0,worker_count=0,idle_count=0;
  while(std::getline(csv,line)) {
    const auto first=line.find(','),second=line.find(',',first+1);
    const auto tid=std::stol(line.substr(first+1,second-first-1));
    main_count+=tid==main_tid;worker_count+=tid==worker_tid;idle_count+=tid==idle_tid;
  }
  CHECK(main_count>=3 && worker_count>=3 && idle_count==0);
  CHECK(!Capture::Enabled()); // Late boundaries must not re-arm timers.
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  Capture::SetSink({});unsetenv("GEKKOAOT_NATIVE_SAMPLE");
  std::filesystem::remove_all(path);
  std::cout<<"Native CPU samples: disabled path, gate, targeted busy threads, sleeping thread, stop and late signals PASS\n";
}
#else
int main() {std::cout<<"Native CPU sampler unsupported on this platform\n";}
#endif
