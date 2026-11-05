// SPDX-License-Identifier: GPL-3.0-or-later
#include "native/runtime.h"
#include "sdk/device_proofs.h"
#if defined(GEKKOAOT_NATIVE_SDL_INPUT)
#include "platform/sdl_input.h"
#endif
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>

namespace GekkoAOT::Native {
namespace {
std::uint16_t U16(const std::uint8_t *p) {
  return (std::uint16_t(p[0]) << 8) | p[1];
}
std::uint32_t U32(const std::uint8_t *p) {
  return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) |
         (std::uint32_t(p[2]) << 8) | p[3];
}
void P16(std::uint8_t *p, std::uint16_t v) {
  p[0] = v >> 8;
  p[1] = v;
}
void P32(std::uint8_t *p, std::uint32_t v) {
  p[0] = v >> 24;
  p[1] = v >> 16;
  p[2] = v >> 8;
  p[3] = v;
}
} // namespace

bool HostRuntime::RejectHardwareAccess(std::uint32_t address, bool write) {
  if (!native_only_)
    return false;
  const auto p = AddressSpace::ToPhysical(address);
  const char *subsystem = nullptr;
  if (p >= 0x0c004000u && p < 0x0c005000u)
    subsystem = "MI";
  else if (p >= 0x0c006000u && p < 0x0c006400u)
    subsystem = "DI";
  else if (p >= 0x0c006400u && p < 0x0c006800u)
    subsystem = "SI";
  else if (p >= 0x0c006800u && p < 0x0c006c00u)
    subsystem = "EXI/CARD";
  else if (p >= 0x0c006c00u && p < 0x0c006c10u)
    subsystem = "AI";
  // Only DSP audio-buffer registers belong to AI; leave DSP/LLE/mail/ARAM
  // alone.
  else if (p >= 0x0c005030u && p < 0x0c00503cu)
    subsystem = "AI-buffer";
  if (!subsystem)
    return false;
  std::fprintf(stderr,
               "GEKKOAOT_NATIVE_ONLY_UNSUPPORTED subsystem=%s "
               "operation=mmio-%s address=%08x pc=%08x lr=%08x\n",
               subsystem, write ? "write" : "read", address, cpu_.pc, cpu_.lr);
  SetFault(write ? FaultKind::MmioWrite : FaultKind::MmioRead, address);
  return true;
}

std::uint64_t HostRuntime::NativeInputPeriod() const {
  if(!native_only_)return HW::VI::NativeVI::CpuClockHz/120u;
  const auto frame=std::uint64_t(vi_.TicksPerHalfLine())*vi_.HalfLinesPerFrame();
  return std::max<std::uint64_t>(1,frame/(2*devices_.input_samples_per_field));
}

