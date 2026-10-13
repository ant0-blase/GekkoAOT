#include "cpu/cpu.h"
#include "core/native_hardware.h"
#include <array>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
extern "C" {
void func_80016000(CPUState*);void func_80016100(CPUState*);void func_80016200(CPUState*);
void func_80016300(CPUState*);void func_80016400(CPUState*);void func_80016500(CPUState*);
void func_80016600(CPUState*);void func_80016700(CPUState*);void func_80016800(CPUState*);
void func_80016900(CPUState*);void func_80016A00(CPUState*);void func_80016B00(CPUState*);
void func_80016C00(CPUState*);void func_80016D00(CPUState*);
}
#define CHECK(x) do{if(!(x)){std::fprintf(stderr,"%d: %s\n",__LINE__,#x);std::exit(1);}}while(false)
struct Owner {
  std::array<uint8_t,262144> bytes{};
  bool admitted=true;
  unsigned reads=0,writes=0;bool toggle=false;
};
extern "C" uint8_t* gekkoaot_native_address_base(CPUState*){return nullptr;}
extern "C" uint8_t* gekkoaot_native_locked_cache_base(CPUState* c) {
  auto& o=*static_cast<Owner*>(c->external_user_data); return o.admitted?o.bytes.data():nullptr;
}
static uint64_t Read(CPUState* c,uint32_t address,uint8_t size) {
  auto& o=*static_cast<Owner*>(c->external_user_data);++o.reads;
  // A host effect at a safe boundary must not redirect stable LC storage.
  if(address==0xcc006000u&&o.toggle) o.bytes[0]^=0x5a;
  const auto offset=address-0xe0000000u;
  if(size>o.bytes.size()||offset>o.bytes.size()-size) return 0x76543210u;
  uint64_t value=0;for(unsigned j=0;j<size;++j)value=value<<8|o.bytes[offset+j];return value;
}
static void Write(CPUState* c,uint32_t address,uint64_t value,uint8_t size) {
  auto& o=*static_cast<Owner*>(c->external_user_data);++o.writes;
  const auto offset=address-0xe0000000u;
  if(size>o.bytes.size()||offset>o.bytes.size()-size)return;
  for(unsigned j=size;j--;) {o.bytes[offset+j]=value;value>>=8;}
}
int main() {
  auto cpu=std::make_unique<CPUState>();auto owner=std::make_unique<Owner>();
  auto ram=std::make_unique<uint8_t[]>(GC_MAIN_RAM_SIZE);
  uint64_t hash=1469598103934665603ull;unsigned cases=0;
  const auto mix=[&](uint64_t v){hash=(hash^v)*1099511628211ull;};
  const auto run=[&](unsigned op,uint32_t address,uint32_t gqr,uint64_t fp,bool revoked,bool toggle=false) {
    *cpu={};cpu->ram=ram.get();cpu->ram_size=GC_MAIN_RAM_SIZE;
    cpu->external_user_data=owner.get();cpu->external_read=Read;cpu->external_write=Write;
    cpu->pc=0x80016000u+op*0x100u;cpu->lr=0x81234560u;cpu->msr=0x2000u;cpu->hid2=0xe0000000u;
    cpu->gpr[3]=0x1234abcd;cpu->gpr[4]=address;cpu->gpr[7]=0xcc006000u;cpu->gqr[1]=gqr;
    cpu->reserve_valid=true;cpu->reserve_addr=address^((cases&1u)?32u:0u);
    cpu->fpr[1]=std::bit_cast<double>(fp);cpu->ps1[1]=std::bit_cast<double>(fp^0x8000000000000000ull);
    owner->reads=owner->writes=0;owner->toggle=toggle;owner->admitted=!revoked;
    // These bytes model DMA writes to the exact same owner storage.
    for(unsigned j=0;j<16;++j){owner->bytes[j]=uint8_t(j*17+cases);owner->bytes[262128+j]=uint8_t(j*7+cases);}
    const std::array<void(*)(CPUState*),14> f={func_80016000,func_80016100,func_80016200,func_80016300,
      func_80016400,func_80016500,func_80016600,func_80016700,func_80016800,func_80016900,
      func_80016A00,func_80016B00,func_80016C00,func_80016D00};
    f[op](cpu.get());CHECK(cpu->pc==cpu->lr&&!cpu->exception);
    if(LC_LOWERED&&!revoked&&address==0xe0000000u&&!toggle)CHECK(!owner->reads&&!owner->writes);
    if(LC_LOWERED&&toggle&&!revoked)CHECK(owner->reads==1u);
    if(LC_LOWERED&&toggle&&revoked)CHECK(owner->reads==3u);
    mix(cpu->gpr[3]);mix(std::bit_cast<uint64_t>(cpu->fpr[1]));mix(std::bit_cast<uint64_t>(cpu->ps1[1]));
    mix(cpu->fpscr);mix(cpu->pc);mix(cpu->downcount);
    CPUState snapshot; std::memcpy(&snapshot,cpu.get(),sizeof(snapshot));
    snapshot.ram=nullptr; snapshot.external_user_data=nullptr;
    snapshot.external_read=nullptr; snapshot.external_write=nullptr;
    snapshot.external_read_count=0; snapshot.external_write_count=0;
    for(unsigned j=0;j<sizeof(snapshot);++j)mix(reinterpret_cast<const uint8_t*>(&snapshot)[j]);
    for(unsigned j=0;j<16;++j){mix(owner->bytes[j]);mix(owner->bytes[262128+j]);}++cases;
  };
  for(unsigned op=0;op<9;++op)for(bool revoked:{false,true})
    for(uint32_t address:{0xe0000000u,0xe0000002u,0xe003fff8u,0xe003fffeu,0xdfffffffu,0x60000000u,0xcc006000u})
      run(op,address,0,0x3ff23456789abcdeull,revoked);
  for(unsigned type:{0u,4u,5u,6u,7u})for(unsigned scale=0;scale<64;++scale)
    for(unsigned op=9;op<13;++op)for(bool revoked:{false,true})
      for(uint64_t bits:{0x0000000000000000ull,0x8000000000000000ull,0x3ff23456789abcdeull,
          0x7ff0000000000000ull,0xfff0000000000000ull,0x7ff8000000000042ull,0x0000000000000001ull})
        run(op,0xe0000000u,(type|(scale<<8))|(type<<16)|(scale<<24),bits,revoked);
  run(13,0xe0000000u,0,0,false,true);run(13,0xe0000000u,0,0,true,true);
  std::printf("cases=%u digest=%016llx\n",cases,(unsigned long long)hash);
}
