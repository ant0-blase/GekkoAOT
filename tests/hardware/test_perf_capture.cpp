// SPDX-License-Identifier: GPL-3.0-or-later
#include "perf/capture_session.h"
#include <cstdlib>
#include <iostream>
#include <thread>
#include <vector>

#define CHECK(x) do { if(!(x)) { std::cerr<<__LINE__<<": " #x "\n"; std::exit(1); } } while(false)
int main() {
  using namespace GekkoAOT::Capture;
  CHECK(!Enabled());
  Sink abi;
  CHECK(Compatible(&abi,sizeof(abi)));
  CHECK(!Compatible(nullptr,sizeof(abi)));
  CHECK(!Compatible(&abi,sizeof(abi)-1));
  ++abi.abi; CHECK(!Compatible(&abi,sizeof(abi))); --abi.abi;
  ++abi.kind_count; CHECK(!Compatible(&abi,sizeof(abi)));
  auto session=std::make_unique<Session>(128);
  SetSink(session->GetSink());
  Mark(Kind::WsiSuccess,7); CHECK(session->Size()==0);
  const auto stale=Now();
  session->Start();
  Record(Kind::Count,Now(),Now()); // reject incompatible telemetry, never index beyond the histogram
  Record(Kind::XfbPublish,stale,stale,99); // not startup data
  { FrameContext context(42); Scope work(Kind::RenderWork); }
  CHECK(frame_id==0);
  Mark(Kind::XfbPublish,42,0x1000);
  Mark(Kind::XfbSelected,42,3);
  Mark(Kind::WsiSuccess,42);
  session->Stop();
  CHECK(session->Size()==4);
  CHECK(session->At(0).id==42);
  CHECK(session->Summary(Kind::WsiSuccess).count==1);
  Mark(Kind::WsiSuccess,43); CHECK(session->Size()==4);
  SetSink({});
  const auto directory=std::filesystem::temp_directory_path()/
      ("gekkoaot-capture-test-"+std::to_string(ThreadId())+"-"+std::to_string(Now()));
  std::filesystem::create_directory(directory);
  session->Write(directory/"capture");
  CHECK(std::filesystem::file_size(directory/"capture.json")>0);
  CHECK(std::filesystem::file_size(directory/"capture.events.csv")>0);
  CHECK(std::filesystem::file_size(directory/"capture.trace.json")>0);
  CHECK(!std::filesystem::exists(directory/"capture.json.tmp"));
  bool rejected=false;
  try { session->Write(directory/"missing"/"capture"); }
  catch(const std::runtime_error&) { rejected=true; }
  CHECK(rejected);
  std::filesystem::remove_all(directory);

  auto concurrent=std::make_unique<Session>(128);
  SetSink(concurrent->GetSink());concurrent->Start();
  std::vector<std::thread> writers;
  for(unsigned i=0;i<4;++i)writers.emplace_back([i] {
    for(unsigned n=0;n<10000;++n)Mark(Kind::GpuSubmit,n,i);
  });
  for(auto& thread:writers)thread.join();concurrent->Stop();
  CHECK(concurrent->Size()==128);
  CHECK(concurrent->Dropped()==40000-128);
  CHECK(concurrent->Summary(Kind::GpuSubmit).count==40000);
  for(unsigned i=0;i<128;++i)CHECK(concurrent->At(i).tid!=0);
  SetSink({});

  auto stopping=std::make_unique<Session>(8192);
  SetSink(stopping->GetSink());stopping->Start();
  std::atomic<bool> run{true};
  std::thread writer([&] {while(run.load()) Mark(Kind::ViFieldObserved);});
  std::this_thread::sleep_for(std::chrono::milliseconds(5));
  stopping->Stop();const auto count=stopping->Summary(Kind::ViFieldObserved).count;
  std::this_thread::sleep_for(std::chrono::milliseconds(2));
  CHECK(stopping->Summary(Kind::ViFieldObserved).count==count);
  run=false;writer.join();CHECK(stopping->Summary(Kind::ViFieldObserved).count==count);
  SetSink({});
  std::cout<<"Capture: opt-in gate, frame identity, closed-window publication, overflow and concurrent stop PASS\n";
}
