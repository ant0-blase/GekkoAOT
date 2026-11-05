// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/cpu_state.h"
#include "module/secondary_dispatch_cache.h"
#include <array>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <thread>
#include <vector>
#define CHECK(x) do { if (!(x)) { std::cerr << __LINE__ << ": " #x "\n"; std::exit(1); } } while(false)
static void A(CPUState* c) { c->gpr[3] = 0x12345678; c->pc = c->lr; }
static void B(CPUState* c) { c->gpr[4] = 0x87654321; c->pc = c->lr; }
int main(int argc, char**) {
  std::array<GekkoAOTChunkRoute, 512> first{}, second{};
  for (unsigned i = 0; i < first.size(); ++i) {
    first[i] = {0x80010000u + i * 32, 0x80010000u + i * 32 + 20, i & 1 ? A : B};
    second[i] = {first[i].start, first[i].end, i & 1 ? B : A};
  }
  std::array<uint16_t,512*8> instruction_indices{};
  for(unsigned route=0;route<first.size();++route)
    for(unsigned word=0;word<5;++word)instruction_indices[route*8+word]=route+1;
  const GekkoAOTInstructionRoutes image_first{first.data(),instruction_indices.data(),0x80010000,512*8,512};
  const GekkoAOTInstructionRoutes image_second{second.data(),instruction_indices.data(),0x80010000,512*8,512};
  GekkoAOTChunkCache cache{};
  std::array<GekkoAOTChunkPage,4> pages{};
  for(unsigned page=0;page<pages.size();++page)pages[page]={page*128u,(page+1u)*128u};
  GekkoAOTSharedChunkCache shared_first{first.data(), {}, pages.data(), 0x80010u,4};
  GekkoAOTSharedChunkCache shared_second{second.data(), {}, pages.data(), 0x80010u,4};
  uint64_t random = 0x3456789abcdef123ull;
  for (unsigned n = 0; n != 200000; ++n) {
    random = random * 6364136223846793005ull + 1;
    const auto& table = n & 1 ? first : second;
    const uint32_t pc = n < 32 ? 0x80010000 + n :
        uint32_t(0x8000f000 + (random >> 32) % 0x6000);
    GekkoAOTChunkFn expected = nullptr;
    for (const auto& r : table)
      if (pc >= r.start && pc < r.end && !((pc - r.start) & 3)) expected = r.fn;
    const auto fn = gekkoaot_find_chunk(table.data(), table.size(), pc, &cache);
    CHECK(fn == expected);
    CHECK(gekkoaot_find_instruction_chunk(n & 1 ? &image_first : &image_second,pc)==expected);
    CHECK(gekkoaot_find_shared_chunk(n & 1 ? &shared_first : &shared_second, table.size(), pc) == expected);
    CHECK(gekkoaot_find_chunk(table.data(), table.size(), pc, &cache) == fn);
    CHECK(gekkoaot_find_chunk(table.data(), table.size(), pc, nullptr) == fn);
    // Cache/table hits cannot alter guest state; invoking the chosen wrapper
    // must have exactly the same effects as ordinary generated dispatch.
    CPUState cpu{}, reference{};
    cpu.pc = pc; cpu.lr = 0x81234560; cpu.gpr[3] = 17; cpu.gpr[4] = 18;
    reference = cpu;
    if (fn) fn(&cpu);
    if (expected) expected(&reference);
    CHECK(std::memcmp(&cpu, &reference, sizeof(cpu)) == 0);
  }
  CHECK(!gekkoaot_find_chunk(first.data(), first.size(), 0xffffffff, &cache));
  CHECK(!gekkoaot_find_chunk(first.data(), first.size(), 0, &cache));
  std::vector<std::thread> workers;
  for (unsigned thread=0; thread<8; ++thread) workers.emplace_back([&,thread] {
    uint64_t random=thread+1;
    for(unsigned n=0;n<200000;++n) {
      random=random*6364136223846793005ull+1;
      const auto pc=uint32_t(0x8000f000+(random>>32)%0x6000);
      auto* shared=n&1 ? &shared_first : &shared_second;
      const auto& routes=n&1 ? first : second;
      CHECK(gekkoaot_find_instruction_chunk(n&1 ? &image_first : &image_second,pc)==gekkoaot_find_chunk(routes.data(),routes.size(),pc,nullptr));
      CHECK(gekkoaot_find_shared_chunk(shared,routes.size(),pc)==
            gekkoaot_find_chunk(routes.data(),routes.size(),pc,nullptr));
    }
  });
  for(auto& worker:workers) worker.join();
  CHECK(!gekkoaot_find_shared_chunk(&shared_first, first.size(), 0));
  CHECK(!gekkoaot_find_shared_chunk(&shared_first, first.size(), 0xffffffff));
  // A chunk crosses a page boundary; the next two pages are a text hole.
  const GekkoAOTChunkRoute crossing[]={{0x1000,0x2400,A},{0x5000,0x5020,B}};
  const GekkoAOTChunkPage crossing_pages[]={{0,1},{0,1},{1,1},{1,1},{1,2}};
  GekkoAOTSharedChunkCache cross{crossing,{},crossing_pages,1,5};
  for(uint32_t pc=0;pc<0x6000;++pc)
    CHECK(gekkoaot_find_shared_chunk(&cross,2,pc)==gekkoaot_find_chunk(crossing,2,pc,nullptr));
  // Highest 16-bit encoding (route 65535), zero/gap and corrupt indices.
  std::vector<GekkoAOTChunkRoute> large(65535,{0,4,A});large.back().fn=B;
  const uint16_t edge_indices[]={65535,0,65534};
  GekkoAOTInstructionRoutes edge_image{large.data(),edge_indices,0xfffffff0,3,65535};
  CHECK(gekkoaot_find_instruction_chunk(&edge_image,0xfffffff0)==B);
  CHECK(!gekkoaot_find_instruction_chunk(&edge_image,0xfffffff4));
  CHECK(gekkoaot_find_instruction_chunk(&edge_image,0xfffffff8)==A);
  CHECK(!gekkoaot_find_instruction_chunk(&edge_image,0));
  CHECK(!gekkoaot_find_instruction_chunk(&edge_image,0xfffffffc));
  edge_image.route_count=3;CHECK(!gekkoaot_find_instruction_chunk(&edge_image,0xfffffff0));
  if (argc > 1) {
    CPUState cpu{}; volatile uint32_t result = 0;
    for (unsigned cached : {0u, 1u, 2u, 3u}) {
      const auto begin = std::chrono::steady_clock::now();
      for (unsigned i = 0; i != 10000000; ++i) {
        const auto pc = first[(i*103u)&511u].start + ((i & 3) * 4);
        auto fn = cached == 3 ? gekkoaot_find_instruction_chunk(&image_first,pc) : cached == 2 ? gekkoaot_find_shared_chunk(&shared_first, first.size(), pc) :
                  gekkoaot_find_chunk(first.data(), first.size(), pc, cached ? &cache : nullptr);
        fn(&cpu); result = cpu.gpr[3] + cpu.gpr[4];
      }
      std::cout << "secondary route " << (cached == 3 ? "instruction-image" : cached == 2 ? "shared" : cached ? "thread-local" : "binary") << " ns/call="
          << std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - begin).count() / 10000000 << '\n';
    }
    (void)result;
  }
  std::cout << "secondary direct routes: 200K cases, collisions, module identity, interior PC, gaps, alignment and full state passed\n";
}
