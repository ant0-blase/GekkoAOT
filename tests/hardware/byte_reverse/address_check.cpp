// SPDX-License-Identifier: GPL-3.0-or-later
#include "cpu/cpu.h"
#include "native/guest_address_window.h"
#include "native/guest_ram.h"
#include "core/native_hardware.h"
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
extern "C" {
void func_80012000(CPUState*);
void func_80012020(CPUState*);
void func_80012060(CPUState*);
void func_80012080(CPUState*);
}
#define CHECK(x) do { if (!(x)) { std::cerr << __LINE__ << ": " #x "\n"; std::exit(1); } } while(false)
struct Owner { unsigned reads=0,writes=0; uint32_t address=0;uint64_t value=0;uint8_t size=0;uint8_t* base=nullptr; };
extern "C" uint8_t* gekkoaot_native_address_base(CPUState* cpu) {
  return static_cast<Owner*>(cpu->external_user_data)->base;
}
extern "C" const GekkoAOTFlatVmemView* gekkoaot_native_flat_vmem_view(CPUState*) {
  static const GekkoAOTFlatVmemView disabled{};
  return &disabled;
}
static u64 Read(CPUState* cpu, u32 address, u8 size) {
  auto& owner=*static_cast<Owner*>(cpu->external_user_data);
  ++owner.reads; owner.address=address;owner.size=size;return 0x12345678;
}
static void Write(CPUState* cpu, u32 address, u64 value,u8 size) {
  auto& owner=*static_cast<Owner*>(cpu->external_user_data);
  ++owner.writes;owner.address=address;owner.size=size;owner.value=value;
}
int main(int argc,char** argv) {
  using namespace GekkoAOT::Native;
  const bool mapped=argc>1 && std::atoi(argv[1]);
  auto window=std::make_unique<GuestAddressWindow>(GC_MAIN_RAM_SIZE,WII_MEM2_SIZE,0);
  if(mapped) CHECK(window->Ready());
  GuestRam ram(GC_MAIN_RAM_SIZE,mapped ? window->Base() : nullptr);
  GuestRam mem2(WII_MEM2_SIZE,mapped ? window->Base()+0x10000000u : nullptr);
  auto cpu=std::make_unique<CPUState>();Owner owner{};
  cpu->ram=ram.data();cpu->ram_size=ram.size();cpu->mem2=mem2.data();cpu->mem2_size=mem2.size();
  cpu->external_user_data=&owner;cpu->external_read=Read;cpu->external_write=Write;
  owner.base=mapped ? window->Base() : nullptr;
  auto prepare=[&](u32 pc,u32 address) {
    cpu->pc=pc;cpu->lr=0x81234560;cpu->gpr[4]=address;cpu->exception=0;
    cpu->downcount=0;cpu->cycle_budget=0;owner.reads=owner.writes=0;
  };
  for(u32 base : {0x80000000u,0xc0000000u,0x90000000u,0xd0000000u}) {
    auto* bytes=(base&0x10000000u) ? mem2.data() : ram.data();
    const auto length=(base&0x10000000u) ? mem2.size() : ram.size();
    for(u32 offset : {u32(0),u32(0x10002),u32(length-4)}) {
      for(unsigned n=0;n<65536;++n) {
        bytes[offset]=n>>8;bytes[offset+1]=n;bytes[offset+2]=0x12;bytes[offset+3]=0x34;
        prepare(0x80012000,base+offset);func_80012000(cpu.get());
        CHECK(cpu->gpr[3]==((n>>8)|((n&255)<<8)));CHECK(!owner.reads&&!owner.writes);
        CHECK(cpu->pc==cpu->lr&&!cpu->exception);
      }
      prepare(0x80012060,base+offset);func_80012060(cpu.get());
      CHECK(cpu->gpr[3]==0x3412ffffu);CHECK(!owner.reads&&!owner.writes);
      prepare(0x80012080,base+offset);cpu->gpr[3]=0x12345678;func_80012080(cpu.get());
      CHECK(std::memcmp(bytes+offset,"\x78\x56\x34\x12",4)==0);CHECK(!owner.reads&&!owner.writes);
    }
    // Last halfword is valid; a word crossing the end must use compatibility.
    bytes[length-2]=0xab;bytes[length-1]=0xcd;
    prepare(0x80012000,base+length-2);func_80012000(cpu.get());CHECK(cpu->gpr[3]==0xcdab);
    prepare(0x80012060,base+length-2);func_80012060(cpu.get());
    CHECK(owner.reads==1&&owner.address==base+length-2&&owner.size==4);
  }
  for(u32 address : {0u,0x01800000u,0xcc000010u,0xcc008000u,0x7e000000u,0xffffffffu}) {
    prepare(0x80012000,address);func_80012000(cpu.get());CHECK(owner.reads==1);
    CHECK(owner.address==address&&owner.size==2);
  }
  std::cout << "Native address lowering: 786432 alias loads, endian stores, null binding, MEM2, full-width guards and hardware fallback passed\n";
}