bool HostRuntime::DispatchDeviceSdk(SimpleHleKind kind,std::uint32_t address) {
  return DispatchDeviceSdkNative<-1>(kind,address);
}
template<int NativeKind>
bool HostRuntime::DispatchDeviceSdkNative(SimpleHleKind dynamic_kind,std::uint32_t address) {
  const auto kind=NativeKind<0?dynamic_kind:static_cast<SimpleHleKind>(NativeKind);
  if (kind >= SimpleHleKind::DvdReadAsync && kind <= SimpleHleKind::DvdOpen)
    {if constexpr(NativeKind>=0)return DispatchDvdSdkNative<NativeKind>(kind,address);
     else return DispatchDvdSdk(kind,address);}
  if ((kind >= SimpleHleKind::CardInit && kind <= SimpleHleKind::CardFormatAsync) ||
      kind == SimpleHleKind::CardGetSerial || kind == SimpleHleKind::CardGetUnique ||
      kind == SimpleHleKind::CardCheckExAsync)
    {if constexpr(NativeKind>=0)return DispatchCardSdkNative<NativeKind>(kind,address);
     else return DispatchCardSdk(kind,address);}
  if (kind == SimpleHleKind::PadRead) {
    const auto output_address = cpu_.gpr[3];
    auto *out = memory_.Resolve(output_address, 48);
    if (!out)
      return false; // paged/unmapped operands are explicitly unsupported in
                    // strict mode
#if defined(GEKKOAOT_NATIVE_SDL_INPUT)
    // Refresh at the first real VI field read, then coalesce repeated reads
    // of that same field. Avoid an additional 0..8.3 ms snapshot age from the
    // background maintenance sampler without adding a fabricated input timer.
    if (devices_.pad_read_field != perf_vi_boundaries_total_) {
      if (!GekkoAOT::Input::HostInput().SampleOwner()) return false;
      devices_.pad_read_field = perf_vi_boundaries_total_;
      ++native_input_samples_;
    }
    cpu_.gpr[3] = GekkoAOT::Input::HostInput().ReadSnapshot(out);
    unsigned mode=devices_.pad_analog_mode;
    if(devices_.pad_analog_sda){
      std::uint32_t configured=0;
      if(!memory_.Read32(cpu_.gpr[13]+std::int32_t(*devices_.pad_analog_sda),&configured))return false;
      mode=(configured>>8)&7;
    }
    GekkoAOT::Input::ApplyAnalogMode(out,mode);
    for(unsigned port=0;port<4;++port) {
      auto* pad=out+12*port;
      const auto generation=native_only_ ? native_pad_publications_ : si_.ResponseGeneration(port);
      if(pad[10]==0 && (!generation || generation==devices_.pad_response_generation[port])) {
        // Retail PADRead clears the payload and reports PAD_ERR_TRANSFER when
        // SIGetResponse has no unread automatic reply. Returning another OK
        // sample here makes repeated game reads erase pressed/released edges.
        std::memset(pad,0,10);
        pad[10]=std::uint8_t(-3);
      }
      devices_.pad_response_generation[port]=generation;
    }
#else
    cpu_.gpr[3] = GekkoAOT::Input::Encode({}, out);
#endif
    static const bool trace=std::getenv("GEKKOAOT_PAD_READ_TRACE")!=nullptr;
    static const bool trace_all=[] {
      const auto* value=std::getenv("GEKKOAOT_PAD_READ_TRACE");
      return value && std::strcmp(value,"all")==0;
    }();
    if(trace){
      static std::array<std::uint8_t,48> previous{};static unsigned lines=0;
      bool changed=false;
      for(unsigned port=0;port<4;++port)
        changed |= U16(previous.data()+12*port)!=U16(out+12*port) || previous[12*port+10]!=out[12*port+10];
      // Opt-in diagnostics must still record Start after a long menu/gameplay
      // session; analog noise must not exhaust a lifetime line budget.
      if(trace_all || lines<8u || changed){
        std::uint32_t caller_frame=0,caller_lr=0;
        if(memory_.Read32(cpu_.gpr[1],&caller_frame) && caller_frame>=cpu_.gpr[1] &&
           caller_frame-cpu_.gpr[1]<=0x10000u)
          memory_.Read32(caller_frame+4,&caller_lr);
        std::fprintf(stderr,"GEKKOAOT_PAD_READ_TRACE read=%llu field=%llu lr=%08x caller=%08x output=%08x buttons=%04x,%04x,%04x,%04x err=%d,%d,%d,%d stick=%d,%d c=%d,%d\n",
          (unsigned long long)native_pad_reads_,(unsigned long long)perf_vi_boundaries_total_,cpu_.lr,
          caller_lr,output_address,
          U16(out),U16(out+12),U16(out+24),U16(out+36),int(std::int8_t(out[10])),int(std::int8_t(out[22])),
          int(std::int8_t(out[34])),int(std::int8_t(out[46])),int(std::int8_t(out[2])),int(std::int8_t(out[3])),
          int(std::int8_t(out[4])),int(std::int8_t(out[5])));
        std::memcpy(previous.data(),out,48);++lines;
      }
    }
    ++native_pad_reads_;
    cpu_.pc = cpu_.lr;
    return true;
  }
  const auto a = cpu_.gpr[3], b = cpu_.gpr[4], c = cpu_.gpr[5];
  const auto done = [&](std::uint32_t value) {
    cpu_.gpr[3] = value;
    cpu_.pc = cpu_.lr;
    return true;
  };
  const auto finish = [&]() {
    cpu_.pc = cpu_.lr;
    return true;
  };
  auto *scb = sram_control_address_
                  ? memory_.Resolve(sram_control_address_, 0x54)
                  : nullptr;
  const auto enable_lc = [&] {
    std::memset(lc_.Pointer(HW::LC::NativeLC::Base, 16384), 0, 16384);
    // Architectural CPU ABI metadata remains visible to mfspr/mfmsr; it
    // does not drive a cache/DMA machine. Data accesses use the host pointer.
    cpu_.msr |= 0x1000u;
    cpu_.hid2 |= 0x100f0000u;
    spr_state_[542] = 0xe00001feu;
    spr_state_[543] = 0xe0000002u;
  };
  switch (kind) {
  case SimpleHleKind::SiSetXy: {
    if(!native_only_ || !devices_.input_policy)return false;
    // Recognised SDK sampling profiles become a host snapshot frequency.
    // No serial transactions, virtual poll register or scanline state machine.
    constexpr std::array<std::pair<unsigned,unsigned>,17> profiles{{
      {246,2},{14,19},{30,9},{44,6},{52,5},{65,4},{87,3},{131,2},
      {296,2},{15,21},{29,11},{45,7},{52,6},{63,5},{78,4},{104,3},{156,2}}};
    const bool known=std::any_of(profiles.begin(),profiles.end(),[&](const auto& xy){
      return xy.second==b && (xy.first==a || xy.first*2==a);
    });
    if(!known)return false;
    auto* policy=memory_.Resolve(devices_.input_policy,4);if(!policy)return false;
    const auto value=(U32(policy)&0xfc0000ffu)|(a<<16)|(b<<8);
    P32(policy,value);devices_.input_samples_per_field=b;native_input_hw_phase_=0;
    return done(value);
  }
  case SimpleHleKind::ConsoleInit:
    if (!native_only_) return false;
    devices_.console = std::ferror(stdout) ? nullptr : stdout;
    return done(devices_.console ? 0 : 2);
  case SimpleHleKind::ConsoleWrite: {
    if (!native_only_) return false;
    if (!devices_.console) return done(2);
    auto* bytes = memory_.Resolve(a,b);if(!bytes)return false;
    // SDK WriteUARTN changes LF to CR in its mutable input buffer. Preserve
    // that ABI side effect, but send logical newlines to the host console.
    for (unsigned i=0;i<b;++i) if(bytes[i]=='\n')bytes[i]='\r';
    std::array<char,1024> packet;
    for (unsigned offset=0;offset<b;) {
      const auto length=std::min<unsigned>(b-offset,packet.size());
      for(unsigned i=0;i<length;++i)packet[i]=bytes[offset+i]=='\r'?'\n':char(bytes[offset+i]);
      if(std::fwrite(packet.data(),1,length,devices_.console)!=length)return done(3);
      offset+=length;
    }
    return done(0);
  }
  case SimpleHleKind::DvdInit:
    if (!native_only_) return false;
    if (!devices_.dvd_initialized) {
      if (!devices_.dvd_fs_init || !disc_image_size_ || !native_vfs_.Ready()) return false;
      devices_.dvd_initialized = true;
      // Keep the original SDK FST/current-directory software ABI. The media
      // provider/worker is already opened by AttachDiscImage, without DI.
      cpu_.pc = devices_.dvd_fs_init;
      return true;
    }
    return finish();
  case SimpleHleKind::DvdDriveStatus:
    // A host request owns the command until RAM publication/completion. The
    // original SDK's software queue cannot observe this native worker.
    if (!devices_.dvd.Empty()) return done(1);
    if (!native_only_) return false;
    return done(!disc_image_size_ ? 11 : devices_.dvd.Empty() ? 0 : 1);
  case SimpleHleKind::DvdCheckDisk:
    if (!native_only_) return false;
    return done(disc_image_size_ != 0);
  case SimpleHleKind::PadInit:
  case SimpleHleKind::SiInit:
    if (!native_only_) return false;
#if defined(GEKKOAOT_NATIVE_SDL_INPUT)
    if (!GekkoAOT::Input::HostInput().Initialize() ||
        !GekkoAOT::Input::HostInput().SampleOwner()) return false;
    return kind == SimpleHleKind::PadInit ? done(1) : finish();
#else
    return false;
#endif
  case SimpleHleKind::PadSetAnalogMode: {
    if(!native_only_ || a>7)return false;
    if(!devices_.pad_analog_sda)return false;
    const auto slot=cpu_.gpr[13]+std::int32_t(*devices_.pad_analog_sda);
    if(!memory_.Write32(slot,a<<8))return false;
    devices_.pad_analog_mode=a;
    return finish();
  }
  case SimpleHleKind::PadReset:
  case SimpleHleKind::PadRecalibrate:
    if (!native_only_ || (a&0x0fffffffu)) return false;
#if defined(GEKKOAOT_NATIVE_SDL_INPUT)
    return GekkoAOT::Input::HostInput().Initialize() &&
           GekkoAOT::Input::HostInput().ResetPortsOwner(a) ? done(1) : false;
#else
    return false;
#endif
  case SimpleHleKind::PadControlMotor:
  case SimpleHleKind::PadControlAllMotors:
#if defined(GEKKOAOT_NATIVE_SDL_INPUT)
    if (kind == SimpleHleKind::PadControlMotor) {
      if (!GekkoAOT::Input::HostInput().RumbleOwner(a,b)) return false;
    } else {
      const auto* commands=memory_.Resolve(a,16);if(!commands)return false;
      for(unsigned i=0;i<4;++i)if(U32(commands+4*i)>2)return false;
      for(unsigned i=0;i<4;++i)
        if(!GekkoAOT::Input::HostInput().RumbleOwner(i,U32(commands+4*i)))return false;
    }
    return finish();
#else
    return false;
#endif
  case SimpleHleKind::ExiInit:
    if (!native_only_) return false;
    // Initialise the actual filesystem/clock/font providers. Unsupported
    // EXI peripherals still fault; this does not install a transaction model.
    if (!native_exi_host_.Initialized()) native_exi_host_.Enable();
    return finish();
  case SimpleHleKind::OsReadRom: {
    auto* out = memory_.Resolve(a, b);
    if (!out || !native_exi_host_.ReadRom(c, {out,b})) return false;
    native_gx_.NotifyMemoryWrite(a,b);
    return done(1);
  }
  case SimpleHleKind::OsInitMemoryProtection: {
    if (!native_only_ || !devices_.set_interrupt_handler || !devices_.register_reset)
      return false;
    const auto* code = memory_.Resolve(address, 512);
    if (!code) return false;
    const auto proof = GekkoAOTSdk::ProveMemoryInit({code,512}, address,
        devices_.set_interrupt_handler, devices_.register_reset);
    auto* handlers = memory_.Resolve(0x80003040u, 5 * 4);
    std::uint32_t size = 0;
    if (!proof || !handlers || !memory_.Resolve(proof->handler, 4) ||
        !memory_.Resolve(proof->reset_info, 16) || !memory_.Read32(0x800000f0u, &size) ||
        size != AddressSpace::RetailMem1Size || !memory_.GrantRamReadWrite())
      return false;
    // Preserve CPU-visible BAT metadata from the SDK's 24 MiB ABI. Actual
    // loads/stores continue to use the host address window, without MI.
    for (const auto base : {528u, 536u}) {
      spr_state_[base] = 0x800001ffu; spr_state_[base + 1] = 2;
      spr_state_[base + 4] = 0x810000ffu; spr_state_[base + 5] = 0x01000002u;
    }
    for (unsigned channel = 0; channel < 5; ++channel)
      P32(handlers + channel * 4, proof->handler);
    cpu_.gpr[3] = 0xf0000000u;
    if (!DispatchDeviceSdk(SimpleHleKind::OsMaskInterrupts, address)) return false;
    cpu_.gpr[3] = 0x08000000u;
    if (!DispatchDeviceSdk(SimpleHleKind::OsUnmaskInterrupts, address)) return false;
    // Preserve the SDK reset callback registration via its ordinary AOT
    // routine. Tail-call it with the caller's unchanged ABI/return address;
    // no controller registers or fake protection interrupts are involved.
    cpu_.gpr[3] = proof->reset_info;
    cpu_.pc = devices_.register_reset;
    return true;
  }
  case SimpleHleKind::OsMaskInterrupts:
  case SimpleHleKind::OsUnmaskInterrupts:
  case SimpleHleKind::OsSetInterruptMask: {
    // Native completions have no MI/EXI/AI interrupt register banks. Keep
    // the SDK's global/local masks authoritative and apply the remaining
    // real GX/VI/DSP event enables without walking those absent controllers.
    // The compatibility mode still runs its original hardware mask routine.
    if (!native_only_) return false;
    auto* masks = memory_.Resolve(0x800000c4u, 8);
    if (!masks) return false;
    const auto old_global = U32(masks), old_local = U32(masks + 4);
    const bool local = kind == SimpleHleKind::OsSetInterruptMask;
    const auto global = local ? old_global :
        kind == SimpleHleKind::OsMaskInterrupts ? old_global | a : old_global & ~a;
    const auto combined = global | (local ? a : old_local);
    P32(masks, global);
    if (local) P32(masks + 4, a);
    std::uint32_t pi_mask = 0x40; // DSP owns its own source enables.
    constexpr std::array<std::uint32_t, 10> pi_causes{
        0x800, 0x200, 0x400, 0x8, 0x4, 0x2, 0x1, 0x100, 0x1000, 0x2000};
    for (unsigned i = 0; i < pi_causes.size(); ++i)
      if (!(combined & (0x80000000u >> (17 + i)))) pi_mask |= pi_causes[i];
    // SI and DI completions are delivered by native services, never PI.
    pi_mask &= ~(0x8u | 0x4u);
    dsp_.SetSdkInterruptMask(combined);
    pi_.Write(0xcc003004u, pi_mask, 4);
    return done(local ? old_local : old_global);
  }
  case SimpleHleKind::LcEnable:
    enable_lc();
    return finish();
  case SimpleHleKind::LcDisable:
    cpu_.hid2 &= ~0x10000000u;
    return finish();
  case SimpleHleKind::LcAlloc:
  case SimpleHleKind::LcAllocNoInvalidate:
  case SimpleHleKind::LcAllocOneTag:
  case SimpleHleKind::LcAllocTags: {
    const bool tags = kind == SimpleHleKind::LcAllocTags;
    const bool one = kind == SimpleHleKind::LcAllocOneTag;
    const auto address = (tags || one) ? b : a;
    const std::uint64_t bytes = one ? 32 : tags ? std::uint64_t(c) * 32 : b;
    if (bytes > HW::LC::NativeLC::Size || (address & 31u) || (bytes & 31u))
      return false;
    auto *p = lc_.Pointer(address, bytes);
    if (!p)
      return false;
    if (!tags && !one && !(cpu_.hid2 & 0x10000000u))
      enable_lc();
    std::memset(p, 0, bytes);
    return finish();
  }
  case SimpleHleKind::LcQueueLength:
    // All native LC copies complete before returning. This is the number of
    // outstanding host operations, not a fabricated hardware queue status.
    return done(0);
  case SimpleHleKind::LcQueueWait:
  case SimpleHleKind::LcFlushQueue:
    std::atomic_thread_fence(std::memory_order_acq_rel);
    return finish();
  case SimpleHleKind::LcLoadData:
  case SimpleHleKind::LcStoreData: {
    const std::uint64_t bytes = (std::uint64_t(c) + 31u) & ~std::uint64_t(31);
    if (bytes > HW::LC::NativeLC::Size || (a & 31u) || (b & 31u))
      return false;
    auto *dst = kind == SimpleHleKind::LcLoadData
                    ? static_cast<std::uint8_t *>(lc_.Pointer(a, bytes))
                    : memory_.Resolve(a, bytes);
    const auto *src =
        kind == SimpleHleKind::LcLoadData
            ? memory_.Resolve(b, bytes)
            : static_cast<const std::uint8_t *>(lc_.Pointer(b, bytes));
    if (!dst || !src)
      return false;
    std::memmove(dst, src, bytes);
    if (kind == SimpleHleKind::LcStoreData)
      native_gx_.NotifyMemoryWrite(a, bytes);
    return done((bytes + 4095) / 4096);
  }

  case SimpleHleKind::AiInit:
    if (!devices_.disable_scheduler || !devices_.enable_scheduler)
      return false;
    if (a && ((a & 7u) || a < 128 || !memory_.Resolve(a - 128, 128)))
      return false;
    if (!devices_.ai_initialized) {
      devices_.aid_callback = devices_.ais_callback = 0;
      devices_.audio_stack = a;
      devices_.ai_initialized = devices_.ai_owned = true;
      ai_.SetLeft(0);
      ai_.SetRight(0);
      ai_.SetTrigger(0);
      ai_.ResetCounter();
      ai_.EnableCounterEvents();
      ai_.SetStreamRate(true);
      ai_.SetDspRate(false);
      dsp_.EnableNativeAudioPlayback();
      if (auto* masks = memory_.Resolve(0x800000c4u,8)) {
        const auto global=U32(masks)&~0x04800000u; // SDK DSP_AI and AI callbacks
        P32(masks,global);dsp_.SetSdkInterruptMask(global|U32(masks+4));
      }
    }
    return finish();
  case SimpleHleKind::AiReset:
    if (!devices_.ai_owned)
      return false;
    devices_.ai_initialized = false;
    return finish();
  case SimpleHleKind::AiCheckInit:
    if (!devices_.ai_owned)
      return false;
    return done(devices_.ai_initialized);
  case SimpleHleKind::AiRegisterDmaCallback: {
    if (!devices_.ai_owned)
      return false;
    const auto old = devices_.aid_callback;
    devices_.aid_callback = a;
    return done(old);
  }
  case SimpleHleKind::AiRegisterStreamCallback: {
    if (!devices_.ai_owned)
      return false;
    const auto old = devices_.ais_callback;
    devices_.ais_callback = a;
    return done(old);
  }

  case SimpleHleKind::OsGetRtc: {
    auto *out = memory_.Resolve(a, 4);
    if (!out)
      return false;
    P32(out, native_exi_host_.Rtc());
    return done(1);
  }
  case SimpleHleKind::OsSetRtc:
    return done(native_exi_host_.SetRtc(a));
  case SimpleHleKind::OsInitSram:
    if (!scb)
      return false;
    std::copy(native_exi_host_.Sram().begin(), native_exi_host_.Sram().end(),
              scb);
    P32(scb + 0x40, 64);
    P32(scb + 0x44, 0);
    P32(scb + 0x48, 0);
    P32(scb + 0x4c, 1);
    P32(scb + 0x50, 0);
    return finish();
  case SimpleHleKind::OsLockSram:
  case SimpleHleKind::OsLockSramEx:
    if (!scb)
      return false;
    if (U32(scb + 0x48))
      return done(0);
    P32(scb + 0x44, (cpu_.msr & 0x8000u) ? 1 : 0);
    P32(scb + 0x48, 1);
    cpu_.msr &= ~0x8000u;
    return done(sram_control_address_ +
                (kind == SimpleHleKind::OsLockSramEx ? 20 : 0));
  case SimpleHleKind::OsUnlockSram:
  case SimpleHleKind::OsUnlockSramEx: {
    if (!scb || !U32(scb + 0x48))
      return false;
    if (a) {
      if (kind == SimpleHleKind::OsUnlockSram) {
        if ((scb[19] & 3) > 2)
          scb[19] &= ~3u;
        std::uint16_t sum = 0, inv = 0;
        for (unsigned i = 12; i < 20; i += 2) {
          const auto w = U16(scb + i);
          sum += w;
          inv += std::uint16_t(~w);
        }
        P16(scb, sum);
        P16(scb + 2, inv);
      }
      const auto gbs = U16(scb + 60);
      if ((gbs & 0x7c00u) == 0x5000u || (gbs & 0xc0u) == 0xc0u)
        P16(scb + 60, 0);
      const bool ok = native_exi_host_.CommitSram(
          std::span<const std::uint8_t, 64>(scb, 64));
      P32(scb + 0x4c, ok ? 1 : 0);
      P32(scb + 0x40, ok ? 64 : 0);
    }
    P32(scb + 0x48, 0);
    cpu_.msr = (cpu_.msr & ~0x8000u) | (U32(scb + 0x44) ? 0x8000u : 0);
    return done(U32(scb + 0x4c));
  }
  case SimpleHleKind::OsSyncSram:
    if (!scb)
      return false;
    return done(U32(scb + 0x4c));
  case SimpleHleKind::OsCheckSram: {
    if (!scb)
      return false;
    std::uint16_t sum = 0, inv = 0;
    for (unsigned i = 12; i < 20; i += 2) {
      const auto w = U16(scb + i);
      sum += w;
      inv += std::uint16_t(~w);
    }
    return done(U16(scb) == sum && U16(scb + 2) == inv);
  }
  case SimpleHleKind::AiInitDma:
    if (!dsp_.BindAudioBuffer(a, b))
      return false;
    return finish();
  case SimpleHleKind::AiStartDma:
    dsp_.SetAudioPlaying(true);
    return finish();
  case SimpleHleKind::AiStopDma:
    dsp_.SetAudioPlaying(false);
    return finish();
  case SimpleHleKind::AiGetDmaEnable:
    return done(dsp_.SdkAudioPlaying());
  case SimpleHleKind::AiGetDmaBytesLeft:
    return done(dsp_.AudioBytesLeft());
  case SimpleHleKind::AiGetDmaStart:
    return done(dsp_.SdkAudioSource());
  case SimpleHleKind::AiGetDmaLength:
    return done(dsp_.SdkAudioLength());
  case SimpleHleKind::AiGetDspRate:
    return done(ai_.DspSampleRateHz() == 48043);
  case SimpleHleKind::AiSetDspRate:
    if (a > 1)
      return false;
    if (a != std::uint32_t(ai_.DspSampleRateHz() == 48043) && a == 0)
      ai_.ResetCounter();
    ai_.SetDspRate(a);
    dsp_.SetAudioSampleRateHz(ai_.DspSampleRateHz());
    return finish();
  case SimpleHleKind::AiGetStreamRate:
    return done(ai_.StreamSampleRateHz() == 48043);
  case SimpleHleKind::AiSetStreamRate:
    if (a != 1)
      return finish(); // Retail SDK accepts only the 48 kHz public mode.
    if (ai_.StreamSampleRateHz() != 48043) {
      ai_.SetStreamRate(true);
      ai_.ResetCounter();
    }
    return finish();
  case SimpleHleKind::AiGetStreamState:
    return done(ai_.IsPlaying());
  case SimpleHleKind::AiSetStreamState:
    if (a > 1)
      return false;
    if (a && !ai_.IsPlaying() && ai_.StreamSampleRateHz() == 32028)
      ai_.ResetCounter();
    ai_.SetPlaying(a);
    return finish();
  case SimpleHleKind::AiGetCount:
    // Query the host audio clock even if a compiled caller loops without
    // returning to Run. There is no register decode or hardware polling here.
    ServiceTimedMmioPoll(0xcc006c08u);
    return done(ai_.SampleCounter());
  case SimpleHleKind::AiResetCount:
    ai_.ResetCounter();
    return finish();
  case SimpleHleKind::AiGetTrigger:
    return done(ai_.InterruptTiming());
  case SimpleHleKind::AiSetTrigger:
    ai_.SetTrigger(a);
    return finish();
  case SimpleHleKind::AiGetLeft:
    return done(ai_.Volume() & 255);
  case SimpleHleKind::AiGetRight:
    return done((ai_.Volume() >> 8) & 255);
  case SimpleHleKind::AiSetLeft:
    ai_.SetLeft(a);
    return finish();
  case SimpleHleKind::AiSetRight:
    ai_.SetRight(a);
    return finish();
  case SimpleHleKind::LcLoadBlocks:
  case SimpleHleKind::LcStoreBlocks: {
    if (c > 127)
      return false;
    const auto bytes = std::size_t(c ? c : 128) * 32;
    // SDK operands are destination, source, number of 32-byte blocks.
    auto *dst = kind == SimpleHleKind::LcLoadBlocks
                    ? static_cast<std::uint8_t *>(lc_.Pointer(a, bytes))
                    : memory_.Resolve(a, bytes);
    const auto *src =
        kind == SimpleHleKind::LcLoadBlocks
            ? memory_.Resolve(b, bytes)
            : static_cast<const std::uint8_t *>(lc_.Pointer(b, bytes));
    if (!dst || !src || (a & 31) || (b & 31))
      return false;
    std::memmove(dst, src, bytes);
    if (kind == SimpleHleKind::LcStoreBlocks)
      native_gx_.NotifyMemoryWrite(a, bytes);
    return finish();
  }
  default:
    return false;
  }
}
// Generate all specializations from the shared registry. Dead cases are
// removed inside each specialization, without relying on cross-DSO inlining.
#define SDK_SIMPLE(kind, name, abi) template bool HostRuntime::DispatchDeviceSdkNative<static_cast<int>(SimpleHleKind::kind)>(SimpleHleKind,std::uint32_t);
#define SDK_OS(kind, name, abi)
#include "sdk/native_services.def"
#undef SDK_SIMPLE
#undef SDK_OS
} // namespace GekkoAOT::Native
