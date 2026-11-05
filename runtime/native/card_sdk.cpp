// SPDX-License-Identifier: GPL-3.0-or-later
#include "native/runtime.h"
#include <algorithm>
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
void P16(std::uint8_t *p, unsigned v) {
  p[0] = v >> 8;
  p[1] = v;
}
void P32(std::uint8_t *p, std::uint32_t v) {
  p[0] = v >> 24;
  p[1] = v >> 16;
  p[2] = v >> 8;
  p[3] = v;
}
bool TraceCards() {
  static const bool enabled = [] {
    const char* value = std::getenv("GEKKOAOT_CARD_TRACE");
    return value && std::string_view(value) == "1";
  }();
  return enabled;
}
const char* CardApiName(SimpleHleKind kind) {
  switch (kind) {
#define DEVICE_SDK(id, name) case SimpleHleKind::id: return name;
#include "native/device_sdk.def"
#undef DEVICE_SDK
  default: return "unknown";
  }
}
Card::SdkCard::Info Info(const std::uint8_t *p) {
  return {int(U32(p + 4)), U32(p + 8), std::int32_t(U32(p + 12)), U16(p + 16)};
}
void PutInfo(std::uint8_t *p, unsigned slot, const Card::SdkCard::Info &i) {
  P32(p, slot);
  P32(p + 4, i.file);
  P32(p + 8, i.offset);
  P32(p + 12, i.length);
  P16(p + 16, i.block);
}
} // namespace
void HostRuntime::RegisterDeviceSupport(std::string_view name,
                                        std::uint32_t address) {
  if (name == "OSDisableScheduler")
    devices_.disable_scheduler = address;
  if (name == "OSEnableScheduler")
    devices_.enable_scheduler = address;
  if (name == "__OSSetInterruptHandler") devices_.set_interrupt_handler = address;
  if (name == "OSRegisterResetFunction") devices_.register_reset = address;
  if (name == "__DVDFSInit") devices_.dvd_fs_init = address;
}
void HostRuntime::ResetDeviceServices() {
  for (auto &card : devices_.cards) {
    card.Wait();
    card.Unmount();
  }
  devices_.dvd.Reset();
  devices_.dvd_bytes.clear();
  devices_.dvd_cancel_callback = devices_.dvd_cancel_block = 0;
  devices_.completion = {};
  devices_.disk = {0x80000000u, 0x80000000u};
  devices_.detach = {};
  devices_.card_work = {};
  devices_.card_control = 0;devices_.card_stride=0x110;devices_.card_disk_none=0;devices_.card_disk_sda=0;
  devices_.card_initialized = false;
  devices_.pad_read_field = ~std::uint64_t(0);
  devices_.pad_response_generation = {};
  devices_.input_policy = 0;devices_.input_samples_per_field=2;devices_.pad_analog_mode=3;devices_.pad_analog_sda.reset();
  devices_.callback_phase = 0;
  devices_.ai_initialized = devices_.ai_owned = devices_.audio_callback_active =
      false;
  devices_.aid_callback = devices_.ais_callback = devices_.audio_stack =
      devices_.callback_stack = 0;
  devices_.disable_scheduler = devices_.enable_scheduler = 0;
  devices_.set_interrupt_handler = devices_.register_reset = 0;
  devices_.dvd_fs_init = 0; devices_.dvd_initialized = false;
  devices_.console = nullptr;
  devices_.last_result = {Card::SdkCard::NoCard, Card::SdkCard::NoCard};
}
void HostRuntime::PublishCardState(unsigned slot, int result, bool metadata) {
  auto& d = devices_;
  if (slot >= 2 || !d.card_control) return;
  auto* control = memory_.Resolve(d.card_control + slot * d.card_stride, d.card_stride);
  if (!control) return;
  auto& card = d.cards[slot];
  P32(control, card.Mounted() ? 1 : 0);
  P32(control + 4, result);
  if(d.card_stride==0x110)P32(control + 0x10c, d.disk[slot]?d.disk[slot]:d.card_disk_none);
  if (!card.Mounted()) return;
  P16(control + 8, card.Capacity() / 0x20000);
  P16(control + 10, 128);
  P32(control + 12, Card::SdkCard::Block);
  P16(control + 16, card.Capacity() / Card::SdkCard::Block);
  P32(control + 0x80, d.card_work[slot]);
  const auto dir = card.DirectoryBlock(), fat = card.AllocationBlock();
  P32(control + 0x84, d.card_work[slot] + dir * Card::SdkCard::Block);
  P32(control + 0x88, d.card_work[slot] + fat * Card::SdkCard::Block);
  if (metadata) {
    if (auto* work = memory_.Resolve(d.card_work[slot], 5 * Card::SdkCard::Block)) {
      const auto image = native_exi_host_.Card(slot)->Image();
      // The SDK work area is part of its software ABI (including inlined
      // directory/serial helpers), not an EXI transaction or register bank.
      for (const auto block : {dir, fat})
        std::copy_n(image.data() + block * Card::SdkCard::Block, Card::SdkCard::Block,
                    work + block * Card::SdkCard::Block);
    }
  }
}
bool HostRuntime::BeginDeviceCallback(std::uint32_t callback, std::uint32_t a,
                                      std::uint32_t b, std::uint32_t stack) {
  auto &d = devices_;
  if (!callback)
    return true;
  if (d.callback_phase || !(cpu_.msr & 0x8000u) || !d.disable_scheduler ||
      !d.enable_scheduler)
    return false;
  const auto sp = cpu_.gpr[1];
  const auto frame=DeviceState::CallbackFrameBytes;
  // An interrupt may land where the code keeps r1 only word aligned (Star
  // Fox Adventures: sp=...77c); the frame goes at the next 16-byte boundary
  // below it, linked back to the interrupted sp, as an exception handler's.
  const auto base = sp >= frame + 16u ? (sp - frame) & ~15u : 0u;
  auto* linkage = base && !(sp & 3) ? memory_.Resolve(base, frame) : nullptr;
  auto* audio_linkage = stack >= 8 && !(stack & 7) ? memory_.Resolve(stack - 8, 8) : nullptr;
  if (!linkage || (stack && !audio_linkage)) {
    std::fprintf(stderr,"GEKKOAOT_NATIVE_CALLBACK_UNSUPPORTED reason=stack-mapping callback=%08x sp=%08x stack=%08x\n",callback,sp,stack);
    SetFault(FaultKind::HleFailure, callback);
    return false;
  }
  d.interrupted = cpu_;
  memory_.Read32(0x800000d4u,&d.saved_current_context);
  memory_.Read32(0x800000c0u,&d.saved_physical_context);
  memory_.Read32(0x800000d8u,&d.saved_fpu_context);
  // Match the SDK's asynchronous frame reservation. An ordinary call linkage
  // is insufficient at arbitrary AOT boundaries: a split PPC prologue may
  // already have saved registers below its not-yet-adjusted stack pointer.
  // Only initialise the bridge linkage, not the interrupted spill area.
  P32(linkage, sp); P32(linkage + 4, 0);
  if (stack) { P32(audio_linkage, 0); P32(audio_linkage + 4, 0); }
  cpu_.gpr[1] = base;
  d.callback_stack = stack;
  d.callback = callback;
  d.arg3 = a;
  d.arg4 = b;
  d.callback_phase = 1;
  cpu_.msr &= ~0x8000u;
  cpu_.pc = d.disable_scheduler;
  cpu_.lr = DeviceState::ReturnPc;
  return true;
}
bool HostRuntime::ServiceDeviceEvents() {
  auto &d = devices_;
  if (dsp_.NativeAudioFailed()) {
    SetFault(FaultKind::HleFailure, cpu_.pc,
             static_cast<unsigned>(SimpleHleKind::AiInitDma));
    return false;
  }
  // Complete on this thread, after worker publication, before exposing result
  // or calling game code. The I/O thread never touches CPUState or callbacks.
  if (d.callback_phase) {
    if (cpu_.pc != DeviceState::ReturnPc)
      return false;
    if (d.callback_phase == 1) {
      cpu_.pc = d.callback;
      cpu_.lr = DeviceState::ReturnPc;
      cpu_.gpr[3] = d.arg3;
      cpu_.gpr[4] = d.arg4;
      if (d.callback_stack)
        cpu_.gpr[1] = d.callback_stack - 8;
      d.callback_phase = 2;
      return true;
    }
    if (d.callback_phase == 2) {
      if (d.callback_stack)
        cpu_.gpr[1] = (d.interrupted.gpr[1] - DeviceState::CallbackFrameBytes) & ~15u;
      cpu_.pc = d.enable_scheduler;
      cpu_.lr = DeviceState::ReturnPc;
      d.callback_phase = 3;
      return true;
    }
    if (d.audio_callback_active) {
      d.audio_callback_active = false;
    }
    d.callback_stack = 0;
    const auto time = cpu_.timebase;
    const auto budget = cpu_.cycle_budget;
    const auto downcount = cpu_.downcount;
    const auto reads = cpu_.external_read_count,
               writes = cpu_.external_write_count;
    cpu_ = d.interrupted;
    cpu_.timebase = time;
    cpu_.cycle_budget = budget;
    cpu_.downcount = downcount;
    cpu_.external_read_count = reads;
    cpu_.external_write_count = writes;
    // A callback's temporary OS/FPU context must not escape into the
    // interrupted thread after restoring its physical CPU register bank.
    memory_.Write32(0x800000d4u,d.saved_current_context);
    memory_.Write32(0x800000c0u,d.saved_physical_context);
    memory_.Write32(0x800000d8u,d.saved_fpu_context);
    d.callback_phase = 0;
    GekkoAOT::NativeOS::Service::Get().CompleteNativeCallback(&cpu_);
    return true;
  }
  if (d.ai_owned && (cpu_.msr & 0x8000u)) {
    std::uint32_t global_mask=0,local_mask=0;
    memory_.Read32(0x800000c4u,&global_mask);memory_.Read32(0x800000c8u,&local_mask);
    const auto masked=global_mask|local_mask;
    if (dsp_.AudioCompletionPending() && !(masked&0x04000000u)) {
      if (!d.aid_callback)
        dsp_.AcknowledgeAudioCompletion();
      else if (BeginDeviceCallback(d.aid_callback, 0, 0, d.audio_stack)) {
        // Acknowledge before calling game code, as the SDK handler does. A
        // new completion during that callback remains pending for the next
        // boundary instead of being lost at return.
        dsp_.AcknowledgeAudioCompletion();
        d.audio_callback_active = true;
        return true;
      }
    }
    if (ai_.CounterEventPending() && !(masked&0x00800000u)) {
      const auto count = ai_.SampleCounter();
      ai_.AcknowledgeCounterEvent();
      if (d.ais_callback && BeginDeviceCallback(d.ais_callback, count, 0))
        return true;
    }
  }
  if (ServiceDvdSdkEvents())
    return true;
  for (unsigned slot = 0; slot < 2; ++slot) {
    auto &event = d.completion[slot];
    if (!event.active)
      continue;
    if (event.persist) {
      int result;
      if (!d.cards[slot].Complete(result))
        continue;
      event.result = result;
      event.persist = false;
    }
    if (event.read) {
      auto *info = memory_.Resolve(event.info, 20);
      auto *out = memory_.Resolve(event.buffer, event.length);
      if (event.cancelled)
        event.result = Card::SdkCard::Cancelled;
      else if (!info || !out)
        event.result = Card::SdkCard::Fatal;
      else {
        auto i = Info(info);
        event.result = d.cards[slot].Read(i, {out, event.length}, event.offset);
        if (event.result == 0) {
          PutInfo(info, slot, i);
          native_gx_.NotifyMemoryWrite(event.buffer, event.length);
        }
      }
      event.read = false;
    }
    d.last_result[slot] = event.result;
    PublishCardState(slot, event.result, event.metadata);
    if (!event.callback) {
      event = {};
      continue;
    }
    if (!(cpu_.msr & 0x8000u) || !d.disable_scheduler || !d.enable_scheduler)
      continue;
    if (BeginDeviceCallback(event.callback, slot, event.result)) {
      event = {};
      return true;
    }
  }
  return false;
}

