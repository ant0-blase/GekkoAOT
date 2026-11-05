#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Compile and exercise the actual patched Aurora queue, without a GPU.

Compiles the complete production worker; only Tracy/thread affinity are stubbed.
Run after configuring runtime/gx/aurora against the pinned Aurora checkout.
"""
from pathlib import Path
import argparse
import subprocess
import tempfile
import json
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--source',type=Path,required=True)
p.add_argument('--cxx',default='c++')
p.add_argument('--benchmark',type=Path)
a=p.parse_args()
s=(a.source/'lib/gfx/render_worker.cpp').read_text()
impl='' 
cpp='''
#include "gfx/render_worker.hpp"
#include "gekkoaot_capture.hpp"
#include "thread.hpp"
#include <cstdlib>
#define ZoneScoped (void)0
#include <ctime>
#include <atomic>
#include <cassert>
#include <future>
#include <iostream>
#define CHECK(x) do { if(!(x)) std::abort(); } while(false)
namespace aurora::thread { void set_current(const Options&) noexcept {} }
namespace aurora::gfx::render_worker {
'''+impl+'''
}
int main() {
  using namespace aurora::gfx::render_worker;
  using namespace std::chrono_literals;
  BoundedQueue queue(2);
  auto consumer=std::async(std::launch::async,[&] {
    bool closed=false;auto item=queue.pop(closed);
    CHECK(item && !closed && item->frameId==42);return true;
  });
  CHECK(consumer.wait_for(25ms)==std::future_status::timeout);
  CHECK(queue.push({.frameId=42}));
  CHECK(consumer.wait_for(1s)==std::future_status::ready);CHECK(consumer.get());
  auto closing=std::async(std::launch::async,[&] {
    bool closed=false;auto item=queue.pop(closed);CHECK(closed && !item);
  });
  CHECK(closing.wait_for(25ms)==std::future_status::timeout);queue.close();
  CHECK(closing.wait_for(1s)==std::future_status::ready);closing.get();
  CHECK(!queue.push({}));
  queue.reset();
  CHECK(queue.push({.frameId=1}));CHECK(queue.push({.frameId=2}));
  auto producer=std::async(std::launch::async,[&] {return queue.push({.frameId=3});});
  CHECK(producer.wait_for(25ms)==std::future_status::timeout);
  bool closed=false;CHECK(queue.pop(closed)->frameId==1);
  CHECK(producer.wait_for(1s)==std::future_status::ready);CHECK(producer.get());
  queue.close();CHECK(queue.pop(closed)->frameId==2);CHECK(queue.pop(closed)->frameId==3);
  CHECK(!queue.pop(closed) && closed);
  queue.reset();
  std::jthread stopping([&](std::stop_token stop) {
    std::stop_callback wake(stop,[&]{queue.close();});
    bool done=false;CHECK(!queue.pop(done) && done);
  });
  stopping.request_stop();stopping.join();
  std::vector<unsigned> order;
  initialize();
  for(unsigned i=0;i<1024;++i)enqueue_work([&,i] {CHECK(is_worker_thread());order.push_back(i);});
  synchronize();
  shutdown();
  CHECK(is_idle() && order.size()==1024);
  for(unsigned i=0;i<1024;++i)CHECK(order[i]==i);
  initialize();synchronize();shutdown(); // an idle exact waiter must stop too

  std::cout<<"Production Aurora render queue: work, capacity, ordered drain, close, reset, stop PASS\\n";
#ifdef __linux__
  if(std::getenv("GEKKOAOT_QUEUE_BENCHMARK"))
  for(unsigned repeat=0;repeat<5;++repeat)for(bool exact:{false,true}) {
    BoundedQueue idle(2);unsigned wakes=0;long long cpu=0;
    std::thread thread([&] {
      timespec before{},after{};clock_gettime(CLOCK_THREAD_CPUTIME_ID,&before);
      for(;;) {bool closed=false;
        auto item=exact?idle.pop(closed):idle.pop_for(1ms,closed);
        ++wakes;if(closed)break;
      }
      clock_gettime(CLOCK_THREAD_CPUTIME_ID,&after);
      cpu=(after.tv_sec-before.tv_sec)*1000000000LL+after.tv_nsec-before.tv_nsec;
    });
    std::this_thread::sleep_for(200ms);idle.close();thread.join();
    std::cout<<"{\\"exact\\":"<<exact<<",\\"repeat\\":"<<repeat<<",\\"wakes\\":"<<wakes<<",\\"cpu_ns\\":"<<cpu<<"}\\n";
  }
#endif

}
'''
with tempfile.TemporaryDirectory(prefix='gekkoaot-queue-test-') as tmp:
 root=Path(tmp);(root/'test.cpp').write_text(cpp)
 (root/'tracy').mkdir()
 (root/'tracy/Tracy.hpp').write_text('#pragma once\n#define ZoneScoped (void)0\n#define ZoneScopedN(x) (void)0\n')
 subprocess.run([a.cxx,'-std=c++20','-O2','-pthread','-I'+str(a.source/'lib'),
                 '-I'+str(root),str(root/'test.cpp'),str(a.source/'lib/gfx/render_worker.cpp'),'-o',str(root/'test')],check=True)
 import os
 env=dict(os.environ,GEKKOAOT_AURORA_RENDER_EVENT_WAIT='1')
 if a.benchmark:env['GEKKOAOT_QUEUE_BENCHMARK']='1'
 result=subprocess.run([str(root/'test')],check=True,timeout=10,capture_output=True,text=True,env=env)
 legacy=dict(os.environ,GEKKOAOT_AURORA_RENDER_EVENT_WAIT='0')
 subprocess.run([str(root/'test')],check=True,timeout=10,env=legacy)
 print(result.stdout,end='')
 if a.benchmark:
  a.benchmark.write_text(json.dumps([json.loads(line) for line in result.stdout.splitlines() if line.startswith('{')],indent=2)+'\n')
