// SPDX-License-Identifier: GPL-3.0-or-later
#include "native/runtime.h"
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#define CHECK(x) do { if (!(x)) { std::cerr << __LINE__ << ": " #x "\n"; std::exit(1); } } while(false)
namespace GekkoAOT::Native {
struct HardwareTestAccess {
  static void Enable(HostRuntime& r, bool enabled) { r.native_address_base_enabled_ = enabled; }
  static unsigned char* Base(HostRuntime& r) { return r.memory_.NativeAddressBase(); }
  static void FlatEnabled(HostRuntime& r, bool enabled) { r.native_flat_vmem_enabled_ = enabled; }
  static void Paging(HostRuntime& r, bool enabled) { r.memory_.SetPagedVmem(enabled); }
};
}
using namespace GekkoAOT::Native;
int main() {
  auto first=std::make_unique<HostRuntime>(), second=std::make_unique<HostRuntime>();
  const auto& service=HostRuntime::NativeAddressWindowServices();
  CHECK(service.version==1 && service.size==sizeof(service) && service.get_base);
  CHECK(!service.get_base(nullptr));
  CPUState unowned{}; CHECK(!service.get_base(&unowned));
  for(bool enabled : {true,false,true}) {
    HardwareTestAccess::Enable(*first,enabled);
    for(auto* runtime : {first.get(),second.get()}) {
      auto& cpu=runtime->Cpu(); CPUState before;
      cpu.pc=0x81234568; cpu.external_read_count=254; cpu.external_write_count=255;
      std::memcpy(&before,&cpu,sizeof(cpu));
      auto* expected=runtime==first.get()&&!enabled ? nullptr : HardwareTestAccess::Base(*runtime);
      CHECK(service.get_base(&cpu)==expected);
      CHECK(std::memcmp(&before,&cpu,sizeof(cpu))==0);
    }
  }
  auto* a=HardwareTestAccess::Base(*first), *b=HardwareTestAccess::Base(*second);
  CHECK(!a || !b || a!=b);
  const auto& flat=HostRuntime::NativeFlatVmemServices();
  CHECK(flat.version==2 && flat.size==sizeof(flat) && flat.get_view);
  CHECK(!flat.get_view(nullptr) && !flat.get_view(&unowned));
  const auto* view=flat.get_view(&first->Cpu());
  // v322: generated code reads the paged DTLB mirror without NULL checks.
  CHECK(view && view->paged_pages && view->paged_mru && view->paged_pages[0]==0u);
  CHECK(view && view->data && view->size==AddressSpace::RetailFakeVmemSize);
  CHECK(view != flat.get_view(&second->Cpu()));
  CPUState flat_before; std::memcpy(&flat_before,&first->Cpu(),sizeof(flat_before));
  for(unsigned i=0;i<65536;++i) {
    HardwareTestAccess::Paging(*first,true);
    CHECK(view==flat.get_view(&first->Cpu()) && view->size==0u);
    HardwareTestAccess::Paging(*first,false);
    CHECK(view->size==AddressSpace::RetailFakeVmemSize);
  }
  CHECK(std::memcmp(&flat_before,&first->Cpu(),sizeof(flat_before))==0);
  HardwareTestAccess::FlatEnabled(*first,false); CHECK(!flat.get_view(&first->Cpu()));
  HardwareTestAccess::FlatEnabled(*first,true); CHECK(flat.get_view(&first->Cpu())==view);
  const auto& published=HostRuntime::NativeAddressWindowServices(true);
  CHECK(published.version==service.version && published.size==service.size);
  CHECK(!published.get_base(nullptr)); CHECK(!published.get_base(&unowned));
  const char* original=std::getenv("GEKKOAOT_NATIVE_ADDRESS_BASE");
  const bool had_original=original!=nullptr;
  const std::string saved=original ? original : "";
  for (const char* enabled : {"1","0","1"}) {
#ifdef _WIN32
    _putenv_s("GEKKOAOT_NATIVE_ADDRESS_BASE",enabled);
#else
    setenv("GEKKOAOT_NATIVE_ADDRESS_BASE",enabled,1);
#endif
    first->Reset(); second->Reset();
    for (unsigned i=0; i<16384; ++i) {
      auto& cpu=(i&1 ? first : second)->Cpu();
      cpu.pc=0x80000000u+i*4u; cpu.gpr[i%32]=i*0x1234567u;
      CPUState before; std::memcpy(&before,&cpu,sizeof(cpu));
      CHECK(published.get_base(&cpu)==service.get_base(&cpu));
      CHECK(std::memcmp(&before,&cpu,sizeof(cpu))==0);
    }
  }
#ifdef _WIN32
  _putenv_s("GEKKOAOT_NATIVE_ADDRESS_BASE",had_original?saved.c_str():"");
#else
  if(had_original) setenv("GEKKOAOT_NATIVE_ADDRESS_BASE",saved.c_str(),1);
  else unsetenv("GEKKOAOT_NATIVE_ADDRESS_BASE");
#endif
  std::cout << "Native address service: ownership, disable, reset publication and 49152 full CPU states passed\n";
}