bool HostRuntime::DispatchCardSdk(SimpleHleKind kind,std::uint32_t address) {
  return DispatchCardSdkNative<-1>(kind,address);
}
template<int NativeKind>
bool HostRuntime::DispatchCardSdkNative(SimpleHleKind dynamic_kind,std::uint32_t address) {
  const auto kind=NativeKind<0?dynamic_kind:static_cast<SimpleHleKind>(NativeKind);
  using C = Card::SdkCard;
  auto &d = devices_;
  // Retail libraries inline CARDControl accesses. Without a proven layout,
  // mixing native operations with those helpers can report a healthy RAW as
  // corrupt or return before its read completes. Decline the whole family.
  if (!d.card_control) return false;
  const auto a = cpu_.gpr[3], b = cpu_.gpr[4], c = cpu_.gpr[5], e = cpu_.gpr[6],
             f = cpu_.gpr[7];
  const auto done = [&](int r) {
    if (TraceCards())
      std::fprintf(stderr, "GEKKOAOT_CARD_API name=%s pc=%08x lr=%08x arg=%08x,%08x,%08x,%08x,%08x result=%d\n",
                   CardApiName(kind), address, cpu_.lr, a, b, c, e, f, r);
    cpu_.gpr[3] = static_cast<std::uint32_t>(r);
    cpu_.pc = cpu_.lr;
    return true;
  };
  const auto get_name = [&](std::uint32_t address,
                            std::span<const std::uint8_t> &out) {
    const auto available =
        std::min<std::size_t>(memory_.ContiguousAvailable(address), 33);
    const auto *p = memory_.Resolve(address, available);
    if (!p || !available)
      return false;
    const auto *end =
        static_cast<const std::uint8_t *>(std::memchr(p, 0, available));
    if (!end)
      return false;
    out = {p, static_cast<std::size_t>(end - p)};
    return true;
  };
  const bool file_arg =
      kind == SimpleHleKind::CardRead || kind == SimpleHleKind::CardReadAsync ||
      kind == SimpleHleKind::CardWrite ||
      kind == SimpleHleKind::CardWriteAsync ||
      kind == SimpleHleKind::CardClose || kind == SimpleHleKind::CardCancel;
  auto *info = file_arg ? memory_.Resolve(a, 20) : nullptr;
  if (file_arg && !info)
    return false;
  const auto slot = file_arg ? U32(info) : a;
  if (kind == SimpleHleKind::CardInit) {
    if (d.card_initialized) return done(0);
    d.card_initialized = true;
    if (d.card_control)
      if (auto* control = memory_.Resolve(d.card_control, 2 * d.card_stride))
        std::memset(control, 0, 2 * d.card_stride);
    if(d.card_stride==0x108 && !memory_.Write32(cpu_.gpr[13]+std::int32_t(d.card_disk_sda),0x80000000u))return false;
    for (unsigned n = 0; n < 2; ++n) {
      d.disk[n] = 0x80000000u;
      PublishCardState(n, C::NoCard);
    }
    return done(0);
  }
  if (kind == SimpleHleKind::CardSetGlobalDiskId) {
    if(!a && !d.card_disk_none)return false;
    if(d.card_stride==0x108 && !memory_.Write32(cpu_.gpr[13]+std::int32_t(d.card_disk_sda),a?a:d.card_disk_none))return false;
    for (unsigned n = 0; n < 2; ++n) {
      auto *id = memory_.Resolve(a ? a : 0x80000000u, 6);
      if (!id)
        return false;
      d.disk[n] = a;
      d.cards[n].SetDisk(std::span<const std::uint8_t, 6>(id, 6), a == 0);
      PublishCardState(n, d.last_result[n]);
    }
    return done(0);
  }
  if (slot >= 2)
    return done(C::Fatal);
  auto &card = d.cards[slot];
  auto &event = d.completion[slot];
  if (kind == SimpleHleKind::CardGetResult)
    return done(event.active ? C::Busy : d.last_result[slot]);
  if (kind == SimpleHleKind::CardCancel) {
    if (!card.Mounted())
      return done(C::NoCard);
    if (event.active && event.info == a) {
      event.cancelled = true;
      P32(info + 12, 0xffffffffu);
      card.Cancel();
      return done(C::Cancelled);
    }
    return done(0);
  }
  if (kind == SimpleHleKind::CardGetDiskId)
    return done(d.disk[slot]);
  if (kind == SimpleHleKind::CardProbe || kind == SimpleHleKind::CardProbeEx) {
    const auto *image = native_exi_host_.Card(slot);
    const bool ready = image && image->Ready();
    if (kind == SimpleHleKind::CardProbe)
      return done(ready);
    auto *mem = b ? memory_.Resolve(b, 4) : nullptr;
    auto *sector = c ? memory_.Resolve(c, 4) : nullptr;
    if ((b && !mem) || (c && !sector))
      return false;
    if (!ready)
      return done(C::NoCard);
    if (mem)
      P32(mem, image->Image().size() / 0x20000);
    if (sector)
      P32(sector, C::Block);
    return done(0);
  }
  if (event.active)
    return done(C::Busy);
  if (const auto result = card.Synchronize(); result < 0 && result != C::NoCard)
    return done(result);
  if (kind == SimpleHleKind::CardGetSerial || kind == SimpleHleKind::CardGetUnique) {
    auto* out = memory_.Resolve(b, 8);
    if (!out) return false;
    if (!card.Mounted()) return done(C::NoCard);
    if (kind == SimpleHleKind::CardGetSerial) {
      const auto image = native_exi_host_.Card(slot)->Image();
      // The SDK serial is the XOR of the four 64-bit words in CARDID.serial.
      // Preserve byte order without an unaligned host uint64 load.
      for (unsigned i = 0; i < 8; ++i)
        out[i] = image[i] ^ image[8+i] ^ image[16+i] ^ image[24+i];
    } else {
      const auto sram = native_exi_host_.Sram();
      std::copy_n(sram.data() + 20 + slot * 12 + 4, 8, out);
    }
    return done(0);
  }
  if (kind == SimpleHleKind::CardSetDiskId) {
    auto *id = memory_.Resolve(b ? b : 0x80000000u, 6);
    if (!id)
      return false;
    d.disk[slot] = b ? b : 0x80000000u;
    card.SetDisk(std::span<const std::uint8_t, 6>(id, 6));
    PublishCardState(slot, d.last_result[slot]);
    return done(0);
  }
  const bool async = kind == SimpleHleKind::CardMountAsync ||
                     kind == SimpleHleKind::CardReadAsync ||
                     kind == SimpleHleKind::CardWriteAsync ||
                     kind == SimpleHleKind::CardCreateAsync ||
                     kind == SimpleHleKind::CardDeleteAsync ||
                     kind == SimpleHleKind::CardFastDeleteAsync ||
                     kind == SimpleHleKind::CardSetStatusAsync ||
                     kind == SimpleHleKind::CardCheckAsync ||
                     kind == SimpleHleKind::CardCheckExAsync ||
                     kind == SimpleHleKind::CardFormatAsync;
  std::uint32_t callback = 0;
  switch (kind) {
  case SimpleHleKind::CardMountAsync:
    callback = e;
    break;
  case SimpleHleKind::CardReadAsync:
  case SimpleHleKind::CardWriteAsync:
    callback = f;
    break;
  case SimpleHleKind::CardCreateAsync:
    callback = f;
    break;
  case SimpleHleKind::CardDeleteAsync:
  case SimpleHleKind::CardFastDeleteAsync:
    callback = c;
    break;
  case SimpleHleKind::CardCheckExAsync:
    callback = c;
    break;
  case SimpleHleKind::CardSetStatusAsync:
    callback = e;
    break;
  case SimpleHleKind::CardFormatAsync:
  case SimpleHleKind::CardCheckAsync:
    callback = b;
    break;
  default:
    break;
  }
  // A missing scheduler ABI proof is an unsupported translation, never a
  // callback dropped on the floor. The normal mode may run the original AOT
  // body; strict mode reports this exact SDK entry as unsupported.
  if (callback && (!d.disable_scheduler || !d.enable_scheduler))
    return false;
  if(kind==SimpleHleKind::CardCheckExAsync) {
    auto* repaired=b?memory_.Resolve(b,4):nullptr;
    if(b&&!repaired)return false;
    // A valid card requires no repair. Recovery of a damaged redundant copy
    // is not implemented here: decline explicitly rather than claim a repair.
    if(card.Check()==0 && !card.MetadataCopiesValid())return false;
    if(repaired)P32(repaired,0);
  }
  int result = C::Fatal;
  if (kind == SimpleHleKind::CardMount ||
      kind == SimpleHleKind::CardMountAsync) {
    auto *work = memory_.Resolve(b, 5 * C::Block);
    auto *id = memory_.Resolve(d.disk[slot] ? d.disk[slot] : 0x80000000u, 6);
    if (!work || !id)
      return false;
    card.SetDisk(std::span<const std::uint8_t, 6>(id, 6), d.disk[slot] == 0);
    result = card.Mount(native_exi_host_.Card(slot));
    if (result == 0) {
      std::copy_n(native_exi_host_.Card(slot)->Image().data(), 5 * C::Block,
                  work);
      d.detach[slot] = c;
      d.card_work[slot] = b;
    }
  } else if (kind == SimpleHleKind::CardFormat ||
             kind == SimpleHleKind::CardFormatAsync) {
    if (!card.Mounted())
      card.Mount(native_exi_host_.Card(slot));
    result = card.Format(card.Encode());
  } else if (kind == SimpleHleKind::CardUnmount)
    result = card.Unmount();
  else if (kind == SimpleHleKind::CardOpen ||
           kind == SimpleHleKind::CardFastOpen) {
    auto *out = memory_.Resolve(c, 20);
    if (!out)
      return false;
    Card::SdkCard::Info i;
    if (kind == SimpleHleKind::CardOpen) {
      std::span<const std::uint8_t> name;
      if (!get_name(b, name))
        return false;
      result = card.Open(name, i);
    } else
      result = card.FastOpen(b, i);
    P32(out, 0xffffffffu);
    if (result == 0)
      PutInfo(out, slot, i);
  } else if (kind == SimpleHleKind::CardClose) {
    result = card.Mounted() ? 0 : C::NoCard;
    if (result == 0)
      P32(info, 0xffffffffu);
  } else if (kind == SimpleHleKind::CardRead ||
             kind == SimpleHleKind::CardReadAsync ||
             kind == SimpleHleKind::CardWrite ||
             kind == SimpleHleKind::CardWriteAsync) {
    auto *buffer = memory_.Resolve(b, c);
    if (!buffer)
      return false;
    auto i = Info(info);
    if (async && (kind == SimpleHleKind::CardReadAsync)) {
      // Publish the SDK seek/progress ABI now, but not payload bytes. Use the
      // cached chain directly rather than converting an entire CARDStat just
      // to recover the file length.
      result = card.PrepareRead(i,c,e);
      if (result == 0) {
        PutInfo(info,slot,i);
        event.active = true;
        event.read = true;
        event.callback = callback;
        event.info = a;
        event.buffer = b;
        event.length = c;
        event.offset = e;
        PublishCardState(slot, C::Busy);
        return done(0);
      }
    } else {
      result = (kind == SimpleHleKind::CardWrite ||
                kind == SimpleHleKind::CardWriteAsync)
                   ? card.Write(i, {buffer, c}, e)
                   : card.Read(i, {buffer, c}, e);
      if (result == 0) {
        PutInfo(info, slot, i);
        if (kind == SimpleHleKind::CardWrite ||
            kind == SimpleHleKind::CardWriteAsync)
          card.SetTimestamp(
              i.file, static_cast<std::uint32_t>(cpu_.timebase / 40500000u));
        if (kind == SimpleHleKind::CardRead)
          native_gx_.NotifyMemoryWrite(b, c);
      }
    }
  } else if (kind == SimpleHleKind::CardCreate ||
             kind == SimpleHleKind::CardCreateAsync) {
    std::span<const std::uint8_t> name;
    auto *out = memory_.Resolve(e, 20);
    if (!out || !get_name(b, name))
      return false;
    Card::SdkCard::Info i;
    result = card.Create(
        name, c, static_cast<std::uint32_t>(cpu_.timebase / 40500000u), i);
    if (result == 0)
      PutInfo(out, slot, i);
  } else if (kind == SimpleHleKind::CardDelete ||
             kind == SimpleHleKind::CardDeleteAsync ||
             kind == SimpleHleKind::CardFastDelete ||
             kind == SimpleHleKind::CardFastDeleteAsync) {
    int file = b;
    if (kind == SimpleHleKind::CardDelete ||
        kind == SimpleHleKind::CardDeleteAsync) {
      std::span<const std::uint8_t> name;
      if (!get_name(b, name))
        return false;
      Card::SdkCard::Info i;
      result = card.Open(name, i);
      if (result < 0)
        return done(result);
      file = i.file;
    }
    result = card.Delete(file);
  } else if (kind == SimpleHleKind::CardGetStatus ||
             kind == SimpleHleKind::CardSetStatus ||
             kind == SimpleHleKind::CardSetStatusAsync) {
    auto *status = memory_.Resolve(c, 108);
    if (!status)
      return false;
    if (kind == SimpleHleKind::CardGetStatus)
      result = card.GetStatus(b, std::span<std::uint8_t, 108>(status, 108));
    else {
      P32(status + 36, static_cast<std::uint32_t>(cpu_.timebase / 40500000u));
      result =
          card.SetStatus(b, std::span<const std::uint8_t, 108>(status, 108));
      if (result == 0)
        card.GetStatus(b, std::span<std::uint8_t, 108>(status, 108));
    }
  } else if (kind == SimpleHleKind::CardFreeBlocks) {
    auto *bytes = b ? memory_.Resolve(b, 4) : nullptr;
    auto *files = c ? memory_.Resolve(c, 4) : nullptr;
    if ((b && !bytes) || (c && !files))
      return false;
    result = card.FreeBytes();
    if (result >= 0) {
      if (bytes)
        P32(bytes, result);
      if (files)
        P32(files, card.FreeFiles());
      result = 0;
    }
  } else if (kind == SimpleHleKind::CardCheck ||
             kind == SimpleHleKind::CardCheckAsync ||
             kind == SimpleHleKind::CardCheckExAsync)
    result = card.Check();
  else if (kind == SimpleHleKind::CardGetMemSize ||
           kind == SimpleHleKind::CardGetSectorSize ||
           kind == SimpleHleKind::CardGetEncoding) {
    auto *out =
        memory_.Resolve(b, kind == SimpleHleKind::CardGetSectorSize ? 4 : 2);
    if (!out)
      return false;
    result = card.Mounted() ? 0 : C::NoCard;
    if (result == 0) {
      if (kind == SimpleHleKind::CardGetSectorSize)
        P32(out, C::Block);
      else
        P16(out, kind == SimpleHleKind::CardGetEncoding
                     ? card.Encode()
                     : card.Capacity() / 0x20000);
    }
  } else
    return false;
  if (result == 0 && card.Mutated()) {
    if (!card.Submit())
      return done(C::Io);
    if (async) {
      event.active = true;
      event.persist = true;
      event.callback = callback;
      event.info = file_arg ? a : 0;
      event.metadata = true;
      PublishCardState(slot, C::Busy);
      return done(0);
    }
    result = card.Wait();
    PublishCardState(slot, result, true);
  }
  d.last_result[slot] = result;
  if (async && result == 0) {
    event.active = true;
    event.callback = callback;
    event.result = result;
  }
  PublishCardState(slot, event.active ? C::Busy : result);
  return done(result);
}
// Generate all specializations from the shared registry. Dead cases are
// removed inside each specialization, without relying on cross-DSO inlining.
#define SDK_SIMPLE(kind, name, abi) template bool HostRuntime::DispatchCardSdkNative<static_cast<int>(SimpleHleKind::kind)>(SimpleHleKind,std::uint32_t);
#define SDK_OS(kind, name, abi)
#include "sdk/native_services.def"
#undef SDK_SIMPLE
#undef SDK_OS
} // namespace GekkoAOT::Native
