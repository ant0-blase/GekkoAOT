// SPDX-License-Identifier: GPL-3.0-or-later
#include "native/runtime.h"
#include "gx/pe_event_queue.h"
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <thread>
#define CHECK(x) do {if(!(x)){std::fprintf(stderr,"%d: %s\n",__LINE__,#x);std::exit(1);}}while(false)
extern "C" void gekkoaot_test_gx_publish_pe(std::uint32_t,std::uint32_t);
namespace GekkoAOT::Native {
struct HardwareTestAccess {
  static void Verify(HostRuntime& runtime) {
    CHECK(runtime.pe_.Write(0xcc00100a,3,2));
    std::thread parser([] {
      for(unsigned token=1;token<=5;++token) {
        // Retail GXSetDrawSync emits the interrupting word first.
        gekkoaot_test_gx_publish_pe(0x48,token);
        gekkoaot_test_gx_publish_pe(0x47,token);
      }
      gekkoaot_test_gx_publish_pe(0x45,2);
    });parser.join();
    runtime.SyncNativeGXInterrupts();
    CHECK(runtime.pe_.Token()==5);
    CHECK(runtime.pe_.TokenInterruptPending());
    CHECK(runtime.pe_.FinishInterruptPending());
    CHECK(!runtime.native_gx_.PeEventPending());
    // Masked CPU interrupts never stop the parser's token register advancing.
    runtime.cpu_.msr &= ~0x8000u;
    gekkoaot_test_gx_publish_pe(0x47,6);
    runtime.SyncNativeGXInterrupts();
    CHECK(runtime.pe_.Token()==6);
    CHECK(runtime.pe_.TokenInterruptPending() && runtime.pe_.FinishInterruptPending());
    CHECK(runtime.pe_.Write(0xcc00100a,7,2));
    CHECK(!runtime.pe_.TokenInterruptPending() && runtime.pe_.FinishInterruptPending());
    CHECK(runtime.pe_.Write(0xcc00100a,11,2));
    CHECK(!runtime.pe_.FinishInterruptPending());
    CHECK(runtime.pi_.Write(0xcc003004,0x600,4));
    runtime.cpu_.msr |= 0x8000u;
    for (unsigned token=7;token<=9;++token) {
      gekkoaot_test_gx_publish_pe(0x48,token);
      gekkoaot_test_gx_publish_pe(0x47,token);
    }
    const char* option=std::getenv("GEKKOAOT_GX_ORDERED_PE_IRQ");
    const bool ordered=!option || option[0]!='0';
    runtime.SyncNativeGXInterrupts();
    if (ordered) {
      CHECK(runtime.pe_.Token()==7 && runtime.native_gx_.PeEventPending());
      CHECK(runtime.TryTakeExternalInterrupt());
      CHECK(!(runtime.cpu_.msr & 0x8000u));
      // MMIO status reads inside an IRQ cannot overwrite a callback that
      // the SDK has not acknowledged yet, even though EE is now clear.
      runtime.SyncNativeGXInterrupts();
      CHECK(runtime.pe_.Token()==7);
      for (unsigned token=7;token<=9;++token) {
        CHECK(runtime.pe_.Token()==token && runtime.pe_.TokenInterruptPending());
        CHECK(runtime.pe_.Write(0xcc00100a,7,2));
        runtime.SyncNativeGXInterrupts();
      }
      CHECK(!runtime.native_gx_.PeEventPending());
      CHECK(!runtime.pe_.TokenInterruptPending());
    } else {
      CHECK(runtime.pe_.Token()==9 && !runtime.native_gx_.PeEventPending());
      CHECK(runtime.pe_.Write(0xcc00100a,7,2));
    }
    // Clearing EE for a critical section cannot lose queued callbacks.
    // Explicit polling, rather than EE itself, requests the latest register.
    runtime.pe_notification_irq_active_=false;
    runtime.cpu_.msr &= ~0x8000u;
    for (unsigned token=10;token<=12;++token) {
      gekkoaot_test_gx_publish_pe(0x48,token);
      gekkoaot_test_gx_publish_pe(0x47,token);
    }
    gekkoaot_test_gx_publish_pe(0x45,2);
    runtime.SyncNativeGXInterrupts();
    if (ordered) CHECK(runtime.pe_.Token()==10 && runtime.native_gx_.PeEventPending());
    runtime.SyncNativeGXInterrupts(true);
    CHECK(runtime.pe_.Token()==12 && runtime.pe_.FinishInterruptPending());
    CHECK(!runtime.native_gx_.PeEventPending());
    CHECK(runtime.pe_.Write(0xcc00100a,15,2));
    // PI masking has the same latest-value semantics with EE enabled.
    CHECK(runtime.pi_.Write(0xcc003004,0,4));
    runtime.cpu_.msr |= 0x8000u;
    for (unsigned token=13;token<=15;++token) gekkoaot_test_gx_publish_pe(0x48,token);
    runtime.SyncNativeGXInterrupts();
    CHECK(runtime.pe_.Token()==15 && !runtime.native_gx_.PeEventPending());
  }
};
}
int main(int argc,char** argv) {
  CHECK(argc==2);
#if defined(_WIN32)
  _putenv_s("GEKKOAOT_RUNTIME_PARALLEL", "1");
#else
  setenv("GEKKOAOT_RUNTIME_PARALLEL", "1", 1);
#endif
  // HostRuntime plus the event queue exceed the default Windows 1 MiB stack.
  auto owner = std::make_unique<GekkoAOT::Native::HostRuntime>();
  auto& runtime = *owner;
  CHECK(runtime.InitializeNativeGX(argv[1]));
  GekkoAOT::Native::HardwareTestAccess::Verify(runtime);
  runtime.ShutdownNativeGX();
  GekkoAOT::GX::PeEventQueue queue;GekkoAOT::GX::PeEvent event;
  for(std::size_t i=0;i<queue.Capacity;++i) CHECK(queue.Push({0x48,std::uint32_t(i)}));
  CHECK(!queue.Push({0x48,0xffffffff}));
  for(std::size_t i=0;i<queue.Capacity;++i) {CHECK(queue.Pop(&event));CHECK(event.value==i);}
  CHECK(!queue.Pending());CHECK(!queue.Pop(&event));
  std::puts("Ordered PE burst/acknowledgement and bounded queue tests passed");
}
