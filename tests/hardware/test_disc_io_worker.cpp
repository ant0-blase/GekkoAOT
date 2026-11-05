// SPDX-License-Identifier: GPL-3.0-or-later
#include "native/disc_io_worker.h"
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <stdexcept>

#define CHECK(x) do { if (!(x)) { std::cerr << __LINE__ << ": " #x "\n"; std::exit(1); } } while(false)
using GekkoAOT::Native::DiscIoWorker;
struct Media {
  std::atomic<bool> hold{false}, entered{false};
  std::thread::id owner=std::this_thread::get_id(), io;
  unsigned calls=0;
  static bool Read(void* context,std::uint64_t offset,void* out,std::uint32_t size) {
    auto& self=*static_cast<Media*>(context);
    CHECK(self.owner!=std::this_thread::get_id());
    if(self.calls++)CHECK(self.io==std::this_thread::get_id());
    self.io=std::this_thread::get_id();
    self.entered.store(true,std::memory_order_release);
    while(self.hold.load(std::memory_order_acquire))self.hold.wait(true);
    if(offset==UINT64_MAX)return false;
    if(offset==UINT64_MAX-1)throw std::runtime_error("I/O error");
    auto* bytes=static_cast<std::uint8_t*>(out);
    for(unsigned i=0;i<size;++i)bytes[i]=static_cast<std::uint8_t>(offset+i);
    return true;
  }
};
DiscIoWorker::Result Collect(DiscIoWorker& worker,std::vector<std::uint8_t>& bytes) {
  DiscIoWorker::Result result;
  const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
  while(!worker.TryCollect(result,bytes)) {
    CHECK(std::chrono::steady_clock::now()<deadline);
    std::this_thread::yield();
  }
  return result;
}
int main() {
  Media media;DiscIoWorker worker;worker.Start(&media,&Media::Read);
  std::vector<std::uint8_t> bytes;
  media.hold.store(true);
  CHECK(worker.Submit(1,17,4096));
  CHECK(!worker.Submit(2,0,32));
  DiscIoWorker::Result result;
  CHECK(!worker.TryCollect(result,bytes)&&bytes.empty());
  media.hold.store(false,std::memory_order_release);media.hold.notify_all();
  result=Collect(worker,bytes);CHECK(result.ok&&result.sequence==1&&bytes.size()==4096);
  CHECK(bytes.front()==17&&bytes.back()==16);
  // A caller can discard a cancelled generation without publishing any data.
  // The same persistent thread and reusable result storage handle its successor.
  bytes.clear();CHECK(worker.Submit(3,41,32));result=Collect(worker,bytes);
  CHECK(result.sequence==3&&result.ok&&bytes.front()==41);
  CHECK(worker.Submit(4,UINT64_MAX,32));result=Collect(worker,bytes);
  CHECK(!result.ok&&bytes.empty());
  CHECK(worker.Submit(5,UINT64_MAX-1,32));result=Collect(worker,bytes);
  CHECK(!result.ok&&bytes.empty());
  const auto start=std::chrono::steady_clock::now();
  for(unsigned i=0;i<10000;++i) {
    CHECK(worker.Submit(6+i,i,32));result=Collect(worker,bytes);
    CHECK(result.ok&&result.sequence==6+i&&bytes.front()==std::uint8_t(i));
  }
  const auto micros=std::chrono::duration<double,std::micro>(std::chrono::steady_clock::now()-start).count();
  CHECK(worker.WorkerStarts()==1);
  worker.Stop(); // includes the idle/complete waiting states
  std::cout<<"Persistent I/O worker: ordering, private results, errors, reuse and stop passed; "
           <<micros/10000<<" us per synthetic 32-byte round trip\n";
}
