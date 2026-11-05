// SPDX-License-Identifier: GPL-3.0-or-later
#include "native/runtime.h"
#include "sdk_fixture_words.h"
#include "native/sdk_autoresolver.h"
#include "sdk/device_proofs.h"
#include "sdk/card_sync_wrapper.h"
#if defined(GEKKOAOT_NATIVE_SDL_INPUT)
#include "platform/sdl_input.h"
namespace GekkoAOT::Input {
struct SDLInputTestAccess {
 static void Publish(const std::array<Pad,4>& pads){HostInput().published_.Publish(pads);}
};
}
#endif
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <memory>
#include <thread>
#define CHECK(x) do{if(!(x)){std::cerr<<__LINE__<<": " #x "\n";std::exit(1);}}while(false)
namespace GekkoAOT::Native {
struct HardwareTestAccess {
#include "native_sdk_test_routes.h"
 static bool Call(HostRuntime& r,SimpleHleKind kind,std::initializer_list<std::uint32_t> args){unsigned n=3;for(auto a:args)r.Cpu().gpr[n++]=a;r.Cpu().lr=0x80020000;r.Cpu().pc=0x80010000;return NativeSdkTestCall(r,kind,r.Cpu().pc);}
 static auto& Exi(HostRuntime& r){return r.native_exi_host_;}
 static auto& Ai(HostRuntime& r){return r.ai_;}
 static auto& Dsp(HostRuntime& r){return r.dsp_;}
 static auto& Si(HostRuntime& r){return r.si_;}
 static auto& Devices(HostRuntime& r){return r.devices_;}
 static void KeepPadSnapshot(HostRuntime& r){r.devices_.pad_read_field=r.perf_vi_boundaries_total_;}
 static void PublishNativePad(HostRuntime& r){++r.native_pad_publications_;}
 static bool Events(HostRuntime& r){return r.ServiceDeviceEvents();}
 static void Strict(HostRuntime& r,bool yes){r.native_only_=yes;r.fault_={};}
 static bool Faulted(HostRuntime& r){return r.fault_.kind!=HostRuntime::FaultKind::None;}
};
}
using namespace GekkoAOT::Native;
int main(){
  const auto dir=std::filesystem::temp_directory_path()/("gekkoaot-device-sdk-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  std::filesystem::create_directories(dir);
#if defined(_WIN32)
  _putenv_s("GEKKOAOT_USER_DIR",dir.string().c_str());_putenv_s("SDL_AUDIO_DRIVER","dummy");
#else
  setenv("GEKKOAOT_USER_DIR",dir.c_str(),1);setenv("SDL_AUDIODRIVER","dummy",1);setenv("GEKKOAOT_NATIVE_HW_EXI","1",1);
#endif
  auto r=std::make_unique<HostRuntime>();auto& cpu=r->Cpu();auto& mem=r->Memory();using K=SimpleHleKind;using T=HardwareTestAccess;
#if defined(GEKKOAOT_NATIVE_SDL_INPUT)
  {
    // One SI response is consumed once, even when a game reads twice during
    // a frame. The second read must not erase its newly pressed Start edge.
    auto pad_runtime=std::make_unique<HostRuntime>();
    auto& si=T::Si(*pad_runtime);
    GekkoAOT::HW::SI::NativeSI::DeviceHooks hooks;
    hooks.poll=[](void*,std::uint32_t port,std::uint32_t* hi,std::uint32_t* lo){
      *hi=0;*lo=0;return port==0;
    };
    si.SetDeviceHooks(hooks);
    CHECK(si.Write(0xcc006430,0x80,4));
    std::array<GekkoAOT::Input::Pad,4> pads{};
    pads[0].connected=true;pads[0].buttons=0x1000;
    GekkoAOT::Input::SDLInputTestAccess::Publish(pads);
    T::KeepPadSnapshot(*pad_runtime);
    auto read=[&](unsigned err,unsigned buttons){
      CHECK(T::Call(*pad_runtime,K::PadRead,{0x80060000}));
      const auto* bytes=pad_runtime->Memory().Resolve(0x80060000,48);
      CHECK(bytes && bytes[10]==err && ((unsigned(bytes[0])<<8)|bytes[1])==buttons);
      CHECK(bytes[22]==255 && bytes[34]==255 && bytes[46]==255);
      if(err==253)for(unsigned i=0;i<10;++i)CHECK(bytes[i]==0);
    };
    read(253,0); // no SI reply yet
    si.PollNow();read(0,0x1000);read(253,0);
    si.PollNow();read(0,0x1000); // a new unchanged sample is still valid
    pads[0].buttons=0;GekkoAOT::Input::SDLInputTestAccess::Publish(pads);
    si.PollNow();read(0,0);read(253,0);
    T::Strict(*pad_runtime,true);
    T::Devices(*pad_runtime).pad_response_generation={};
    T::PublishNativePad(*pad_runtime);read(0,0);read(253,0);
    T::PublishNativePad(*pad_runtime);read(0,0);
    GekkoAOT::Input::HostInput().SampleOwner();
  }
#endif
  const auto ai_counter=SdkFixtureArray<4>("device_ai_counter");
  for(unsigned i=0;i<ai_counter.size();++i)CHECK(mem.Write32(0x80052000+4*i,ai_counter[i]));
  CHECK(GekkoAOTSdk::ProveAiCounter({mem.Resolve(0x80052000,16),16}));
  CHECK(mem.Write32(0x80052000,0x3c60803d)); // same shape, ordinary RAM getter
  CHECK(!GekkoAOTSdk::ProveAiCounter({mem.Resolve(0x80052000,16),16}));
  r->RegisterSimpleHle(K::AiGetCount,0x80052000);
  T::Strict(*r,true);cpu.pc=0x80052000;cpu.lr=0x80020000;
  CHECK(!cpu.host_call(&cpu,0xfffc0000u|unsigned(K::AiGetCount)));
  CHECK(!T::Faulted(*r));
  T::Strict(*r,false);
  r->RegisterSimpleHle(K::DvdReadPrio,0x80052100);
  T::Strict(*r,true);cpu.pc=0x80052100;cpu.lr=0x80020000;
  CHECK(!cpu.host_call(&cpu,0xfffc0000u|unsigned(K::DvdReadPrio)));
  CHECK(!T::Faulted(*r)); // SDK software wait, with native async media I/O
  T::Strict(*r,false);
  const auto si_policy2001=SdkFixtureArray<24>("device_si_policy2001");
  for(unsigned i=0;i<si_policy2001.size();++i)CHECK(mem.Write32(0x80053000+4*i,si_policy2001[i]));
  CHECK(GekkoAOTSdk::ProveSiPolicy({mem.Resolve(0x80053000,96),96})==0x80010004);
  CHECK(mem.Write32(0x80053028,0x84040008));
  CHECK(!GekkoAOTSdk::ProveSiPolicy({mem.Resolve(0x80053000,96),96}));
  // Older SDKs keep a global disk ID and 0x108-byte slots. Publishing the
  // newer per-slot disk pointer at +0x10c would overwrite slot B's result.
  {
    auto legacy=std::make_unique<HostRuntime>();auto& m=legacy->Memory();
    const auto legacy_card=SdkFixtureArray<87>("device_legacy_card_control");
    for(unsigned n=0;n<legacy_card.size();++n)CHECK(m.Write32(0x80054000+4*n,legacy_card[n]));
    CHECK(GekkoAOTSdk::ProveLegacyCardControl({m.Resolve(0x80054000,348),348}));
    CHECK(m.Write32(0x80054000+22*4,0x3bde0110));
    CHECK(!GekkoAOTSdk::ProveLegacyCardControl({m.Resolve(0x80054000,348),348}));
    CHECK(m.Write32(0x80054000+22*4,0x3bde0108));
    legacy->RegisterSimpleHle(K::CardInit,0x80054000);
    legacy->Cpu().gpr[13]=0x80070000;
    CHECK(m.Write32(0x80060210,0xfeedface));
    CHECK(T::Call(*legacy,K::CardInit,{}));
    std::uint32_t value=0;
    CHECK(m.Read32(0x8006010c,&value)&&value==std::uint32_t(-3));
    CHECK(m.Read32(0x80060210,&value)&&value==0xfeedface);
    CHECK(m.Read32(0x80070030,&value)&&value==0x80000000);
    CHECK(T::Call(*legacy,K::CardSetGlobalDiskId,{0}));
    CHECK(m.Read32(0x80070030,&value)&&value==0x80060210);
  }
  const auto mode_setter=SdkFixtureArray<5>("device_mode_setter");
  for(unsigned i=0;i<mode_setter.size();++i)CHECK(mem.Write32(0x80055000+4*i,mode_setter[i]));
  CHECK(GekkoAOTSdk::ProvePadAnalogModeCell({mem.Resolve(0x80055000,20),20})==std::int16_t(-96));
  CHECK(mem.Write32(0x80055008,0x57e6482e));
  CHECK(!GekkoAOTSdk::ProvePadAnalogModeCell({mem.Resolve(0x80055000,20),20}));
  const auto disk_setter=SdkFixtureArray<14>("device_disk_setter");
  for(unsigned i=0;i<disk_setter.size();++i)CHECK(mem.Write32(0x80056000+4*i,disk_setter[i]));
  CHECK(GekkoAOTSdk::ProveCardDiskSetter({mem.Resolve(0x80056000,56),56})==0x80020000);
  CHECK(mem.Write32(0x80056030,0x90640218));
  CHECK(!GekkoAOTSdk::ProveCardDiskSetter({mem.Resolve(0x80056000,56),56}));
  CHECK(mem.Write32(0x80056030,disk_setter[12]));
  r->RegisterSimpleHle(K::CardSetGlobalDiskId,0x80056000);
  // The original CARDControl ABI is observed by inlined retail helpers.
  // Recover it only from a complete, known getter; one changed offset fails.
  const auto getter=SdkFixtureArray<6>("device_getter");
  for(unsigned i=0;i<getter.size();++i)CHECK(mem.Write32(0x80003100+4*i,getter[i]));
  CHECK(GekkoAOTSdk::ProveCardControl({mem.Resolve(0x80003100,24),24})==0x80020000);
  CHECK(mem.Write32(0x80003110,0x80630108));
  CHECK(!GekkoAOTSdk::ProveCardControl({mem.Resolve(0x80003100,24),24}));
  CHECK(mem.Write32(0x80003110,0x8063010c));
  r->RegisterSimpleHle(K::CardGetDiskId,0x80003100);
  CHECK(T::Call(*r,K::CardInit,{}));
  std::uint32_t software_result=0;CHECK(mem.Read32(0x80020004,&software_result));CHECK(software_result==std::uint32_t(-3));
  // Exact structural proof supplies the original SDK SRAM control object;
  // no arbitrary guest RAM reservation or hard-coded per-game address.
  CHECK(mem.Write32(0x80003000,0x3c608001));CHECK(mem.Write32(0x80003004,0x38630000));CHECK(mem.Write32(0x80003008,0x8063004c));CHECK(mem.Write32(0x8000300c,0x4e800020));
  r->RegisterSimpleHle(K::OsSyncSram,0x80003000);
  auto unlock_family=SdkFixtureArray<22>("device_unlock_family");
  for(const unsigned branch:{4u,13u})unlock_family[branch]=0x48000001u|((0x80050000u-(0x80051000u+4*branch))&0x03fffffcu);
  for(unsigned i=0;i<unlock_family.size();++i)CHECK(mem.Write32(0x80051000+4*i,unlock_family[i]));
  CHECK(GekkoAOTSdk::ProveSramUnlockFamily({mem.Resolve(0x80051000,88),88},0x80051000));
  CHECK(mem.Write32(0x80051004,0x38800108)); // unrelated constructor mode
  CHECK(!GekkoAOTSdk::ProveSramUnlockFamily({mem.Resolve(0x80051000,88),88},0x80051000));
  r->RegisterSimpleHle(K::OsUnlockSram,0x80051000);
  T::Strict(*r,true);cpu.pc=0x80051000;cpu.lr=0x80020000;
  CHECK(!cpu.host_call(&cpu,0xfffc0000u|unsigned(K::OsUnlockSram)));
  CHECK(!T::Faulted(*r)); // original AOT game body, never a hardware fallback
  T::Strict(*r,false);
  CHECK(mem.Write32(0x80051004,0x38800000));CHECK(mem.Write32(0x80051000+13*4,unlock_family[13]+4));
  CHECK(!GekkoAOTSdk::ProveSramUnlockFamily({mem.Resolve(0x80051000,88),88},0x80051000));
  CHECK(T::Call(*r,K::OsInitSram,{}));cpu.msr|=0x8000;
  CHECK(T::Call(*r,K::OsLockSram,{}));CHECK(cpu.gpr[3]==0x80010000);CHECK(!(cpu.msr&0x8000));
  CHECK(T::Call(*r,K::OsLockSramEx,{}));CHECK(cpu.gpr[3]==0); // non-reentrant SDK lock
  auto* sram=mem.Resolve(0x80010000,64);sram[18]=3;sram[19]=0xff;
  CHECK(T::Call(*r,K::OsUnlockSram,{1}));CHECK(cpu.gpr[3]==1);CHECK(cpu.msr&0x8000);CHECK((sram[19]&3)==0);
  CHECK(T::Call(*r,K::OsCheckSram,{}));CHECK(cpu.gpr[3]==1);CHECK(T::Exi(*r).Sram()[18]==3);
  CHECK(T::Call(*r,K::OsGetRtc,{0x80012000}));std::uint32_t rtc=0;CHECK(mem.Read32(0x80012000,&rtc));CHECK(T::Call(*r,K::OsSetRtc,{rtc-123}));
  CHECK(T::Call(*r,K::OsGetRtc,{0x80012000}));std::uint32_t changed=0;CHECK(mem.Read32(0x80012000,&changed));CHECK(changed>=rtc-123&&changed<=rtc-122);
  CHECK(std::filesystem::file_size(dir/"SRAM.raw")==64);CHECK(std::filesystem::file_size(dir/"RTC.bias")==8);
  // Direct AI calls preserve the SDK state, real sample cadence and IRQ
  // boundary. DMA playback still uses the DSP renderer (LLE remains present).
  CHECK(T::Call(*r,K::AiSetLeft,{123}));CHECK(T::Call(*r,K::AiSetRight,{231}));CHECK(T::Ai(*r).Volume()==0xe77b);
  CHECK(T::Call(*r,K::AiSetTrigger,{48043}));CHECK(T::Call(*r,K::AiSetStreamState,{1}));
  T::Ai(*r).AdvanceCycles(486000000);CHECK(T::Ai(*r).SampleCounter()==48043);CHECK(T::Ai(*r).Control()&8);
  CHECK(T::Call(*r,K::AiResetCount,{}));CHECK(T::Ai(*r).SampleCounter()==0);
  CHECK(T::Call(*r,K::AiInitDma,{0x80014000,640}));CHECK(T::Dsp(*r).AudioDmaSource()==0x14000);CHECK((T::Dsp(*r).AudioDmaControl()&0x7fff)==20);
  CHECK(T::Call(*r,K::AiStartDma,{}));CHECK(T::Dsp(*r).AudioDmaControl()&0x8000);CHECK(T::Call(*r,K::AiStopDma,{}));CHECK(!(T::Dsp(*r).AudioDmaControl()&0x8000));
  CHECK(!T::Call(*r,K::AiInitDma,{0x80014001,640}));CHECK(!T::Call(*r,K::AiInitDma,{0x80014000,641}));
  auto* ram=mem.Resolve(0x80018000,4096);for(unsigned i=0;i<4096;++i)ram[i]=i*13;
  CHECK(T::Call(*r,K::LcLoadBlocks,{0xe0000000,0x80018000,0}));std::memset(ram,0,4096);CHECK(T::Call(*r,K::LcStoreBlocks,{0x80018000,0xe0000000,0}));CHECK(ram[37]==std::uint8_t(37*13));
  CHECK(!T::Call(*r,K::LcLoadBlocks,{0xe003ffe0,0x80018000,2}));
  // Native queued reads must remain BUSY even with the guest scheduler ABI.
  CHECK(T::Devices(*r).dvd.Submit({.block=0x80018000,.destination=0x80019000,.bytes=32}));
  CHECK(T::Call(*r,K::DvdDriveStatus,{}));CHECK(cpu.gpr[3]==1);
  T::Devices(*r).dvd.Reset();
  CHECK(!T::Call(*r,K::DvdDriveStatus,{})); // preserve legacy error/media states
  // A real pending CARD result is published before invoking a callback.
  // Callback CPU clobbers are isolated, but elapsed time is not rewound.
  r->RegisterDeviceSupport("OSDisableScheduler",0x80030000);r->RegisterDeviceSupport("OSEnableScheduler",0x80030100);
  CHECK(T::Call(*r,K::CardMountAsync,{0,0x80040000,0,0x80030200}));CHECK(T::Devices(*r).completion[0].active);
  CHECK(mem.Read32(0x80020004,&software_result));CHECK(software_result==std::uint32_t(-1));
  cpu.pc=0x80023456;cpu.lr=0x80029998;cpu.gpr[3]=987;cpu.gpr[1]=0x8007fff0;cpu.msr|=0x8000;cpu.timebase=100;
  CHECK(mem.Write32(0x800000d4,0x80060000));CHECK(mem.Write32(0x800000d8,0x80061000));CHECK(mem.Write32(0x800000c0,0x60000));
  CHECK(mem.Write32(cpu.gpr[1]+4,0x12345678));
  CHECK(mem.Write32(cpu.gpr[1]-4,0x89abcdef)); // split-prologue spill
  CHECK(T::Events(*r));CHECK(cpu.pc==0x80030000);CHECK(!(cpu.msr&0x8000));
  CHECK(cpu.gpr[1]==0x8007fff0-DeviceState::CallbackFrameBytes);CHECK(mem.Write32(cpu.gpr[1]+4,DeviceState::ReturnPc));
  cpu.pc=DeviceState::ReturnPc;CHECK(T::Events(*r));CHECK(cpu.pc==0x80030200);CHECK(cpu.gpr[3]==0&&cpu.gpr[4]==0);
  cpu.gpr[3]=42;cpu.fpr[7]=1234;cpu.pc=DeviceState::ReturnPc;CHECK(T::Events(*r));CHECK(cpu.pc==0x80030100);
  CHECK(mem.Write32(0x800000d4,0x80062000));CHECK(mem.Write32(0x800000d8,0x80062000));
  cpu.pc=DeviceState::ReturnPc;cpu.timebase=200;CHECK(T::Events(*r));CHECK(cpu.pc==0x80023456&&cpu.lr==0x80029998&&cpu.gpr[3]==987&&cpu.fpr[7]==0&&cpu.timebase==200);CHECK(cpu.msr&0x8000);
  CHECK(!T::Devices(*r).completion[0].active);CHECK(!T::Events(*r));
  CHECK(cpu.gpr[1]==0x8007fff0);CHECK(mem.Read32(cpu.gpr[1]+4,&software_result));CHECK(software_result==0x12345678);
  CHECK(mem.Read32(cpu.gpr[1]-4,&software_result));CHECK(software_result==0x89abcdef);
  CHECK(mem.Read32(0x800000d4,&software_result));CHECK(software_result==0x80060000);
  CHECK(mem.Read32(0x800000d8,&software_result));CHECK(software_result==0x80061000);
  CHECK(mem.Read32(0x80020004,&software_result));CHECK(software_result==0);
  CHECK(mem.Read32(0x80020080,&software_result));CHECK(software_result==0x80040000);
  CHECK(mem.Read32(0x8002010c,&software_result));CHECK(software_result==0x80000000);
  CHECK(T::Call(*r,K::CardGetSerial,{0,0x80015000}));CHECK(cpu.gpr[3]==0);
  const auto raw=T::Exi(*r).Card(0)->Image();
  for(unsigned i=0;i<8;++i)CHECK(mem.Resolve(0x80015000,8)[i]==(raw[i]^raw[i+8]^raw[i+16]^raw[i+24]));
  CHECK(T::Call(*r,K::CardGetSerial,{1,0x80015000}));CHECK(cpu.gpr[3]==std::uint32_t(-3));
  CHECK(T::Call(*r,K::CardGetUnique,{0,0x80015000}));CHECK(cpu.gpr[3]==0);
  // Exercise the SDK frontend as well as the RAW core: real persistence,
  // exactly one completion, and cancelled reads never publish destination RAM.
  std::memcpy(mem.Resolve(0x80016000,7),"native",7);
  const auto card_callback=[&](int expected) {
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(3);
    while(!T::Events(*r)){CHECK(std::chrono::steady_clock::now()<deadline);std::this_thread::yield();}
    CHECK(cpu.pc==0x80030000);cpu.pc=DeviceState::ReturnPc;CHECK(T::Events(*r));
    CHECK(cpu.pc==0x80030200&&cpu.gpr[3]==0&&cpu.gpr[4]==std::uint32_t(expected));
    cpu.pc=DeviceState::ReturnPc;CHECK(T::Events(*r));cpu.pc=DeviceState::ReturnPc;CHECK(T::Events(*r));
  };
  CHECK(T::Call(*r,K::CardCreateAsync,{0,0x80016000,8192,0x80016100,0x80030200}));CHECK(cpu.gpr[3]==0);
  card_callback(0);
  auto* payload=mem.Resolve(0x80018000,8192);for(unsigned i=0;i<8192;++i)payload[i]=i*23;
  CHECK(T::Call(*r,K::CardWriteAsync,{0x80016100,0x80018000,8192,0,0x80030200}));CHECK(cpu.gpr[3]==0);
  card_callback(0);
  auto* destination=mem.Resolve(0x8001a000,8192);std::memset(destination,0xff,8192);
  CHECK(T::Call(*r,K::CardReadAsync,{0x80016100,0x8001a000,8192,0,0x80030200}));CHECK(cpu.gpr[3]==0);
  CHECK(T::Call(*r,K::CardCancel,{0x80016100}));CHECK(cpu.gpr[3]==std::uint32_t(-14));
  card_callback(-14);for(unsigned i=0;i<8192;++i)CHECK(destination[i]==255);
  CHECK(T::Call(*r,K::CardReadAsync,{0x80016100,0x8001a000,8192,0,0x80030200}));CHECK(cpu.gpr[3]==0);
  card_callback(0);CHECK(std::memcmp(payload,destination,8192)==0);CHECK(!T::Devices(*r).completion[0].active);
  // Native AI buffer requests are host completions, with no PI/DSP AID
  // interrupt or synthetic 200-cycle startup timer. Playback preserves the
  // real sample cadence even while completion callbacks are delayed.
  CHECK(T::Call(*r,K::AiInit,{0}));CHECK(T::Call(*r,K::AiRegisterDmaCallback,{0x80030500}));
  CHECK(T::Call(*r,K::AiInitDma,{0x80014000,640}));
  const auto aid_before=T::Dsp(*r).AidInterruptCount();
  CHECK(T::Call(*r,K::AiStartDma,{}));CHECK(T::Dsp(*r).AudioCompletionPending());
  T::Dsp(*r).AdvanceCycles(4860000);CHECK(T::Dsp(*r).AudioFramesSubmitted()==320);
  const auto complete_audio_callback=[&]{CHECK(T::Events(*r));cpu.pc=DeviceState::ReturnPc;CHECK(T::Events(*r));CHECK(cpu.pc==0x80030500);cpu.pc=DeviceState::ReturnPc;CHECK(T::Events(*r));cpu.pc=DeviceState::ReturnPc;CHECK(T::Events(*r));};
  complete_audio_callback();CHECK(!T::Dsp(*r).AudioCompletionPending());
  T::Dsp(*r).AdvanceCycles(1);CHECK(T::Dsp(*r).AudioFramesSubmitted()==320);CHECK(!T::Dsp(*r).AudioCompletionPending());
  T::Dsp(*r).AdvanceCycles(4860000);CHECK(T::Dsp(*r).AudioFramesSubmitted()==640);CHECK(T::Dsp(*r).AudioCompletionPending());
  complete_audio_callback();T::Dsp(*r).AdvanceCycles(1);CHECK(T::Dsp(*r).AudioFramesSubmitted()==640);
  CHECK(T::Dsp(*r).AidInterruptCount()==aid_before);
  CHECK(T::Call(*r,K::AiStopDma,{}));
  // SDK global and local masks remain authoritative in Native-Only. No
  // virtual MI/AI/EXI register access is required to change delivery masks.
  T::Strict(*r,true);cpu.exception=0;cpu.msr|=0x8000;
  CHECK(mem.Write32(0x800000c4,0));CHECK(mem.Write32(0x800000c8,0));
  CHECK(T::Call(*r,K::OsMaskInterrupts,{0xffffffe0}));CHECK(cpu.gpr[3]==0);CHECK(cpu.msr&0x8000);
  CHECK(mem.Read32(0x800000c4,&software_result));CHECK(software_result==0xffffffe0);
  CHECK(T::Call(*r,K::OsUnmaskInterrupts,{0x80}));CHECK(cpu.gpr[3]==0xffffffe0);
  CHECK((r->ProcessorInterface().InterruptMaskValue()&0x100)!=0);
  CHECK(T::Call(*r,K::OsSetInterruptMask,{0x80}));CHECK(cpu.gpr[3]==0);
  CHECK((r->ProcessorInterface().InterruptMaskValue()&0x100)==0);
  CHECK(!T::Faulted(*r));
  const auto sampling=SdkFixtureArray<27>("device_sampling");
  for(unsigned i=0;i<sampling.size();++i)CHECK(mem.Write32(0x80051000+4*i,sampling[i]));
  r->RegisterSimpleHle(K::SiSetXy,0x80051000);
  CHECK(T::Call(*r,K::SiSetXy,{246,2}));CHECK(cpu.gpr[3]==0x00f60200);
  CHECK(T::Devices(*r).input_samples_per_field==2);
  CHECK(T::Call(*r,K::SiSetXy,{14,19}));CHECK(T::Devices(*r).input_samples_per_field==19);
  CHECK(mem.Write32(0x80051000+4*5,0x546f801e));
  CHECK(!GekkoAOTSdk::ProveSiPolicy({mem.Resolve(0x80051000,108),108}));
  // A module compiled with a stale service plan must never reinterpret a
  // device token as a different, potentially destructive CARD operation.
  r->RegisterSimpleHle(K::OsGetRtc,0x80050000);
  cpu.pc=0x80050000;cpu.lr=0x80020000;cpu.gpr[3]=0x80015000;
  CHECK(cpu.host_call(&cpu,0xfffc0000u|unsigned(K::OsGetRtc)));
  cpu.pc=0x80050000;cpu.gpr[3]=0;
  CHECK(!cpu.host_call(&cpu,0xfffc0000u|unsigned(K::CardUnmount)));
  CHECK(T::Devices(*r).cards[0].Mounted());CHECK(T::Faulted(*r));
  T::Strict(*r,true);cpu.exception=0;
  CHECK(T::Call(*r,K::ConsoleWrite,{0x80015000,0}));CHECK(cpu.gpr[3]==2);
  CHECK(T::Call(*r,K::ConsoleInit,{}));CHECK(cpu.gpr[3]==0);
  auto* console=std::tmpfile();CHECK(console);T::Devices(*r).console=console;
  std::memcpy(mem.Resolve(0x80015000,8),"one\ntwo\n",8);
  CHECK(T::Call(*r,K::ConsoleWrite,{0x80015000,8}));CHECK(cpu.gpr[3]==0);
  CHECK(std::memcmp(mem.Resolve(0x80015000,8),"one\rtwo\r",8)==0);
  std::rewind(console);std::array<char,8> console_text{};
  CHECK(std::fread(console_text.data(),1,8,console)==8);CHECK(std::memcmp(console_text.data(),"one\ntwo\n",8)==0);
  T::Devices(*r).console=nullptr;std::fclose(console);
  // Native memory initialisation grants real host RAM permissions and retains
  // the SDK handler/reset registrations. Unproven variants fail closed.
  r->RegisterDeviceSupport("__OSSetInterruptHandler",0x80035000);
  r->RegisterDeviceSupport("OSRegisterResetFunction",0x80035100);
  unsigned at=0;
  const auto insn=[&](std::uint32_t word){CHECK(mem.Write32(0x80010000+4*at++,word));};
  const auto branch=[&](std::uint32_t target){insn(0x48000001u|((target-(0x80010000+4*at))&0x03fffffcu));};
  insn(0x3fc08004);insn(0x3bde6000); // r30 = handler 0x80046000
  for(unsigned channel=0;channel<5;++channel){insn(0x38600000|channel);insn(0x7fc4f378);branch(0x80035000);}
  insn(0x3c608004);insn(0x38637000);branch(0x80035100);insn(0x4e800020);
  CHECK(mem.Write32(0x800000f0,AddressSpace::RetailMem1Size));
  CHECK(T::Call(*r,K::OsInitMemoryProtection,{}));CHECK(cpu.pc==0x80035100&&cpu.gpr[3]==0x80047000);
  CHECK(mem.Read32(0x80003050,&software_result));CHECK(software_result==0x80046000);
  CHECK(mem.Write32(0x80010000+4*4,0x60000000)); // remove one mandatory registration
  T::Strict(*r,true);cpu.exception=0;CHECK(!T::Call(*r,K::OsInitMemoryProtection,{}));CHECK(T::Faulted(*r));
  T::Strict(*r,true);cpu.exception=0;
  // All six domains: direct RAM and LC still work; raw MI, DI, SI, EXI,
  // AI and audio-buffer accesses fault before changing virtual device state.
  for(const unsigned address:{0xcc004000u,0xcc006000u,0xcc006400u,0xcc006800u,0xcc006c00u,0xcc005030u}){
    T::Strict(*r,true);cpu.exception=0;cpu.external_write(&cpu,address,0xffffffffu,address==0xcc005030?2:4);CHECK(T::Faulted(*r));
    T::Strict(*r,true);cpu.exception=0;(void)cpu.external_read(&cpu,address,address==0xcc005030?2:4);CHECK(T::Faulted(*r));
  }
  T::Strict(*r,true);cpu.exception=0;cpu.external_write(&cpu,0x80018000,0x12345678,4);CHECK(!T::Faulted(*r));
  CHECK(T::Call(*r,K::LcLoadBlocks,{0xe0000000,0x80018000,1}));CHECK(!T::Faulted(*r));
  CHECK(!T::Call(*r,K::AiInitDma,{0x80014001,640}));CHECK(T::Faulted(*r));
  // Relocation-normalised CARD wrappers can have identical shapes while
  // calling different Async functions. Reject stale destructive labels.
  const auto wrapper=SdkFixtureArray<18>("device_wrapper");
  for(unsigned i=0;i<wrapper.size();++i)CHECK(mem.Write32(0x80052000+4*i,wrapper[i]));
  const auto manifest=dir/"card-manifest.txt";
  {std::ofstream out(manifest);out<<"0x80052000 CARDDelete\n0x80052100 CARDFormatAsync\n";}
  CHECK(!RegisterNativeSdkManifest(*r,manifest));
  {std::ofstream out(manifest);out<<"0x80052000 CARDDelete\n0x80052100 CARDDeleteAsync\n";}
  CHECK(RegisterNativeSdkManifest(*r,manifest));
  r.reset();std::filesystem::remove_all(dir);
  std::cout<<"Native SDK: SRAM/RTC persistence, AI clock, LC, callback state/ordering, strict MI/DI/SI/EXI/AI rejection PASS\n";
}
