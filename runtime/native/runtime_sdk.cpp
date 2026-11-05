// SPDX-License-Identifier: GPL-3.0-or-later
// HostRuntime: native SDK services, HLE dispatch and the service table.
#include "native/runtime_internal.h"

namespace GekkoAOT::Native {

void HostRuntime::RegisterNativeHookCodeProof(std::uint32_t address) {
  native_hook_code_proofs_.erase(address);
  native_hook_proof_last_ = nullptr; native_hook_proof_last_address_ = 0u;
  const auto* descriptor = module_.Descriptor();
  std::uint32_t chunk = 0u;
  if (!descriptor || !descriptor->chunk_hashes ||
      !FindModuleChunk(descriptor, address, &chunk) ||
      descriptor->chunk_hashes[chunk] == 0u) return;
  const auto range = descriptor->chunk_ranges[chunk];
  native_hook_code_proofs_[address] = {range.start, range.end,
                                     descriptor->chunk_hashes[chunk], 0u, false};
}

bool HostRuntime::NativeHookCodeMatches(std::uint32_t address) {
  if (!has_fixed_secondary_modules_) return true;
  NativeHookCodeProof* cached = address == native_hook_proof_last_address_ ? native_hook_proof_last_ : nullptr;
  if (!cached) {
    const auto found = native_hook_code_proofs_.find(address);
    if (found == native_hook_code_proofs_.end()) return true;
    cached = &found->second;
    native_hook_proof_last_address_ = address;
    native_hook_proof_last_ = cached;
  }
  auto& proof = *cached;
  if (proof.epoch != fixed_exec_route_epoch_) {
    const auto size = static_cast<std::size_t>(proof.end - proof.start);
    const auto* bytes = memory_.Resolve(proof.start, size);
    proof.active = bytes && Fnv1a64(bytes, size) == proof.hash;
    proof.epoch = fixed_exec_route_epoch_;
    static unsigned stale_hook_logs = 0u;
    if (!proof.active && stale_hook_logs++ < 16u)
      std::fprintf(stderr,
                   "GEKKOAOT_OVERLAY_HLE_CODE_PROOF=1 address=%08x active=0 reason=bootstrap-code-replaced\n",
                   address);
  }
  return proof.active;
}

void HostRuntime::RebuildNativeInterceptIndex() {
  native_region_probe_cache_.fill({});
  native_intercepts_.clear();
  native_intercepts_.reserve(os_hooks_.size() + simple_hle_hooks_.size());
  for (const auto& [address, _] : os_hooks_) native_intercepts_.push_back(address);
  for (const auto& [address, _] : simple_hle_hooks_) native_intercepts_.push_back(address);
  std::sort(native_intercepts_.begin(), native_intercepts_.end());
  native_intercepts_.erase(std::unique(native_intercepts_.begin(), native_intercepts_.end()),
                           native_intercepts_.end());
}

bool HostRuntime::DispatchOsHle(GekkoAOT::NativeOS::Kind kind,std::uint32_t address) {
  return DispatchOsHleNative<-1>(kind,address);
}
template<int NativeKind>
bool HostRuntime::DispatchOsHleNative(GekkoAOT::NativeOS::Kind dynamic_kind,std::uint32_t address) {
  const auto kind=NativeKind<0?dynamic_kind:static_cast<GekkoAOT::NativeOS::Kind>(NativeKind);
  Capture::Scope capture(Capture::Kind::NativeOs, address);
  ++hle_calls_;
  ++os_hle_kind_calls_[static_cast<std::size_t>(kind) & 63u];
  static const bool trace_os_hle = RuntimeEnvBool("GEKKOAOT_TRACE_OS_HLE", false);
  if (trace_os_hle && (hle_calls_ <= 64u || (hle_calls_ & (hle_calls_ - 1u)) == 0u))
    std::fprintf(stderr, "GEKKOAOT_OS_HLE_ENTRY calls=%llu kind=%u address=%08x pc=%08x lr=%08x msr=%08x\n",
                 static_cast<unsigned long long>(hle_calls_), static_cast<unsigned>(kind),
                 address, cpu_.pc, cpu_.lr, cpu_.msr);

  // GEKKOAOT_NATIVE_OS_INTERRUPT_ENTRY_FENCE_V149:
  // A statically lowered NativeOS leaf can be entered from inside a compiled
  // native call chain without first returning to HostRuntime::Run(), so the
  // normal dispatch-boundary asynchronous-interrupt probe is skipped.  That is
  // observable for the three MSR[EE] helpers: OSDisableInterrupts in particular
  // may clear EE before an already-pending external/DEC interrupt gets the
  // architectural chance it would have had at the guest function-entry
  // boundary.
  //
  // The direct-HLE state-sync path has already published PC as the intercepted
  // guest entrypoint.  If an interrupt is pending, take it now and report the
  // host call handled without executing the leaf.  SRR0 therefore points back
  // at the SDK entry and RFI naturally retries it, matching the guest path and
  // preserving the OSDisableInterrupts RAS semantics.  Apply the same fence to
  // Enable/Restore because callers may enter either while EE is already set.
  if (kind == GekkoAOT::NativeOS::Kind::DisableInterrupts ||
      kind == GekkoAOT::NativeOS::Kind::EnableInterrupts ||
      kind == GekkoAOT::NativeOS::Kind::RestoreInterrupts) {
    if (TryTakeExternalInterrupt() || TryTakeDecrementerInterrupt())
      return true;
  }

  // Diagnostic: unbounded trace of OSThread lifecycle services (rare calls).
  static const bool trace_threads = RuntimeEnvBool("GEKKOAOT_TRACE_OS_THREADS", false);
  if (trace_threads) {
    using K = GekkoAOT::NativeOS::Kind;
    switch (kind) {
    case K::CreateThread: case K::ExitThread: case K::CancelThread: case K::JoinThread:
    case K::DetachThread: case K::ResumeThread: case K::SuspendThread: case K::LoadContext: {
      std::uint32_t current = 0;
      memory_.Read32(0x800000e4u, &current);
      std::fprintf(stderr, "GEKKOAOT_OS_THREAD_TRACE kind=%s r3=%08x r4=%08x r5=%08x r6=%08x r7=%08x r8=%08x lr=%08x current=%08x tb=%llx\n",
                   GekkoAOT::NativeOS::Name(kind), cpu_.gpr[3], cpu_.gpr[4], cpu_.gpr[5], cpu_.gpr[6],
                   cpu_.gpr[7], cpu_.gpr[8], cpu_.lr, current, static_cast<unsigned long long>(cpu_.timebase));
      break;
    }
    default: break;
    }
  }
  auto& native_os = GekkoAOT::NativeOS::Service::Get();
  if (GekkoAOT::NativeOS::Service::IsInlineFastKind(kind)) {
    if (native_os.DispatchInlineFast(kind, &cpu_)) return true;
  } else if (kind == GekkoAOT::NativeOS::Kind::YieldThread && yield_spin_enabled_) {
    // A yield with no ready peer of equal or higher priority returns to
    // its caller on the same thread; count it for ParkYieldSpin.
    const auto lr = cpu_.lr;
    std::uint32_t before = 0, after = 0;
    const bool known = memory_.Read32(0x800000e4u, &before);
    if (native_os.DispatchNative<NativeKind>(kind, &cpu_)) {
      if (known && cpu_.pc == lr && cpu_.exception == 0u &&
          memory_.Read32(0x800000e4u, &after) && after == before) {
        // A yield is a scheduling point: deliver an interrupt that became
        // pending while this AOT chain spun (SRR0 = the yield's return
        // address), instead of letting the loop run until the chain ends.
        if ((cpu_.msr & kPpcMsrEe) != 0u && (pi_.InterruptPending() || decrementer_pending_)) {
          yield_noop_count_ = 0u;
          if (!TryTakeExternalInterrupt()) TryTakeDecrementerInterrupt();
          return true;
        }
        const auto now = std::chrono::steady_clock::now();
        if (lr != yield_noop_lr_ || now - yield_spin_start_ > std::chrono::microseconds(64)) {
          yield_noop_lr_ = lr; yield_noop_count_ = 0u; yield_spin_start_ = now;
        }
        if (++yield_noop_count_ >= 64u) {
          yield_noop_count_ = 0u;
          ParkYieldSpin();
          yield_spin_start_ = std::chrono::steady_clock::now();
        }
      }
      return true;
    }
  } else if (native_os.DispatchNative<NativeKind>(kind, &cpu_)) {
    return true;
  }

  // OSInitThreadQueue is two plain stores, and byte-identical game
  // leaves exist (HPPOA initializes its own lists in paged VM through such a
  // leaf). The native service rejects a pointer it cannot resolve before
  // modifying anything, so run the original compiled body for exactly this
  // call (one-shot region-probe admission) instead of failing the game.
  if (kind == GekkoAOT::NativeOS::Kind::InitThreadQueue && cpu_.exception == 0u &&
      cpu_.pc == address) {
    hle_decline_entry_ = address;
    ++hle_guest_declines_;
    return false;
  }

  // Native mode is fail-closed: there is no guest/Dolphin scheduler fallback.
  // Blocking with an empty run queue is handled by NativeOS::EnterIdle(); a
  // false return now means a genuine native scheduler/ABI failure.
  std::fprintf(stderr,
               "GEKKOAOT_OS_HLE_FAILURE kind=%u entry=%08x pc=%08x lr=%08x "
               "r3=%08x r4=%08x r5=%08x r6=%08x r7=%08x r8=%08x r9=%08x\n",
               static_cast<unsigned>(kind), address, cpu_.pc, cpu_.lr,
               cpu_.gpr[3], cpu_.gpr[4], cpu_.gpr[5], cpu_.gpr[6],
               cpu_.gpr[7], cpu_.gpr[8], cpu_.gpr[9]);
  SetFault(FaultKind::HleFailure, address);
  return false;
}

HostRuntime::GuestRangeStatus HostRuntime::TranslateGuestRange(
    std::uint32_t address, std::uint32_t size, bool write, std::vector<GuestSpan>& spans) {
  spans.clear();
  if (size == 0u) return GuestRangeStatus::Ok;
  if (std::uint64_t(address) + size > 0x100000000ull) return GuestRangeStatus::Invalid;
  std::uint32_t done = 0u;
  while (done < size) {
    const std::uint32_t ea = address + done;
    const std::uint32_t remaining = size - done;
    if (memory_.PagedVmem() && AddressSpace::IsVmem(ea)) {
      const auto translation = TranslateVmemAddress(ea, write);
      // Report, never raise -- the caller declines to the guest code,
      // which takes the architectural DSI itself at the exact access.
      if (translation.status != PageTranslation::Status::Mapped)
        return GuestRangeStatus::NotResident;
      const std::uint32_t bytes = std::min<std::uint32_t>(0x1000u - (ea & 0xfffu), remaining);
      if (translation.physical >= memory_.Mem1().size() ||
          bytes > memory_.Mem1().size() - translation.physical)
        return GuestRangeStatus::Invalid;
      spans.push_back({memory_.Mem1().data() + translation.physical, bytes});
      done += bytes;
      continue;
    }
    // The locked-cache scratchpad is plain memory for libc services
    // (Spyro: A Hero's Tail memsets 0xE0001000); it has its own backing.
    if (ea >= HW::LC::NativeLC::Base && ea - HW::LC::NativeLC::Base < HW::LC::NativeLC::Size) {
      const std::uint32_t bytes = std::min<std::uint32_t>(
          remaining, HW::LC::NativeLC::Size - (ea - HW::LC::NativeLC::Base));
      auto* host = static_cast<std::uint8_t*>(lc_.Pointer(ea, bytes));
      if (!host) return GuestRangeStatus::Invalid;
      spans.push_back({host, bytes});
      done += bytes;
      continue;
    }
    const std::size_t available = memory_.ContiguousAvailable(ea);
    if (available == 0u) return GuestRangeStatus::Invalid;
    const auto bytes = static_cast<std::uint32_t>(std::min<std::size_t>(remaining, available));
    auto* host = memory_.Resolve(ea, bytes);
    if (!host) return GuestRangeStatus::Invalid;
    spans.push_back({host, bytes});
    done += bytes;
  }
  return GuestRangeStatus::Ok;
}

#if defined(_MSC_VER)
__declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
__attribute__((noinline,cold))
#endif
bool HostRuntime::RefreshGxPixelProof(unsigned family,std::uint32_t address,const std::uint8_t* code,std::uint32_t bytes) {
  const auto sda=GekkoAOTSdk::ProveGxPixelLeaf({code,bytes},GekkoAOTSdk::GxPixelFamily(family));
  if(!sda)return false;
  auto& proof=gx_pixel_proofs_[family];proof.address=address;proof.bytes=bytes;proof.sda=*sda;
  std::copy_n(code,bytes,proof.code.begin());return true;
}
bool HostRuntime::DispatchGxPixelHle(SimpleHleKind kind,std::uint32_t address) {
  return DispatchGxPixelHleNative<-1>(kind,address);
}
template<int NativeKind>
bool HostRuntime::DispatchGxPixelHleNative(SimpleHleKind dynamic_kind,std::uint32_t address) {
    const auto kind=NativeKind<0?dynamic_kind:static_cast<SimpleHleKind>(NativeKind);
    const auto decline=[&](){hle_decline_entry_=address;++hle_guest_declines_;return false;};
    const auto family=GekkoAOTSdk::GxPixelFamily(unsigned(kind)-unsigned(SimpleHleKind::GxSetAlphaCompare));
    const auto bytes=GekkoAOTSdk::GxPixelProof(family).masks.size()*4;
    const auto* code=memory_.Resolve(address,bytes);
    if(!code || !native_sdk_gx_packets_enabled_ || !native_gx_.Ready())return decline();
    auto& proof=gx_pixel_proofs_[unsigned(family)];
    // Compare every resident byte on every call. Overlay changes and raw RAM
    // mutations cannot hide behind an epoch or a previously admitted symbol.
    if(proof.address!=address || proof.bytes!=bytes || std::memcmp(proof.code.data(),code,bytes)!=0) {
      if(!RefreshGxPixelProof(unsigned(family),address,code,bytes))return decline();
    }
    std::uint32_t context=0;
    if(!memory_.Read32(cpu_.gpr[13]+std::int32_t(proof.sda),&context))return decline();
    const unsigned footprint=family==GekkoAOTSdk::GxPixelFamily::AlphaCompare?4u:
      family==GekkoAOTSdk::GxPixelFamily::ZMode?0x1dcu:0x1d4u;
    auto* data=memory_.Resolve(context,footprint);
    // GXData must be plain, contiguous guest RAM and cannot alias the SDA
    // pointer cell. These leaves neither translate paged memory nor perform IO.
    const auto physical=context&0x3fffffffu,cell=(cpu_.gpr[13]+std::int32_t(proof.sda))&0x3fffffffu;
    if(!data || physical>=memory_.Mem1().size() || footprint>memory_.Mem1().size()-physical ||
       data!=memory_.Mem1().data()+physical ||
       (cell>=physical && cell<physical+footprint))return decline();
    const bool direct=CpuFifoTargetsGpu() &&
      ((!pi_.FifoBase() && !pi_.FifoEnd()) || GekkoAOT::NativeHW::CP::CanDirectSubmit(
        native_hw_.CP(),native_gx_.Ready(),cp_.GPLinkEnabled(),cp_.GPReadEnabled(),
        cp_.BreakpointActiveForHost() || cp_.RegisterDrainPending(),cp_.FifoReadWriteDistance(),
        cp_.FifoReadPointer()==cp_.FifoWritePointer()));
    const unsigned offset=family==GekkoAOTSdk::GxPixelFamily::ZMode?0x1d8:0x1d0;
    const auto cached=family==GekkoAOTSdk::GxPixelFamily::AlphaCompare?0u:GekkoAOTSdk::DeviceWord(data+offset);
    const auto result=GekkoAOTSdk::ComputeGxPixelLeaf(family,cpu_.gpr,cached);
    const auto word=result.word;
    GX::NativePixelState pixel;
    const bool typed=direct && native_gx_.HasDirectPixelCommands() && GX::LowerNativeBp(word>>24,word,pixel);
    if(TryTakeFifoWriteInterrupt(true))return true;
    struct AtomicService {bool& active;bool previous;explicit AtomicService(bool& f):active(f),previous(f){active=true;}~AtomicService(){active=previous;}} atomic(atomic_fifo_service_);
    // CPU recording FIFOs must retain their exact bytes for later submission
    // and guest reads. Execute the proved SDK semantics natively on that path
    // too, using the same ordered packet transport as native matrix services.
    // Only attach a typed renderer hint to an immediately consumable GPU FIFO.
    if(typed && !native_gx_.StagePixel(word>>24,word&0xffffffu,cpu_fifo_gather_size_+gpu_fifo_gather_size_)){
      SetFault(FaultKind::MmioWrite,0xcc008000u,5);return false;
    }
    if(family!=GekkoAOTSdk::GxPixelFamily::AlphaCompare){
      data[offset]=word>>24;data[offset+1]=word>>16;data[offset+2]=word>>8;data[offset+3]=word;
    }
    const std::array<std::uint8_t,5> packet{0x61,std::uint8_t(word>>24),std::uint8_t(word>>16),std::uint8_t(word>>8),std::uint8_t(word)};
    if(!QueueSdkFifoPacket(packet.data(),packet.size()))return false;
    data[2]=data[3]=0;
    GekkoAOTSdk::CommitGxPixelRegisters(family,cpu_.gpr,context,result);
    cpu_.pc=cpu_.lr;
    ++hle_calls_;++simple_hle_kind_calls_[unsigned(kind)];return true;
}

bool HostRuntime::DispatchSimpleHle(SimpleHleKind kind,std::uint32_t address) {
  return DispatchSimpleHleNative<-1>(kind,address);
}
template<int NativeKind>
bool HostRuntime::DispatchSimpleHleNative(SimpleHleKind dynamic_kind,std::uint32_t address) {
  const auto kind=NativeKind<0?dynamic_kind:static_cast<SimpleHleKind>(NativeKind);
  Capture::Scope capture(Capture::Kind::Sdk, address);
  if (DisabledSimpleServices().test(static_cast<unsigned>(kind))) {
    if(native_only_ && (kind>=SimpleHleKind::OsGetRtc || kind==SimpleHleKind::PadRead || kind==SimpleHleKind::DvdFastOpen)) { SetFault(FaultKind::HleFailure,address,static_cast<unsigned>(kind)); return false; }
    hle_decline_entry_ = address;  // admit the guest body once (see probe)
    ++hle_guest_declines_;
    return false;
  }
  if(kind>=SimpleHleKind::GxSetAlphaCompare && kind<=SimpleHleKind::GxSetAlphaUpdate) {
    if constexpr(NativeKind>=int(SimpleHleKind::GxSetAlphaCompare) && NativeKind<=int(SimpleHleKind::GxSetAlphaUpdate))
      return DispatchGxPixelHleNative<NativeKind>(kind,address);
    else return DispatchGxPixelHle(kind,address);
  }
  if(kind==SimpleHleKind::TangentCameraFrustum && GekkoAOTSdk::ProveJ3dFrustum(address,[&](auto a,auto n){return memory_.Resolve(a,n);})) {
    const auto decline=[&](){hle_decline_entry_=address;++hle_guest_declines_;return false;};
    const auto read=[&](auto pc,auto n){return memory_.Resolve(pc,n);};
    const bool wide=native_gx_.WidescreenActive();
    if((!wide && live_graphics_settings_.fov_y==0) ||
       !GekkoAOTSdk::ProveJ3dConstants(address,cpu_.gpr[2],read))return decline();
    auto* camera=memory_.Resolve(cpu_.gpr[3],0x5c);
    std::array<std::array<float,8>,32> cameras{};
    const auto count=native_gx_.RecentPerspectiveProjections(cameras);
    for(unsigned i=0;i<count;++i){
      std::array<float,6> projection{};std::copy_n(cameras[i].begin(),6,projection.begin());
      const float target=wide?native_gx_.WidescreenTargetAspect():cameras[i][7];
      if(!GX::AdjustJ3dFrustum(camera,projection,cameras[i][7],target,live_graphics_settings_.fov_y))continue;
      native_gx_.NotifyMemoryWrite(cpu_.gpr[3]+4,48);
      static std::uint64_t updates=0;
      if(++updates<=8 || updates%3600==0)std::fprintf(stderr,
        "GEKKOAOT_CAMERA_FRUSTUM provider=j3d address=%08x aspect=%.6f->%.6f updates=%llu culling=guest lens=unchanged\n",
        cpu_.gpr[3],cameras[i][7],target,(unsigned long long)updates);
      cpu_.pc=cpu_.lr;++hle_calls_;++simple_hle_kind_calls_[unsigned(kind)];return true;
    }
    static const bool diagnostics=std::getenv("GEKKOAOT_CAMERA_DIAGNOSTICS")!=nullptr;
    static unsigned rejected=0;
    if(diagnostics && camera && count && rejected++<12)
      std::fprintf(stderr,"GEKKOAOT_CAMERA_REJECT provider=j3d address=%08x fov=%.6f aspect=%.6f near=%.6f far=%.6f observed=%.6f,%.6f,%.6f,%.6f,%.6f,%.6f\n",
        cpu_.gpr[3],GX::ReadBigEndianFloat(camera+0x4c),GX::ReadBigEndianFloat(camera+0x50),
        GX::ReadBigEndianFloat(camera+0x54),GX::ReadBigEndianFloat(camera+0x58),
        cameras[0][0],cameras[0][1],cameras[0][2],cameras[0][3],cameras[0][4],cameras[0][5]);
    return decline();
  }
  if(kind==SimpleHleKind::TangentCameraFrustum) {
    const auto decline=[&](){hle_decline_entry_=address;++hle_guest_declines_;return false;};
    const bool update=GekkoAOTSdk::ProveTangentCameraUpdate(address,[&](auto pc,auto bytes){return memory_.Resolve(pc,bytes);});
    if(!update && !GekkoAOTSdk::ProveTangentFrustum(address,[&](auto pc,auto bytes){return memory_.Resolve(pc,bytes);}))return decline();
    if(tangent_camera_epoch_!=fixed_exec_route_epoch_){tangent_cameras_.Reset();tangent_camera_epoch_=fixed_exec_route_epoch_;}
    const auto camera_address=cpu_.gpr[update?3:4];
    auto* camera=memory_.Resolve(camera_address,0x144);
    if(!update && !GekkoAOTSdk::ProveTangentCameraObject(camera,[&](auto pc,auto bytes){return memory_.Resolve(pc,bytes);}))return decline();
    std::array<std::array<float,8>,32> cameras{};
    if(!camera || camera[0x140]!=1)return decline();
    const auto camera_count=native_gx_.RecentPerspectiveProjections(cameras);
    if(camera_count==0)return decline();
    const float before_h=GX::ReadBigEndianFloat(camera+0x2c),before_v=GX::ReadBigEndianFloat(camera+0x30);
    bool adjusted=false;std::array<float,6> projection{};float aspect=0;
    for(unsigned i=0;i<camera_count;++i){
      std::copy_n(cameras[i].begin(),6,projection.begin());aspect=cameras[i][7];
      if(tangent_cameras_.Apply(camera_address,camera,projection,aspect,
        live_graphics_settings_.fov_y,native_gx_.WidescreenActive()?native_gx_.WidescreenTargetAspect():0.f)){adjusted=true;break;}
    }
    if(adjusted && (GX::ReadBigEndianFloat(camera+0x2c)!=before_h || GX::ReadBigEndianFloat(camera+0x30)!=before_v)){
      native_gx_.NotifyMemoryWrite(camera_address+0x2c,8);
      native_gx_.NotifyMemoryWrite(camera_address+0x140,4);
      static std::uint64_t updates=0;
      if(++updates<=8 || updates%3600==0)std::fprintf(stderr,"GEKKOAOT_CAMERA_FRUSTUM provider=tangent-%s address=%08x fov=%.3f aspect=%.6f updates=%llu culling=guest\n",update?"update":"consumer",camera_address,live_graphics_settings_.fov_y,aspect,(unsigned long long)updates);
    }
    static const bool diagnostics=std::getenv("GEKKOAOT_CAMERA_DIAGNOSTICS")!=nullptr;
    static unsigned rejected=0;
    if(!adjusted && diagnostics && rejected++<12)
      std::fprintf(stderr,"GEKKOAOT_CAMERA_REJECT provider=tangent lens=%.6f,%.6f near=%.6f observed=%.6f,%.6f,%.6f,%.6f,%.6f,%.6f aspect=%.6f\n",before_h,before_v,GX::ReadBigEndianFloat(camera+0x24),projection[0],projection[1],projection[2],projection[3],projection[4],projection[5],aspect);
    // Admit the original native AOT body once: it builds every original plane,
    // including the near/far planes, from the adjusted shared camera inputs.
    return decline();
  }
  if(kind==SimpleHleKind::AngularCameraSetFov) {
    const auto decline=[&](){hle_decline_entry_=address;++hle_guest_declines_;return false;};
    const bool wide=native_gx_.WidescreenActive();
    const float override=live_graphics_settings_.fov_y;
    if(!wide && override==0)return decline();
    float focal=0;
    if(!GekkoAOTSdk::ProveAngularCameraSetter(address,[&](auto pc,auto bytes){return memory_.Resolve(pc,bytes);},&focal))return decline();
    auto* camera=memory_.Resolve(cpu_.gpr[3],0xf0);
    std::array<std::array<float,8>,32> cameras{};
    if(!camera)return decline();
    const auto camera_count=native_gx_.RecentPerspectiveProjections(cameras);
    if(camera_count==0)return decline();
    const float degrees=override!=0?override:float(cpu_.fpr[1]);
    const float width=GX::ReadBigEndianFloat(camera+0xe4),height=GX::ReadBigEndianFloat(camera+0xe8);
    const float aspect=wide?native_gx_.WidescreenTargetAspect():width/height;
    std::array<std::uint8_t,0xf0> adjusted{};
    std::memcpy(adjusted.data(),camera,adjusted.size());
    if(!GX::SetAngularCamera(adjusted.data(),degrees,aspect,focal))return decline();
    // A game may rewrite its native angles before every setter call. Accept
    // either its original projection or the requested projection observed in
    // the preceding frame, including all lens offsets and depth coefficients.
    bool correlated=false;
    for(unsigned i=0;i<camera_count;++i){
      std::array<float,6> observed{};std::copy_n(cameras[i].begin(),6,observed.begin());
      if(GX::SameCameraProjection(GX::AngularProjection(camera),observed) ||
         GX::SameCameraProjection(GX::AngularProjection(adjusted.data()),observed)){correlated=true;break;}
    }
    if(!correlated)return decline();
    if(!GX::SetAngularCamera(camera,degrees,aspect,focal))return decline();
    native_gx_.NotifyMemoryWrite(cpu_.gpr[3],0xf0);
    static std::uint64_t updates=0;
    if(++updates<=8 || updates%3600==0)std::fprintf(stderr,"GEKKOAOT_CAMERA_FRUSTUM provider=angular address=%08x fov=%.3f aspect=%.6f updates=%llu culling=guest\n",cpu_.gpr[3],degrees,aspect,(unsigned long long)updates);
    cpu_.fpr[1]=std::tan(degrees*0.008726646259971648f);
    cpu_.pc=cpu_.lr;++hle_calls_;return true;
  }
  if(kind >= SimpleHleKind::OsGetRtc || kind==SimpleHleKind::PadRead) {
    if(kind==SimpleHleKind::PadRead && !EnvNativePad()) {
      if(native_only_) SetFault(FaultKind::HleFailure,address,static_cast<unsigned>(kind));
      else {hle_decline_entry_=address;++hle_guest_declines_;}
      return false;
    }
    const bool handled=[&] {
      if constexpr(NativeKind>=0)return DispatchDeviceSdkNative<NativeKind>(kind,address);
      else return DispatchDeviceSdk(kind,address);
    }();
    if(!handled) {
      if(native_only_) {
        std::fprintf(stderr,"GEKKOAOT_NATIVE_ONLY_UNSUPPORTED subsystem=sdk operation=%s pc=%08x\n",SimpleServiceName(static_cast<unsigned>(kind)),address);
        SetFault(FaultKind::HleFailure,address,static_cast<unsigned>(kind));
      }
      else { hle_decline_entry_=address; ++hle_guest_declines_; }
      return false;
    }
    ++hle_calls_; ++simple_hle_kind_calls_[static_cast<unsigned>(kind)];
    return true;
  }
  ++hle_calls_;
  ++simple_hle_kind_calls_[static_cast<std::size_t>(kind)];
  const auto finish=[&]() { cpu_.pc=cpu_.lr; return true; };
  // Simple libc string HLEs may receive pointers in the SDK VM aperture.
  // AddressSpace::Resolve() intentionally returns nullptr for paged VMEM, so
  // treating every C string as one host-contiguous span makes valid calls such
  // as CoD Finest Hour's strpbrk() fail as a native-HLE error. Keep a few
  // per-dispatch scratch buffers so two-argument string helpers can hold both
  // translated strings at once. The guest address remains authoritative for
  // return values; the copied bytes are only a host view of the string.
  std::array<std::vector<std::uint8_t>,4> c_string_scratch{};
  std::size_t c_string_scratch_used=0u;
  bool c_string_not_resident=false;
  const auto c_string=[&](std::uint32_t guest,const std::uint8_t** data,std::size_t* length)->bool {
    if(!data||!length)return false;
    if(memory_.PagedVmem() && AddressSpace::IsVmem(guest)) {
      if(c_string_scratch_used>=c_string_scratch.size()) return false;
      auto& copy=c_string_scratch[c_string_scratch_used++];
      std::uint32_t ea=guest;
      const std::uint32_t vmem_end=
          AddressSpace::FakeVmemBase+AddressSpace::RetailFakeVmemSize;
      while(AddressSpace::IsVmem(ea) && ea<vmem_end) {
        const auto translation=TranslateVmemAddress(ea,false);
        if(translation.status!=PageTranslation::Status::Mapped) {
          c_string_not_resident=true;  // v301: decline to the guest code
          return false;
        }
        const std::uint32_t bytes=std::min<std::uint32_t>(
            0x1000u-(ea&0xfffu),vmem_end-ea);
        if(translation.physical>=memory_.Mem1().size() ||
           bytes>memory_.Mem1().size()-translation.physical) return false;
        const auto* begin=memory_.Mem1().data()+translation.physical;
        const auto* end=static_cast<const std::uint8_t*>(std::memchr(begin,0,bytes));
        copy.insert(copy.end(),begin,end ? end : begin+bytes);
        if(end) {
          copy.push_back(0u);
          *data=copy.data();
          *length=copy.size()-1u;
          return true;
        }
        ea+=bytes;
      }
      return false;
    }
    const std::size_t available=memory_.ContiguousAvailable(guest);
    if(available==0u)return false;
    const auto* ptr=memory_.Resolve(guest,1u);
    if(!ptr)return false;
    const void* end=std::memchr(ptr,0,available);
    if(!end)return false;
    *data=ptr;
    *length=static_cast<const std::uint8_t*>(end)-ptr;
    return true;
  };
  // A paged-VM operand is not resident. Nothing has been modified yet:
  // admit the original AOT body for exactly this entry once (the generated
  // static-HLE stub falls through to its region probe when the service
  // declines) so the guest loop faults page by page with resumable state.
  const auto decline_to_guest=[&]() -> bool {
    hle_decline_entry_=address;
    ++hle_guest_declines_;
    return false;
  };
  const auto string_hle_failure=[&]() -> bool {
    if(c_string_not_resident) return decline_to_guest();
    SetFault(FaultKind::HleFailure,address);
    return false;
  };
  // VM-correct memory helpers (TranslateGuestRange).
  const auto range_failure=[&](GuestRangeStatus status) -> bool {
    // A range a native service cannot translate (an unusual mapping)
    // runs the original guest body, which performs the exact architectural
    // accesses, instead of stopping the game.
    static unsigned decline_logs=0;
    if(status!=GuestRangeStatus::NotResident && decline_logs++<16u)
      std::fprintf(stderr,"GEKKOAOT_NATIVE_HLE_RANGE_DECLINE_V365 pc=%08x kind=%u r3=%08x r4=%08x r5=%08x action=guest-body\n",
                   address,static_cast<unsigned>(kind),cpu_.gpr[3],cpu_.gpr[4],cpu_.gpr[5]);
    return decline_to_guest();
  };
  const auto gather=[](const std::vector<GuestSpan>& spans,std::uint8_t* out) {
    for(const auto& span:spans){std::memcpy(out,span.host,span.length);out+=span.length;}
  };
  const auto scatter=[](const std::vector<GuestSpan>& spans,const std::uint8_t* in) {
    for(const auto& span:spans){std::memmove(span.host,in,span.length);in+=span.length;}
  };
  // Bounded C-string read with guest access fidelity -- page by page,
  // never touching a byte past the terminator or the limit. *length excludes
  // the terminator; *terminated reports whether one was found within limit.
  // A null buffer only measures.
  const auto bounded_string=[&](std::uint32_t base,std::uint32_t limit,std::vector<std::uint8_t>* buf,
                                std::uint32_t* length,bool* terminated)->GuestRangeStatus {
    if(buf) buf->clear();
    *length=0u; *terminated=false;
    auto& span=hle_spans_[0];
    while(*length<limit) {
      const std::uint32_t ea=base+*length;
      const std::uint32_t chunk=std::min<std::uint32_t>(limit-*length,0x1000u-(ea&0xfffu));
      if(const auto st=TranslateGuestRange(ea,chunk,false,span);st!=GuestRangeStatus::Ok) return st;
      for(const auto& piece:span) {
        const auto* zero=static_cast<const std::uint8_t*>(std::memchr(piece.host,0,piece.length));
        const std::uint32_t take=zero?static_cast<std::uint32_t>(zero-piece.host):piece.length;
        if(buf) buf->insert(buf->end(),piece.host,piece.host+take);
        *length+=take;
        if(zero){*terminated=true;return GuestRangeStatus::Ok;}
      }
    }
    return GuestRangeStatus::Ok;
  };
  switch(kind) {
#define DEVICE_SDK(k, text) case SimpleHleKind::k:
#include "native/device_sdk.def"
#undef DEVICE_SDK
    return false;

  case SimpleHleKind::GxSetArray: {
    auto& proof=gx_set_array_proof_;
    if(proof.address!=address || proof.epoch!=fixed_exec_route_epoch_) {
      proof.leaf=false;
      proof.valid=GekkoAOTSdk::ProveGxSetArray(memory_.Resolve(address,140u),140u,&proof.leaf);
      proof.address=address;proof.epoch=fixed_exec_route_epoch_;
    }
    const bool leaf=proof.leaf;
    if(!native_sdk_gx_arrays_enabled_ || !proof.valid) return decline_to_guest();
    std::array<std::uint8_t,12> packet{};
    if(!GekkoAOTSdk::GxVertexArrayPacket(cpu_.gpr[3],cpu_.gpr[4],cpu_.gpr[5],packet,leaf)) return decline_to_guest();
    if(!native_gx_.Ready()) return decline_to_guest();
    const auto index=(cpu_.gpr[3]==25u?10u:cpu_.gpr[3])-9u;
    const auto stride=cpu_.gpr[5]&0xffu;
    const auto input_attribute=cpu_.gpr[3],input_base=cpu_.gpr[4];
    if(TryTakeFifoWriteInterrupt(true)) return true;
    struct AtomicService {
      bool& active; bool previous;
      explicit AtomicService(bool& f):active(f),previous(f){active=true;}
      ~AtomicService(){active=previous;}
    } atomic(atomic_fifo_service_);
    if(native_sdk_gx_packets_enabled_) {
      // Publish a typed host operation at its exact transport position.
      // The guest FIFO mirror/accounting remain observable; the renderer skips
      // decoding these two CP commands only after checking the transport and
      // a complete-command boundary. Backlogged/breakpoint/custom paths retain
      // their original decoder. No guest pointer or CPUState crosses this ABI.
      const bool direct = CpuFifoTargetsGpu() &&
          ((!pi_.FifoBase() && !pi_.FifoEnd()) || GekkoAOT::NativeHW::CP::CanDirectSubmit(
            native_hw_.CP(), native_gx_.Ready(), cp_.GPLinkEnabled(), cp_.GPReadEnabled(),
            cp_.BreakpointActiveForHost() || cp_.RegisterDrainPending(), cp_.FifoReadWriteDistance(),
            cp_.FifoReadPointer() == cp_.FifoWritePointer()));
      if(direct && native_gx_.HasTypedArrayCommands() &&
          !native_gx_.StageArrayBinding(index,input_base,stride,cpu_fifo_gather_size_+gpu_fifo_gather_size_)) {
        SetFault(FaultKind::MmioWrite,0xcc008000u,12u);return false;
      }
      if(!QueueSdkFifoPacket(packet.data(),packet.size())) return false;
    } else {
      for(unsigned off=0;off<12;off+=6) {
        ExternalWrite(&cpu_,0xcc008000,packet[off],1);
        ExternalWrite(&cpu_,0xcc008000,packet[off+1],1);
        const auto word=(std::uint32_t(packet[off+2])<<24)|(std::uint32_t(packet[off+3])<<16)|
                        (std::uint32_t(packet[off+4])<<8)|packet[off+5];
        ExternalWrite(&cpu_,0xcc008000,word,4);
        if(fault_.kind!=FaultKind::None || cpu_.exception)return false;
      }
    }
    // Preserve even the leaf's volatile register/CR/XER effects. The admitted
    // vertex path ends at bltlr after its second addic. (index - 12 < 0).
    if(leaf) {
      cpu_.gpr[0]=stride;cpu_.gpr[3]=index|0xb0u;cpu_.gpr[4]=input_base&0x3fffffffu;
      cpu_.gpr[6]=0xcc010000u;cpu_.gpr[7]=8u;
      cpu_.cr=(cpu_.cr&0x0fffffffu)|(input_attribute==25u?0x20000000u:0x80000000u)|((cpu_.xer&0x80000000u)>>3);
    } else {
      cpu_.gpr[0]=index-12u;cpu_.gpr[3]=0xcc010000u;cpu_.gpr[4]=stride;cpu_.gpr[6]=index;
      cpu_.xer&=~0x20000000u;
      cpu_.cr=(cpu_.cr&0x0fffffffu)|0x80000000u|((cpu_.xer&0x80000000u)>>3);
    }
    return finish();
  }
  case SimpleHleKind::DvdFastOpen: {
    // Use the SDK's live SDA globals, not a guessed low-memory FST mirror.
    // All reads/writes are admitted before mutation; paged/invalid/aliased
    // operands decline to the original resumable AOT body.
    if (!EnvNativeDvdFst()) return decline_to_guest();
    const auto proof=GekkoAOTSdk::ProveDvdFastOpen(memory_.Resolve(address,116u),116u);
    if (!proof) return decline_to_guest();
    const auto entry=cpu_.gpr[3];
    if (static_cast<std::int32_t>(entry)<0) {cpu_.gpr[3]=0;return finish();}
    const auto count_addr=cpu_.gpr[13]+static_cast<std::uint32_t>(proof->count_offset);
    const auto fst_addr=cpu_.gpr[13]+static_cast<std::uint32_t>(proof->fst_offset);
    auto read_word=[&](std::uint32_t ea,std::uint32_t& word) {
      auto& spans=hle_spans_[0];
      if (TranslateGuestRange(ea,4u,false,spans)!=GuestRangeStatus::Ok) return false;
      std::array<std::uint8_t,4> bytes{};gather(spans,bytes.data());
      word=(std::uint32_t(bytes[0])<<24)|(std::uint32_t(bytes[1])<<16)|
           (std::uint32_t(bytes[2])<<8)|bytes[3];return true;
    };
    std::uint32_t count=0,fst=0;
    if (!read_word(count_addr,count)) return decline_to_guest();
    if (entry>=count) {cpu_.gpr[3]=0;return finish();}
    if (!read_word(fst_addr,fst) || entry>(UINT32_MAX-8u)/12u ||
        fst>UINT32_MAX-entry*12u-8u) return decline_to_guest();
    const auto item=fst+entry*12u;
    std::uint32_t name=0,start=0,length=0;
    if (!read_word(item,name)) return decline_to_guest();
    if (name&0xff000000u) {cpu_.gpr[3]=0;return finish();}
    if (!read_word(item+4u,start) || !read_word(item+8u,length)) return decline_to_guest();
    const auto info=cpu_.gpr[4];
    if (info>UINT32_MAX-0x3bu) return decline_to_guest();
    std::array<std::vector<GuestSpan>,3> inputs;
    for (unsigned i=0;i<3;++i) {
      const auto src=i==0?count_addr:i==1?fst_addr:item;
      if (TranslateGuestRange(src,i==2?12u:4u,false,inputs[i])!=GuestRangeStatus::Ok)
        return decline_to_guest();
    }
    constexpr std::array<std::uint32_t,4> offsets{0x30,0x34,0x38,0x0c};
    const std::array<std::uint32_t,4> values{start,length,0,0};
    for (std::size_t i=0;i<4;++i) {
      const auto dst=info+offsets[i];
      const auto overlaps=[&](std::uint32_t src,std::uint32_t bytes) {
        return std::uint64_t(dst)<std::uint64_t(src)+bytes && std::uint64_t(src)<std::uint64_t(dst)+4u;
      };
      if (overlaps(count_addr,4)||overlaps(fst_addr,4)||overlaps(item,12) ||
          TranslateGuestRange(dst,4u,true,hle_spans_[i])!=GuestRangeStatus::Ok)
        return decline_to_guest();
      // Cached/uncached aliases and translated VM pages can overlap despite
      // distinct EAs. Preserve the original leaf's write/re-read ordering.
      for (const auto& out:hle_spans_[i]) {
        const auto overlaps_host=[&](const GuestSpan& in) {
          const auto a=reinterpret_cast<std::uintptr_t>(out.host);
          const auto b=reinterpret_cast<std::uintptr_t>(in.host);
          return a<b+in.length && b<a+out.length;
        };
        for (const auto& input:inputs)
          for (const auto& in:input) if (overlaps_host(in)) return decline_to_guest();
        for (std::size_t j=0;j<i;++j)
          for (const auto& in:hle_spans_[j]) if (overlaps_host(in)) return decline_to_guest();
      }
    }
    for (std::size_t i=0;i<4;++i) {
      const auto v=values[i];
      const std::array<std::uint8_t,4> bytes{std::uint8_t(v>>24),std::uint8_t(v>>16),std::uint8_t(v>>8),std::uint8_t(v)};
      scatter(hle_spans_[i],bytes.data());
    }
    cpu_.gpr[3]=1;return finish();
  }
  case SimpleHleKind::PadRead: return false; // dispatched before generic libc scratch
  case SimpleHleKind::OsGetTime: NoteClockCall(true); return GekkoAOT::Time::GetTime(&cpu_);
  case SimpleHleKind::OsGetTick: NoteClockCall(false); return GekkoAOT::Time::GetTick(&cpu_);
  case SimpleHleKind::Memcpy:
  case SimpleHleKind::Memmove: {
    // memmove semantics for both: an overlapping memcpy is undefined in C.
    const std::uint32_t dst_addr=cpu_.gpr[3],src_addr=cpu_.gpr[4],size=cpu_.gpr[5];
    auto& src=hle_spans_[0]; auto& dst=hle_spans_[1];
    if(const auto st=TranslateGuestRange(src_addr,size,false,src);st!=GuestRangeStatus::Ok) return range_failure(st);
    if(const auto st=TranslateGuestRange(dst_addr,size,true,dst);st!=GuestRangeStatus::Ok) return range_failure(st);
    if(src.size()<=1u && dst.size()<=1u) {
      if(size) std::memmove(dst[0].host,src[0].host,size);
    } else {
      hle_scratch_.resize(size); gather(src,hle_scratch_.data()); scatter(dst,hle_scratch_.data());
    }
    cpu_.gpr[3]=dst_addr;
    return finish();
  }
  case SimpleHleKind::Memset:
  case SimpleHleKind::Bzero: {
    const bool bzero=kind==SimpleHleKind::Bzero;
    const std::uint32_t dst_addr=cpu_.gpr[3],size=bzero?cpu_.gpr[4]:cpu_.gpr[5];
    const int value=bzero?0:static_cast<int>(cpu_.gpr[4]&0xffu);
    auto& dst=hle_spans_[1];
    if(const auto st=TranslateGuestRange(dst_addr,size,true,dst);st!=GuestRangeStatus::Ok) return range_failure(st);
    for(const auto& span:dst) std::memset(span.host,value,span.length);
    if(!bzero) cpu_.gpr[3]=dst_addr;
    return finish();
  }
  case SimpleHleKind::Memcmp: {
    const std::uint32_t lhs_addr=cpu_.gpr[3],rhs_addr=cpu_.gpr[4],size=cpu_.gpr[5];
    auto& lhs=hle_spans_[0]; auto& rhs=hle_spans_[1];
    if(const auto st=TranslateGuestRange(lhs_addr,size,false,lhs);st!=GuestRangeStatus::Ok) return range_failure(st);
    if(const auto st=TranslateGuestRange(rhs_addr,size,false,rhs);st!=GuestRangeStatus::Ok) return range_failure(st);
    const std::uint8_t *a=nullptr,*b=nullptr;
    if(lhs.size()<=1u && rhs.size()<=1u) {
      if(size){a=lhs[0].host;b=rhs[0].host;}
    } else {
      hle_scratch_.resize(std::size_t(size)*2u);
      gather(lhs,hle_scratch_.data()); gather(rhs,hle_scratch_.data()+size);
      a=hle_scratch_.data(); b=hle_scratch_.data()+size;
    }
    // Retail libc returns the difference of the first mismatching bytes.
    std::int32_t result=0;
    for(std::uint32_t i=0;i<size;++i) if(a[i]!=b[i]){result=int(a[i])-int(b[i]);break;}
    cpu_.gpr[3]=static_cast<std::uint32_t>(result); return finish();
  }
  case SimpleHleKind::Strlen: {
    const std::uint8_t* src=nullptr; std::size_t length=0;
    if(!c_string(cpu_.gpr[3],&src,&length)) return string_hle_failure();
    cpu_.gpr[3]=static_cast<std::uint32_t>(length); return finish();
  }
  case SimpleHleKind::Strcmp: {
    const std::uint8_t *lhs=nullptr,*rhs=nullptr; std::size_t ll=0,rl=0;
    if(!c_string(cpu_.gpr[3],&lhs,&ll)||!c_string(cpu_.gpr[4],&rhs,&rl)) return string_hle_failure();
    std::size_t i=0; const std::size_t common=std::min(ll,rl);
    for(;i<common&&lhs[i]==rhs[i];++i){}
    const int result = int(lhs[i]) - int(rhs[i]);
    cpu_.gpr[3]=static_cast<std::uint32_t>(static_cast<std::int32_t>(result)); return finish();
  }
  case SimpleHleKind::Strncmp: {
    // Lockstep page walk over both strings: touches exactly the bytes the
    // guest loop reads (up to the first difference or terminator), so paged
    // VM operands raise the same DSI and RFI retries the whole call.
    const std::uint32_t lhs_addr=cpu_.gpr[3],rhs_addr=cpu_.gpr[4],limit=cpu_.gpr[5];
    auto& ls=hle_spans_[0]; auto& rs=hle_spans_[1];
    std::int32_t result=0; bool done=false;
    for(std::uint32_t i=0u;i<limit&&!done;) {
      const std::uint32_t la=lhs_addr+i,ra=rhs_addr+i;
      const std::uint32_t chunk=std::min({limit-i,0x1000u-(la&0xfffu),0x1000u-(ra&0xfffu)});
      if(const auto st=TranslateGuestRange(la,chunk,false,ls);st!=GuestRangeStatus::Ok) return range_failure(st);
      if(const auto st=TranslateGuestRange(ra,chunk,false,rs);st!=GuestRangeStatus::Ok) return range_failure(st);
      const std::uint32_t n=std::min(ls[0].length,rs[0].length);
      const std::uint8_t *a=ls[0].host,*b=rs[0].host;
      for(std::uint32_t k=0u;k<n;++k) {
        if(a[k]!=b[k]){result=int(a[k])-int(b[k]);done=true;break;}
        if(a[k]==0u){done=true;break;}
      }
      i+=n;
    }
    cpu_.gpr[3]=static_cast<std::uint32_t>(result); return finish();
  }
  case SimpleHleKind::Strcpy: {
    const std::uint32_t dst_addr=cpu_.gpr[3],src_addr=cpu_.gpr[4];
    const std::uint8_t* src=nullptr; std::size_t length=0;
    if(!c_string(src_addr,&src,&length)) return string_hle_failure();
    if(length>=std::numeric_limits<std::uint32_t>::max()){SetFault(FaultKind::HleFailure,address);return false;}
    auto& dst=hle_spans_[1];
    if(const auto st=TranslateGuestRange(dst_addr,static_cast<std::uint32_t>(length+1u),true,dst);st!=GuestRangeStatus::Ok) return range_failure(st);
    scatter(dst,src); cpu_.gpr[3]=dst_addr; return finish();
  }
  case SimpleHleKind::Strncpy: {
    // Copy up to the terminator, then NUL-pad to exactly `limit` bytes.
    const std::uint32_t dst_addr=cpu_.gpr[3],src_addr=cpu_.gpr[4],limit=cpu_.gpr[5];
    std::uint32_t length=0u; bool terminated=false;
    if(const auto st=bounded_string(src_addr,limit,&hle_scratch_,&length,&terminated);st!=GuestRangeStatus::Ok) return range_failure(st);
    auto& dst=hle_spans_[1];
    if(const auto st=TranslateGuestRange(dst_addr,limit,true,dst);st!=GuestRangeStatus::Ok) return range_failure(st);
    hle_scratch_.resize(limit,0u);
    scatter(dst,hle_scratch_.data()); cpu_.gpr[3]=dst_addr; return finish();
  }
  case SimpleHleKind::Strcat: {
    const std::uint32_t dst_addr=cpu_.gpr[3],src_addr=cpu_.gpr[4];
    const std::uint8_t *dst_ro=nullptr,*src=nullptr; std::size_t dl=0,sl=0;
    if(!c_string(dst_addr,&dst_ro,&dl)||!c_string(src_addr,&src,&sl)) return string_hle_failure();
    if(dl+sl+1u>=0x100000000ull){SetFault(FaultKind::HleFailure,address);return false;}
    auto& dst=hle_spans_[1];
    if(const auto st=TranslateGuestRange(dst_addr+static_cast<std::uint32_t>(dl),static_cast<std::uint32_t>(sl+1u),true,dst);st!=GuestRangeStatus::Ok) return range_failure(st);
    scatter(dst,src); cpu_.gpr[3]=dst_addr; return finish();
  }
  case SimpleHleKind::Strchr:
  case SimpleHleKind::Strrchr: {
    const std::uint32_t src_addr=cpu_.gpr[3]; const std::uint8_t needle=static_cast<std::uint8_t>(cpu_.gpr[4]);
    const std::uint8_t* src=nullptr; std::size_t length=0;
    if(!c_string(src_addr,&src,&length)) return string_hle_failure();
    const std::size_t count=length+1u; const std::uint8_t* found=nullptr;
    if(kind==SimpleHleKind::Strchr){ found=static_cast<const std::uint8_t*>(std::memchr(src,needle,count)); }
    else { for(std::size_t i=count;i>0u;--i)if(src[i-1u]==needle){found=src+i-1u;break;} }
    cpu_.gpr[3]=found?src_addr+static_cast<std::uint32_t>(found-src):0u; return finish();
  }
  case SimpleHleKind::Memchr: {
    // Page at a time: like the guest loop, never touch bytes past the match.
    const std::uint32_t src_addr=cpu_.gpr[3],size=cpu_.gpr[5]; const std::uint8_t needle=static_cast<std::uint8_t>(cpu_.gpr[4]);
    std::uint32_t done=0u;
    while(done<size) {
      const std::uint32_t ea=src_addr+done;
      const std::uint32_t chunk=std::min<std::uint32_t>(size-done,0x1000u-(ea&0xfffu));
      auto& span=hle_spans_[0];
      if(const auto st=TranslateGuestRange(ea,chunk,false,span);st!=GuestRangeStatus::Ok) return range_failure(st);
      std::uint32_t offset=0u;
      for(const auto& piece:span) {
        if(const auto* hit=static_cast<const std::uint8_t*>(std::memchr(piece.host,needle,piece.length))) {
          cpu_.gpr[3]=ea+offset+static_cast<std::uint32_t>(hit-piece.host); return finish();
        }
        offset+=piece.length;
      }
      done+=chunk;
    }
    cpu_.gpr[3]=0u; return finish();
  }
  case SimpleHleKind::Strstr: {
    const std::uint32_t hay_addr=cpu_.gpr[3],needle_addr=cpu_.gpr[4];
    const std::uint8_t *hay=nullptr,*needle=nullptr; std::size_t hl=0,nl=0;
    if(!c_string(needle_addr,&needle,&nl)) return string_hle_failure();
    if(nl==0u){cpu_.gpr[3]=hay_addr;return finish();}
    if(!c_string(hay_addr,&hay,&hl)) return string_hle_failure();
    const auto* found=std::search(hay,hay+hl,needle,needle+nl);
    cpu_.gpr[3]=found==hay+hl?0u:hay_addr+static_cast<std::uint32_t>(found-hay); return finish();
  }
  case SimpleHleKind::Strncat: {
    // Append at most `limit` source bytes (stopping at its terminator) and
    // always terminate, exactly like the retail loop.
    const std::uint32_t dst_addr=cpu_.gpr[3],src_addr=cpu_.gpr[4],limit=cpu_.gpr[5];
    const std::uint8_t* dst_ro=nullptr; std::size_t dl=0;
    if(!c_string(dst_addr,&dst_ro,&dl)) return string_hle_failure();
    if(dl>=0xffffffffull){SetFault(FaultKind::HleFailure,address);return false;}
    std::uint32_t length=0u; bool terminated=false;
    if(const auto st=bounded_string(src_addr,limit,&hle_scratch_,&length,&terminated);st!=GuestRangeStatus::Ok) return range_failure(st);
    hle_scratch_.push_back(0u);
    auto& dst=hle_spans_[1];
    if(const auto st=TranslateGuestRange(dst_addr+static_cast<std::uint32_t>(dl),length+1u,true,dst);st!=GuestRangeStatus::Ok) return range_failure(st);
    scatter(dst,hle_scratch_.data()); cpu_.gpr[3]=dst_addr; return finish();
  }
  case SimpleHleKind::Strnlen: {
    std::uint32_t length=0u; bool terminated=false;
    if(const auto st=bounded_string(cpu_.gpr[3],cpu_.gpr[4],nullptr,&length,&terminated);st!=GuestRangeStatus::Ok) return range_failure(st);
    cpu_.gpr[3]=length; return finish();
  }
  case SimpleHleKind::Strcspn:
  case SimpleHleKind::Strpbrk: {
    const std::uint32_t src_addr=cpu_.gpr[3],set_addr=cpu_.gpr[4];
    const std::uint8_t *src=nullptr,*set=nullptr; std::size_t sl=0,setl=0;
    if(!c_string(src_addr,&src,&sl)||!c_string(set_addr,&set,&setl)) return string_hle_failure();
    std::array<bool,256> table{}; for(std::size_t i=0;i<setl;++i)table[set[i]]=true;
    std::size_t i=0; while(i<sl&&!table[src[i]])++i;
    if(kind==SimpleHleKind::Strcspn) cpu_.gpr[3]=static_cast<std::uint32_t>(i);
    else cpu_.gpr[3]=i<sl?src_addr+static_cast<std::uint32_t>(i):0u;
    return finish();
  }
  case SimpleHleKind::DcFlushRange:
  case SimpleHleKind::DcFlushRangeNoSync:
  case SimpleHleKind::DcStoreRange:
  case SimpleHleKind::DcStoreRangeNoSync:
  case SimpleHleKind::DcInvalidateRange:
  case SimpleHleKind::DcZeroRange:
  case SimpleHleKind::DcTouchRange:
  case SimpleHleKind::IcInvalidateRange: {
    const std::uint32_t addr=cpu_.gpr[3],size=cpu_.gpr[4];
    if(size==0u)return finish();
    const std::uint64_t base=std::uint64_t(addr&~31u);
    const std::uint64_t end=std::uint64_t(addr)+std::uint64_t(size);
    if(end>0x100000000ull){SetFault(FaultKind::HleFailure,address);return false;}
    const std::uint64_t rounded_end=(end+31ull)&~31ull;
    if(rounded_end>0x100000000ull||rounded_end<base){SetFault(FaultKind::HleFailure,address);return false;}
    const std::uint32_t range_addr=static_cast<std::uint32_t>(base);
    const std::uint32_t range_size=static_cast<std::uint32_t>(rounded_end-base);

    if(kind==SimpleHleKind::DcZeroRange){
      auto& dst=hle_spans_[1];
      if(const auto st=TranslateGuestRange(range_addr,range_size,true,dst);st!=GuestRangeStatus::Ok) return range_failure(st);
      for(const auto& span:dst) std::memset(span.host,0,span.length);
      native_gx_.NotifyMemoryWrite(range_addr,range_size);
      return finish();
    }
    if(kind==SimpleHleKind::DcTouchRange) return finish(); // host prefetch is unnecessary
    if(kind!=SimpleHleKind::IcInvalidateRange){
      // Host RAM is coherent; the architectural D-cache operation is the
      // publication boundary for Aurora's cached arrays/textures. Publish the
      // whole SDK range in one DSO call rather than one callback per 32-byte line.
      native_gx_.NotifyMemoryWrite(range_addr,range_size);
      return finish();
    }

    // AOT instruction-cache invalidation: invalidate the low-memory I-cache
    // lines that are actually modeled and drop executable/REL route caches once
    // for the whole SDK range instead of once per guest icbi instruction.
    InvalidateInstructionRange(range_addr,range_size);
    return finish();
  }
  case SimpleHleKind::GxLoadPosMtxImm:
  case SimpleHleKind::GxLoadNrmMtxImm:
  case SimpleHleKind::GxLoadNrmMtxImm3x3:
  case SimpleHleKind::GxLoadTexMtxImm: {
    if (cpu_.gqr[0]!=0u || !(cpu_.msr&0x2000u) ||
        !EnvGxMatrixHle()) return decline_to_guest();
    const bool normal=kind==SimpleHleKind::GxLoadNrmMtxImm || kind==SimpleHleKind::GxLoadNrmMtxImm3x3;
    if (normal && !EnvNativeGxNormalMatrix()) return decline_to_guest();
    // An intercepted SDK packet has no resumable instruction for each chunk.
    // Deliver an already pending overflow IRQ at its entry, then publish the
    // entire bounded packet atomically with respect to this host-side fence.
    if (TryTakeFifoWriteInterrupt(true)) return true;
    struct AtomicFifoService {
      bool& active;
      bool previous;
      explicit AtomicFifoService(bool& flag) : active(flag), previous(flag) { active = true; }
      ~AtomicFifoService() { active = previous; }
    } atomic_service(atomic_fifo_service_);
    // SFA, SMB and the debug ELF corpus all contain the same retail SDK
    // matrix-upload leaves.  They do no CPU-visible work beyond writing an XF
    // command and raw big-endian f32 matrix bytes to WGPIPE.  Feed those bytes
    // into the existing gather/CP path directly so PI/CP breakpoints and FIFO
    // accounting stay authoritative while removing the guest PSQ + MMIO call
    // chain.
    if(!native_gx_.Ready()){SetFault(FaultKind::HleFailure,address);return false;}
    const std::uint32_t matrix_addr=cpu_.gpr[3];
    const std::uint32_t id=cpu_.gpr[4];
    std::uint32_t xf_addr=0u;
    std::uint32_t payload_bytes=0u;
    if(kind==SimpleHleKind::GxLoadPosMtxImm){
      xf_addr=0x000b0000u|(id*4u); payload_bytes=48u;
    }else if(normal){
      xf_addr=0x00080000u|((id*3u)+0x400u);payload_bytes=36u;
    }else{
      const std::uint32_t type=cpu_.gpr[5];
      if(type>1u){SetFault(FaultKind::HleFailure,address);return false;}
      const std::uint32_t count=type==1u?8u:12u;
      const std::uint32_t base=id>=64u?(id-64u)*4u+0x500u:id*4u;
      xf_addr=base|((count-1u)<<16u); payload_bytes=count*4u;
    }
    // GX matrix sources may live in the Gekko locked-cache aperture. SMB1
    // keeps its current position matrix at 0xE0000000, which ordinary MEM1
    // resolution does not cover. Use the same guest-pointer resolver as the
    // compiled CPU's external pointer path.
    const auto source_bytes=kind==SimpleHleKind::GxLoadNrmMtxImm?44u:payload_bytes;
    std::array<std::uint8_t,48> matrix_copy{};
    const auto* matrix=static_cast<const std::uint8_t*>(lc_.Pointer(matrix_addr,source_bytes));
    if (!matrix) {
      auto& spans=hle_spans_[0];
      if (TranslateGuestRange(matrix_addr,source_bytes,false,spans)!=GuestRangeStatus::Ok)
        return decline_to_guest();
      gather(spans,matrix_copy.data());matrix=matrix_copy.data();
    }
    std::array<std::uint8_t,53> fifo{};
    std::size_t n=0u;
    fifo[n++]=0x10u;
    fifo[n++]=static_cast<std::uint8_t>(xf_addr>>24u);
    fifo[n++]=static_cast<std::uint8_t>(xf_addr>>16u);
    fifo[n++]=static_cast<std::uint8_t>(xf_addr>>8u);
    fifo[n++]=static_cast<std::uint8_t>(xf_addr);
    // WriteMTXPS4x3/4x2 use qr0 raw-f32 PSQ stores.  Reproduce the
    // architectural raw-f32 load/store round trip: denormals flush to +0 and
    // signaling NaNs are quieted; all ordinary matrix values remain bit exact.
    for(std::uint32_t off=0;off<payload_bytes;off+=4u){
      const auto src=kind==SimpleHleKind::GxLoadNrmMtxImm?(off/12u)*16u+off%12u:off;
      std::uint32_t bits=(std::uint32_t(matrix[src])<<24u)|
                         (std::uint32_t(matrix[src+1u])<<16u)|
                         (std::uint32_t(matrix[src+2u])<<8u)|matrix[src+3u];
      const std::uint32_t exponent=bits&0x7f800000u;
      const std::uint32_t fraction=bits&0x007fffffu;
      const bool scalar=normal && (kind==SimpleHleKind::GxLoadNrmMtxImm3x3 ? off==32u : off%12u==8u);
      if(!scalar&&exponent==0u&&fraction!=0u) bits=0u;
      else if(exponent==0x7f800000u&&fraction!=0u) bits|=0x00400000u;
      fifo[n++]=static_cast<std::uint8_t>(bits>>24u);
      fifo[n++]=static_cast<std::uint8_t>(bits>>16u);
      fifo[n++]=static_cast<std::uint8_t>(bits>>8u);
      fifo[n++]=static_cast<std::uint8_t>(bits);
    }
    if (native_sdk_gx_packets_enabled_) {
      // A recognized SDK matrix upload is an ordered native renderer
      // operation. The compatibility mirror still receives the canonical bytes
      // for guest FIFO reads/recording; the renderer does not decode XF packets.
      const auto words=payload_bytes/4u;
      const auto destination=xf_addr&0xffffu;
      const bool direct = native_gx_.HasDirectMatrixCommands() &&
          (xf_addr>>16u)+1u==words && GekkoAOT::GX::NativeMatrixRange(destination,words) &&
          CpuFifoTargetsGpu() && ((!pi_.FifoBase() && !pi_.FifoEnd()) ||
          GekkoAOT::NativeHW::CP::CanDirectSubmit(native_hw_.CP(),native_gx_.Ready(),
            cp_.GPLinkEnabled(),cp_.GPReadEnabled(),
            cp_.BreakpointActiveForHost()||cp_.RegisterDrainPending(),cp_.FifoReadWriteDistance(),
            cp_.FifoReadPointer()==cp_.FifoWritePointer()));
      if(direct && !native_gx_.StageMatrix(destination,words,
                                         cpu_fifo_gather_size_+gpu_fifo_gather_size_)) {
        SetFault(FaultKind::MmioWrite,0xcc008000u,n);return false;
      }
      if (!QueueSdkFifoPacket(fifo.data(),n)) return false;
      return finish();
    }
    // Queue up to eight bytes at a time through the normal WGPIPE entrypoint.
    // GXBeginDisplayList may have selected a CPU recording FIFO; submitting
    // directly to the GPU queue would split one physical gather line between
    // the CPU and GPU buffers and fault on the next guest store.
    for(std::size_t off=0;off<n;){
      const std::size_t remaining=n-off;
      const std::uint8_t chunk=remaining>=8u?8u:remaining>=4u?4u:remaining>=2u?2u:1u;
      std::uint64_t value=0u;
      for(std::uint8_t i=0;i<chunk;++i)value=(value<<8u)|fifo[off+i];
      ExternalWrite(&cpu_,0xcc008000u,value,chunk);
      if(fault_.kind!=FaultKind::None || cpu_.exception!=0u)return false;
      off+=chunk;
    }
    return finish();
  }
  case SimpleHleKind::AngularCameraSetFov:
  case SimpleHleKind::TangentCameraFrustum:
  case SimpleHleKind::GxSetAlphaCompare: case SimpleHleKind::GxSetZMode:
  case SimpleHleKind::GxSetColorUpdate: case SimpleHleKind::GxSetAlphaUpdate:
  case SimpleHleKind::Count: break;
  }
  return false;
}

bool HostRuntime::DispatchNativeRegionProbe() {
  const std::uint32_t start = cpu_.external_addr, end = cpu_.external_value;

  // A native service declined this exact entry (non-resident paged-VM
  // operand). The stub stored the entry PC before calling the service; admit
  // the original guest body once so it faults page by page with exact state.
  if (hle_decline_entry_ != 0u && cpu_.pc == hle_decline_entry_ &&
      start <= hle_decline_entry_ && hle_decline_entry_ < end) {
    hle_decline_entry_ = 0u;
    cpu_.external_rid = 0xffu;
    return false;
  }

  // When DolRecomp asks whether a native wrapper range is available
  // from inside a secondary module, a range wholly owned by that same REL is
  // already compiled guest code. Do not bounce to HostRuntime merely because
  // a process-wide intercept numerically falls inside the range. A genuine
  // import/main-DOL/HLE target leaves this descriptor's code ranges and still
  // takes the normal host-routing path.
  if (native_region_secondary_owner_ && start < end &&
      AddressInRanges(native_region_secondary_owner_->code_ranges,
                      native_region_secondary_owner_->num_code_ranges, start) &&
      AddressInRanges(native_region_secondary_owner_->code_ranges,
                      native_region_secondary_owner_->num_code_ranges, end - 1u)) {
    cpu_.external_rid = 0xffu;
    static unsigned rel_owned_guard_logs = 0u;
    if (rel_owned_guard_logs++ < 64u)
      std::fprintf(stderr,
                   "GEKKOAOT_REL_REGION_GUARD_V205D=1 start=%08x end=%08x pc=%08x action=allow-secondary-owned-aot\n",
                   start, end, cpu_.pc);
    return false;
  }

  // One-shot admission for the exact range that produced a proven
  // zero-progress secondary dispatch.
  if (native_region_retry_start_ != 0u &&
      start == native_region_retry_start_) {
    native_region_retry_start_ = 0u;
    cpu_.external_rid = 0xffu;
    static unsigned retry_probe_logs = 0u;
    if (retry_probe_logs++ < 64u)
      std::fprintf(stderr,
                   "GEKKOAOT_REL_REGION_RETRY_V204C=1 start=%08x end=%08x action=allow-one-shot-native-entry\n",
                   start, end);
    return false;
  }

  const std::size_t slot =
      ((start >> 2u) ^ (start >> 13u) ^ (end >> 5u)) &
      (kNativeRegionProbeCacheSize - 1u);
  auto& cached = native_region_probe_cache_[slot];
  bool blocked = false;
  if (cached.valid && cached.start == start && cached.end == end &&
      cached.epoch == fixed_exec_route_epoch_) {
    blocked = cached.blocked;
  } else {
    auto it =
        std::lower_bound(native_intercepts_.begin(), native_intercepts_.end(), start);
    // Fixed ELF/DOL overlays reuse addresses from the bootstrap executable.
    // A bootstrap SDK hook is applicable only while its original code still
    // occupies that address. Otherwise it can reject an entire overlay chunk
    // forever before a single guest instruction executes.
    for (; start < end && it != native_intercepts_.end() && *it < end; ++it) {
      if (NativeHookCodeMatches(*it)) { blocked = true; break; }
    }
    cached = {start, end, blocked, true, fixed_exec_route_epoch_};
  }

  if (blocked && native_region_probe_capture_)
    native_region_last_blocked_start_ = start;

  cpu_.external_rid = 0xffu;
  return blocked;
}

bool HostRuntime::DispatchHostCall(std::uint32_t address) {
  // A handled real HLE call is a host-visible event for the idle-loop
  // prover; admission, REL-translation and indirect-routing probes are not
  // (the guest code an indirect route executes reports its own effects).
  const bool handled = DispatchHostCallImpl(address);
  if (handled && track_host_effects_ && address != kNativeRegionProbe &&
      address != kRelAddressProbe && address != kGlobalIndirectProbe)
    ++host_effects_;
  return handled;
}

bool HostRuntime::DispatchHostCallImpl(std::uint32_t address) {
  // Resolved SDK calls can be lowered by DolRecomp straight to a stable
  // service token.  No runtime address map, interception query, or redispatch
  // through module_dispatch is required for the positive path.  cpu_.pc still
  // contains the real guest entrypoint and is retained for fault diagnostics.
  if ((address & kStaticDirectMask) == kStaticDirectOsBase) {
    const auto raw = static_cast<std::uint8_t>(address & 0xffu);
    if (raw < static_cast<std::uint8_t>(GekkoAOT::NativeOS::Kind::Count))
      return DispatchOsHle(static_cast<GekkoAOT::NativeOS::Kind>(raw), cpu_.pc);
    return false;
  }
  if ((address & kStaticDirectMask) == kStaticDirectSimpleBase) {
    const auto raw = static_cast<std::uint8_t>(address & 0xffu);
    if (raw < static_cast<std::uint8_t>(SimpleHleKind::Count))
      return DispatchSimpleHle(static_cast<SimpleHleKind>(raw), cpu_.pc);
    return false;
  }

  // Generated modules use this host token only when their local indirect
  // dispatcher cannot resolve a bctr/bctrl target. Resolve the target against
  // the complete active AOT set so callbacks and virtual calls may cross fixed
  // DOL/ELF images (and live RELs) without falling back to an emulator.
  if (address == kGlobalIndirectProbe) {
    return DispatchGlobalIndirect(cpu_.external_addr);
  }

  // REL side modules use this sentinel only for canonical->runtime translation.
  if (address == kRelAddressProbe) {
    // REL-generated memory accesses can request canonical->runtime translation
    // before control returns through ResolveSecondaryAddress().  Honor the same
    // publication epoch here so they never translate through a stale OSLink map.
    if (rel_mapping_dirty_) RefreshSecondaryRelMappings();
    const std::uint32_t canonical=cpu_.external_addr;
    std::uint32_t runtime=canonical;
    if(TranslateCanonicalRelAddress(canonical,&runtime)) {
      cpu_.external_value=runtime; cpu_.external_rid=kRelAddressHandled;
      ++rel_address_translations_; rel_last_canonical_=canonical; rel_last_runtime_=runtime;
      return true;
    }
    ++rel_address_misses_; rel_last_canonical_=canonical; rel_last_runtime_=canonical;
    return false;
  }

  // Static-intercept AOT removes this probe from every region that cannot
  // contain a native SDK/HLE entry. The few remaining guarded chunks use the
  // exact sorted entrypoint set, so there is no hot hash/cache layer anymore.
  if (address == kNativeRegionProbe) return DispatchNativeRegionProbe();

  // Exact direct-mapped positive cache. Hot OS/libc HLE no longer pays an
  // unordered_map lookup at every call; collisions simply fall through to the
  // authoritative maps below and refresh this slot.
  const auto fast_slot=((address>>2u)^(address>>13u))&(kHleFastCacheSize-1u);
  const auto fast=hle_fast_cache_[fast_slot];
  if(fast.address==address) {
    if (!NativeHookCodeMatches(address)) return false;
    if(fast.type==HleFastType::Os)
      return DispatchOsHle(static_cast<GekkoAOT::NativeOS::Kind>(fast.kind),address);
    if(fast.type==HleFastType::Simple)
      return DispatchSimpleHle(static_cast<SimpleHleKind>(fast.kind),address);
  }

  const auto negative_slot=((address>>2u)^(address>>14u))&(kHostCallNegativeCacheSize-1u);
  if(hostcall_negative_cache_[negative_slot]==address) return false;

  if(auto it=os_hooks_.find(address);it!=os_hooks_.end()) {
    if (!NativeHookCodeMatches(address)) return false;
    hle_fast_cache_[fast_slot]={address,HleFastType::Os,static_cast<std::uint8_t>(it->second)};
    return DispatchOsHle(it->second,address);
  }
  if(auto it=simple_hle_hooks_.find(address);it!=simple_hle_hooks_.end()) {
    if (!NativeHookCodeMatches(address)) return false;
    hle_fast_cache_[fast_slot]={address,HleFastType::Simple,static_cast<std::uint8_t>(it->second)};
    return DispatchSimpleHle(it->second,address);
  }
  hostcall_negative_cache_[negative_slot]=address;
  return false;
}

#if defined(_MSC_VER)
__declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
__attribute__((noinline))
#endif
bool HostRuntime::AdmitNativeSdkKindSlow(SimpleHleKind kind) {
      const auto slot=((cpu_.pc>>2u)^(cpu_.pc>>13u))&(kHleFastCacheSize-1u);
      const auto cached=hle_fast_cache_[slot];
      bool admitted=cached.address==cpu_.pc&&cached.type==HleFastType::Simple&&cached.kind==unsigned(kind);
      if (!admitted) {
        const auto hook=simple_hle_hooks_.find(cpu_.pc);
        admitted=hook!=simple_hle_hooks_.end()&&static_cast<unsigned>(hook->second)==unsigned(kind);
      }
      if (!admitted || !NativeHookCodeMatches(cpu_.pc)) {
        // Rejected SDK annotations and software-only SDK wait adapters
        // use their real AOT bodies once, never a device fallback.
        // Strict MMIO guards still reject any untranslated hardware access.
        const auto rejected=rejected_simple_hle_hooks_.find(cpu_.pc);
        if(!admitted && rejected!=rejected_simple_hle_hooks_.end() && unsigned(rejected->second)==unsigned(kind)) {
          hle_decline_entry_=cpu_.pc;++hle_guest_declines_;return false;
        }
        if(native_only_) {
          std::fprintf(stderr,"GEKKOAOT_NATIVE_ONLY_UNSUPPORTED subsystem=sdk operation=stale-aot-token token=%08x pc=%08x action=recompile-with-current-manifest\n",
                       kStaticDirectSimpleBase|unsigned(kind),cpu_.pc);
          SetFault(FaultKind::HleFailure,cpu_.pc,unsigned(kind));
        }
        else {hle_decline_entry_=cpu_.pc;++hle_guest_declines_;}
        return false;
      }
  return true;
}

bool HostRuntime::AdmitNativeOsKind(GekkoAOT::NativeOS::Kind kind) {
  const auto hook=os_hooks_.find(cpu_.pc);
  if(hook!=os_hooks_.end() && hook->second==kind && NativeHookCodeMatches(cpu_.pc))return true;
  if(native_only_)SetFault(FaultKind::HleFailure,cpu_.pc,unsigned(kind));
  else {hle_decline_entry_=cpu_.pc;++hle_guest_declines_;}
  return false;
}
void HostRuntime::RecordNativeSdkEntry(unsigned slot,std::uint32_t address,bool accepted) {
  if(!runtime_profile_enabled_)return;
  auto& calls=native_sdk_entry_calls_[(std::uint64_t(address)<<32u)|slot];
  if(accepted)++calls.accepted;else ++calls.declined;
}
bool HostRuntime::DispatchGameLibrary(unsigned id) {
  static const bool enabled = RuntimeEnvBool("GEKKOAOT_GAME_LIBRARY_HLE", true);
  if (!enabled) return false;
  if (id == (GekkoAOT::Native::EaMpeg::kTokenGcIdct & 0xffu)) return HleEaGcIdct();
  if (id == (GekkoAOT::Native::MohFrontline::kTokenParticles & 0xffu)) return HleMohParticles();
  if (id == (GekkoAOT::Native::MohFrontline::kTokenMorph & 0xffu)) return HleMohMorph();
  if (id == (GekkoAOT::Native::MohFrontline::kTokenRotYzx & 0xffu)) return HleMohRotYzx();
  if (id == (GekkoAOT::Native::MohFrontline::kTokenLink & 0xffu)) return HleMohLink();
  if (id == (GekkoAOT::Native::MohFrontline::kTokenSin & 0xffu)) return HleMohTrig(false);
  if (id == (GekkoAOT::Native::MohFrontline::kTokenCos & 0xffu)) return HleMohTrig(true);
  return false;
}

// Medal of Honor: Frontline's particle loop, entered after the first
// GXBegin of CParticleSystem::RenderSystem. Everything it needs is in registers
// and stack slots the guest just computed; afterwards only the epilogue runs,
// which restores every non-volatile register from the stack.
bool HostRuntime::HleMohParticles() {
  namespace M = GekkoAOT::Native::MohFrontline;
  static const bool enabled = RuntimeEnvBool("GEKKOAOT_MOH_NATIVE_PARTICLES", true);
  static const bool shadow = RuntimeEnvBool("GEKKOAOT_MOH_PARTICLES_SHADOW", false);
  if (!enabled || cpu_.pc != M::kHookPc) return false;
  if (moh_particles_epoch_ != fixed_exec_route_epoch_) {
    moh_particles_epoch_ = fixed_exec_route_epoch_;
    const auto* code = memory_.Resolve(M::kRenderSystemStart, M::kRenderSystemSize);
    moh_particles_admitted_ =
        code && M::CodeHash(code, M::kRenderSystemSize) == M::kRenderSystemHash;
    std::fprintf(stderr, "GEKKOAOT_MOH_NATIVE_PARTICLES_V391=%u shadow=%u\n",
                 moh_particles_admitted_ ? 1u : 0u, shadow ? 1u : 0u);
  }
  if (!moh_particles_admitted_) return false;
  static const bool decline_stats = RuntimeEnvBool("GEKKOAOT_MOH_DECLINE_STATS", false);
  static std::uint64_t declines[8] = {};
  const auto decline = [&](unsigned reason) {
    if (decline_stats && (++declines[reason] & 0xfffu) == 1u)
      std::fprintf(stderr,
                   "GEKKOAOT_MOH_DECLINE_V391 fp=%llu fifo=%llu cp=%llu gx=%llu stack=%llu link=%u read=%u "
                   "bp=%u drain=%u distance=%u fpscr=%08x\n",
                   static_cast<unsigned long long>(declines[0]), static_cast<unsigned long long>(declines[1]),
                   static_cast<unsigned long long>(declines[2]), static_cast<unsigned long long>(declines[3]),
                   static_cast<unsigned long long>(declines[4]), cp_.GPLinkEnabled() ? 1u : 0u,
                   cp_.GPReadEnabled() ? 1u : 0u, cp_.BreakpointActiveForHost() ? 1u : 0u,
                   cp_.RegisterDrainPending() ? 1u : 0u, unsigned(cp_.FifoReadWriteDistance()), cpu_.fpscr);
    return false;
  };
  // Round-to-nearest, no enabled FP exception, FPU owned: the guest just ran
  // FP code on this thread, so anything else is a state this path never saw.
  if (!(cpu_.msr & 0x2000u) || (cpu_.fpscr & 0xfbu) != 0u) return decline(0u);
  if (!native_gx_.Ready() || !CpuFifoTargetsGpu() || cpu_fifo_gather_size_ != 0u) return decline(1u);
  if ((pi_.FifoBase() != 0u || pi_.FifoEnd() != 0u) &&
      !GekkoAOT::NativeHW::CP::CanDirectSubmit(
          native_hw_.CP(), native_gx_.Ready(), cp_.GPLinkEnabled(), cp_.GPReadEnabled(),
          cp_.BreakpointActiveForHost() || cp_.RegisterDrainPending(), cp_.FifoReadWriteDistance(),
          cp_.FifoReadPointer() == cp_.FifoWritePointer()))
    return decline(2u);
  const auto read_word = [&](std::uint32_t address, std::uint32_t* value) {
    const auto* bytes = memory_.Resolve(address, 4u);
    if (!bytes) return false;
    *value = ReadGuestBE32(bytes);
    return true;
  };
  // Later GXBegin calls in the loop only write their header: the first one
  // cleared the dirty state and nothing between them writes BP registers.
  std::uint32_t gx = 0u, gx_dirty = 0u, gx_sent = 0u;
  const std::uint32_t seed_address = cpu_.gpr[13] - 0x6f14u;
  const std::uint32_t counter_address = cpu_.gpr[13] - 0x6f1cu;
  M::ParticleInput in{};
  if (!read_word(cpu_.gpr[13] - 0x7468u, &gx) || !read_word(gx + 0x4f4u, &gx_dirty) ||
      !read_word(gx, &gx_sent) || gx_dirty != 0u || gx_sent == 0u ||
      !read_word(seed_address, &in.seed) || !read_word(counter_address, &in.counter))
    return decline(3u);
  const auto* stack = memory_.Resolve(cpu_.gpr[1], 0x490u);
  if (!stack) return decline(4u);
  const auto single = [&](unsigned offset) {
    const std::uint32_t bits = ReadGuestBE32(stack + offset);
    float value;
    std::memcpy(&value, &bits, 4u);
    return value;
  };
  const auto widened = [&](unsigned offset) {
    const std::uint64_t bits =
        (std::uint64_t(ReadGuestBE32(stack + offset)) << 32u) | ReadGuestBE32(stack + offset + 4u);
    double value;
    std::memcpy(&value, &bits, 8u);
    return float(value);
  };
  const auto reg = [&](unsigned index) { return float(cpu_.fpr[index]); };
  in.rate = std::int32_t(cpu_.gpr[25]);
  in.delay = std::int32_t(cpu_.gpr[27]);
  in.frame = std::int32_t(cpu_.gpr[24]);
  in.nverts = std::uint16_t(cpu_.gpr[26]);
  in.rotate = cpu_.gpr[30] != 0u;
  in.t = reg(24); in.age = reg(25); in.one = reg(22); in.zero = reg(21); in.scale = reg(31);
  in.fwd[0] = reg(29); in.fwd[1] = reg(28); in.fwd[2] = reg(27);
  in.sys_age = single(0x374u); in.sys_age_step = single(0x370u);
  in.t_step = single(0x378u); in.age_step = single(0x37cu); in.last_emit = single(0x384u);
  in.half = widened(0x388u); in.growth = single(0x34u);
  for (unsigned i = 0; i < 3u; ++i) {
    in.sys_vel[i] = single(0x178u + 4u * i);
    in.sys_accel[i] = single(0x168u + 4u * i);
    in.accel[i] = single(0x158u + 4u * i);
    in.pos0[i] = single(0x1d8u + 4u * i);
    in.pos_range[i] = widened(0x390u + 8u * i);
    in.vel0[i] = single(0x1a8u + 4u * i);
    in.vel_range[i] = widened(0x3a8u + 8u * i);
    in.right[i] = single(0x108u + 4u * i);
    in.up[i] = single(0xf8u + 4u * i);
  }
  for (unsigned c = 0; c < 4u; ++c) {
    in.start_range[c] = widened(0x3c0u + 0x20u * c);
    in.start[c] = widened(0x3c8u + 0x20u * c);
    in.end_range[c] = widened(0x3d0u + 0x20u * c);
    in.end[c] = widened(0x3d8u + 0x20u * c);
  }
  in.width_range = widened(0x440u); in.width0 = single(0x208u);
  in.height_range = widened(0x448u); in.height0 = single(0x20cu);
  in.rot_range = single(0x380u); in.rot0 = single(0x20u);

  if (moh_particles_buffer_.empty()) moh_particles_buffer_.resize(0x10000u);
#if defined(__SSE2__) || defined(_M_X64)
  const unsigned mxcsr = _mm_getcsr();
  _mm_setcsr(0x1f80u);
#endif
  M::ParticleResult result{};
  if (shadow) {
    // The guest loop's own later GXBegin calls come back here: the stream
    // computed at its first one already covers them.
    if (!MohShadowArm()) {
#if defined(__SSE2__) || defined(_M_X64)
      _mm_setcsr(mxcsr);
#endif
      return false;
    }
    moh_shadow_expected_.clear();
    M::EmitParticles(in, &result, moh_particles_buffer_.data(), moh_particles_buffer_.size(),
                     [](void* context, const std::uint8_t* bytes, std::size_t size) {
                       auto& expected = *static_cast<std::vector<std::uint8_t>*>(context);
                       expected.insert(expected.end(), bytes, bytes + size);
                     },
                     &moh_shadow_expected_);
#if defined(__SSE2__) || defined(_M_X64)
    _mm_setcsr(mxcsr);
#endif
    moh_shadow_position_ = 0u;
    moh_shadow_line_start_ = gpu_fifo_gather_size_;
    moh_shadow_active_ = !moh_shadow_expected_.empty();
    moh_shadow_particles_ += result.particles;
    moh_shadow_rotated_ += result.rotated;
    if ((++moh_shadow_calls_ & 0x3ffu) == 0u)
      std::fprintf(stderr,
                   "GEKKOAOT_MOH_PARTICLES_SHADOW_V391=1 calls=%llu particles=%llu rotated=%llu "
                   "bytes=%llu mismatched=%llu\n",
                   static_cast<unsigned long long>(moh_shadow_calls_),
                   static_cast<unsigned long long>(moh_shadow_particles_),
                   static_cast<unsigned long long>(moh_shadow_rotated_),
                   static_cast<unsigned long long>(moh_shadow_bytes_),
                   static_cast<unsigned long long>(moh_shadow_mismatches_));
    return false;
  }
  const auto before = gpu_fifo_bursts_;
  M::EmitParticles(in, &result, moh_particles_buffer_.data(), moh_particles_buffer_.size(),
                   [](void* context, const std::uint8_t* bytes, std::size_t size) {
                     auto* self = static_cast<HostRuntime*>(context);
                     (void)self->QueueGpuFifoStream(bytes, size, M::kHookPc);
                   },
                   this);
#if defined(__SSE2__) || defined(_M_X64)
  _mm_setcsr(mxcsr);
#endif
  if (gpu_fifo_bursts_ != before) BumpHostWriteEpoch(&cpu_);
  CountHostEffect();
  if (!memory_.Write32(seed_address, result.seed) ||
      !memory_.Write32(counter_address, result.counter)) {
    SetFault(FaultKind::HleFailure, cpu_.pc, 0u);
    return true;
  }
  // The guest cycle model keeps seeing the loop it replaced.
  cpu_.downcount -= std::int64_t(result.particles * 335u + result.rotated * 155u + result.batches * 100u);
  cpu_.pc = M::kExitPc;
  return true;
}

// _matrix_rot_yzx, bit-identical to the guest routine (see HleMohTrig).
bool HostRuntime::HleMohRotYzx() {
  namespace M = GekkoAOT::Native::MohFrontline;
  static const bool enabled = RuntimeEnvBool("GEKKOAOT_MOH_NATIVE_TRIG", true);
  if (!enabled) return false;
  if (moh_rot_epoch_ != fixed_exec_route_epoch_) {
    moh_rot_epoch_ = fixed_exec_route_epoch_;
    const auto* rot = memory_.Resolve(M::kRotYzx, M::kRotYzxSize);
    const auto* trig = memory_.Resolve(M::kTrigStart, M::kTrigSize);
    moh_rot_admitted_ = rot && trig && M::CodeHash(rot, M::kRotYzxSize) == M::kRotYzxHash &&
                        M::CodeHash(trig, M::kTrigSize) == M::kTrigHash;
    std::fprintf(stderr, "GEKKOAOT_MOH_NATIVE_ROT_V391=%u\n", moh_rot_admitted_ ? 1u : 0u);
  }
  if (!moh_rot_admitted_) return false;
  if (!(cpu_.msr & 0x2000u) || (cpu_.fpscr & 0xfbu) != 0u) return false;
  auto* matrix = memory_.Resolve(cpu_.gpr[3], 0x30u);
  if (!matrix) return false;
#if defined(__SSE2__) || defined(_M_X64)
  const unsigned mxcsr = _mm_getcsr();
  _mm_setcsr(0x1f80u);
#endif
  M::MatrixRotYzx(matrix, std::int32_t(cpu_.gpr[4]), std::int32_t(cpu_.gpr[5]), std::int32_t(cpu_.gpr[6]));
#if defined(__SSE2__) || defined(_M_X64)
  _mm_setcsr(mxcsr);
#endif
  CountHostEffect();
  cpu_.downcount -= 620;
  cpu_.pc = cpu_.lr & ~3u;
  return true;
}

// The vertex loop of DMMorphBlendDisplayLists (morphed faces). Registers at
// the hook: r4 list, r0 list size, r10 output buffer {data, -, used}, r29 mesh
// parts, r30 mesh, r9 active targets, r1+0x18 {target, weight}[].
bool HostRuntime::HleMohMorph() {
  namespace M = GekkoAOT::Native::MohFrontline;
  static const bool enabled = RuntimeEnvBool("GEKKOAOT_MOH_NATIVE_MORPH", true);
  static const bool shadow = RuntimeEnvBool("GEKKOAOT_MOH_PARTICLES_SHADOW", false);
  if (!enabled || cpu_.pc != M::kMorphHookPc) return false;
  if (moh_morph_epoch_ != fixed_exec_route_epoch_) {
    moh_morph_epoch_ = fixed_exec_route_epoch_;
    const auto* code = memory_.Resolve(M::kMorphStart, M::kMorphSize);
    moh_morph_admitted_ = code && M::CodeHash(code, M::kMorphSize) == M::kMorphHash;
    std::fprintf(stderr, "GEKKOAOT_MOH_NATIVE_MORPH_V391=%u shadow=%u\n",
                 moh_morph_admitted_ ? 1u : 0u, shadow ? 1u : 0u);
  }
  if (!moh_morph_admitted_) return false;
  if (!(cpu_.msr & 0x2000u) || (cpu_.fpscr & 0xfbu) != 0u) return false;
  const auto mem1 = memory_.Mem1();
  const auto ram_size = std::uint32_t(std::min<std::size_t>(mem1.size(), 0x01800000u));
  auto* ram = mem1.data();
  const auto offset = [&](std::uint32_t address, std::uint32_t* out) {
    if ((address >> 24u) < 0x80u || (address >> 24u) > 0x81u) return false;
    *out = address & 0x01ffffffu;
    return *out + 4u <= ram_size;
  };
  const auto word = [&](std::uint32_t address, std::uint32_t* out) {
    std::uint32_t at = 0u;
    if (!offset(address, &at)) return false;
    *out = ReadGuestBE32(ram + at);
    return true;
  };
  if (shadow && !moh_morph_expected_.empty()) {
    const auto* actual = ram + moh_morph_expected_output_;
    for (std::size_t i = 0; i < moh_morph_expected_.size(); ++i) {
      if (actual[i] == moh_morph_expected_[i]) continue;
      static unsigned logged = 0u;
      if (logged++ < 16u)
        std::fprintf(stderr, "GEKKOAOT_MOH_MORPH_DIFF_V391 call=%llu offset=%zu size=%zu expected=%02x actual=%02x\n",
                     static_cast<unsigned long long>(moh_morph_calls_), i, moh_morph_expected_.size(),
                     moh_morph_expected_[i], actual[i]);
      ++moh_morph_mismatches_;
    }
    moh_morph_bytes_ += moh_morph_expected_.size();
    moh_morph_expected_.clear();
    if ((++moh_morph_calls_ & 0xffu) == 0u)
      std::fprintf(stderr, "GEKKOAOT_MOH_MORPH_SHADOW_V391=1 calls=%llu bytes=%llu mismatched=%llu\n",
                   static_cast<unsigned long long>(moh_morph_calls_),
                   static_cast<unsigned long long>(moh_morph_bytes_),
                   static_cast<unsigned long long>(moh_morph_mismatches_));
  }
  M::MorphInput in{};
  in.targets = cpu_.gpr[9];
  if (in.targets > M::kMorphMaxTargets) return false;
  std::uint32_t data = 0u, used = 0u, positions = 0u, normals = 0u;
  const std::uint32_t buffer = cpu_.gpr[10], mesh = cpu_.gpr[30];
  if (!offset(cpu_.gpr[4], &in.list) || !word(buffer, &data) || !word(buffer + 8u, &used) ||
      !word(mesh + 0x60u, &positions) || !word(mesh + 0x68u, &normals) ||
      !offset(data + used, &in.output) || !offset(positions, &in.positions) ||
      !offset(normals, &in.normals))
    return false;
  in.list_end = in.list + cpu_.gpr[0];
  for (std::uint32_t t = 0; t < in.targets; ++t) {
    std::uint32_t target = 0u, weight = 0u, lists = 0u, target_list = 0u;
    if (!word(cpu_.gpr[1] + 0x18u + t * 8u, &target) ||
        !word(cpu_.gpr[1] + 0x1cu + t * 8u, &weight) ||
        !word(cpu_.gpr[29] + target * 0x38u + 0x30u, &lists) || !word(lists, &target_list) ||
        !offset(target_list, &in.target_data[t]))
      return false;
    std::memcpy(&in.weight[t], &weight, 4u);
  }
  std::uint32_t written = 0u;
  std::uint64_t vertices = 0u;
#if defined(__SSE2__) || defined(_M_X64)
  const unsigned mxcsr = _mm_getcsr();
  _mm_setcsr(0x1f80u);
#endif
  bool done = false;
  if (shadow) {
    // A private copy of MEM1 would be absurd: the blend reads the list and the
    // mesh and writes only the output span, so run it on a scratch output.
    static std::vector<std::uint8_t> scratch;
    M::MorphInput dry = in;
    const auto saved_output = in.output;
    std::uint32_t measured = 0u;
    // First the size, from the same walk the guest does.
    std::uint64_t cursor = in.list, total = 0u, primitives = 0u;
    while (cursor + 3u <= ram_size && cursor != in.list_end && ram[cursor] != 0u) {
      const std::uint32_t count = (std::uint32_t(ram[cursor + 1u]) << 8u) | ram[cursor + 2u];
      cursor += 3u + std::uint64_t(count) * 7u;
      total += count;
      ++primitives;
    }
    measured = std::uint32_t(primitives * 3u + total * 15u);
    if (saved_output + std::uint64_t(measured) <= ram_size) {
      scratch.assign(ram + saved_output, ram + saved_output + measured);
      if (M::MorphBlend(ram, ram_size, dry, &written, &vertices)) {
        moh_morph_expected_.assign(ram + saved_output, ram + saved_output + written);
        moh_morph_expected_output_ = saved_output;
      }
      std::memcpy(ram + saved_output, scratch.data(), measured);
    }
  } else {
    done = M::MorphBlend(ram, ram_size, in, &written, &vertices);
  }
#if defined(__SSE2__) || defined(_M_X64)
  _mm_setcsr(mxcsr);
#endif
  if (!done) return false;
  if (!memory_.Write32(buffer + 8u, used + written)) {
    SetFault(FaultKind::HleFailure, cpu_.pc, 0u);
    return true;
  }
  CountHostEffect();
  cpu_.downcount -= std::int64_t(vertices * (45u + 130u * in.targets));
  cpu_.pc = M::kMorphExitPc;
  return true;
}

// MSL sin/cos, bit-identical to the guest routines (16M values compared
// against the guest code). Skeleton animation calls them 6 times per
// bone. Both are leaves, so the native superchain keeps running.
bool HostRuntime::HleMohTrig(bool cosine) {
  namespace M = GekkoAOT::Native::MohFrontline;
  static const bool enabled = RuntimeEnvBool("GEKKOAOT_MOH_NATIVE_TRIG", true);
  if (!enabled) return false;
  if (moh_trig_epoch_ != fixed_exec_route_epoch_) {
    moh_trig_epoch_ = fixed_exec_route_epoch_;
    const auto* code = memory_.Resolve(M::kTrigStart, M::kTrigSize);
    moh_trig_admitted_ = code && M::CodeHash(code, M::kTrigSize) == M::kTrigHash;
    std::fprintf(stderr, "GEKKOAOT_MOH_NATIVE_TRIG_V391=%u\n", moh_trig_admitted_ ? 1u : 0u);
  }
  if (!moh_trig_admitted_) return false;
  if (!(cpu_.msr & 0x2000u) || (cpu_.fpscr & 0xfbu) != 0u) return false;
  const double x = cpu_.fpr[1];
  std::uint64_t bits;
  std::memcpy(&bits, &x, 8u);
  const std::uint32_t high = std::uint32_t(bits >> 32u) & 0x7fffffffu;
  // Huge angles need __kernel_rem_pio2: the guest routine handles them.
  if (high > 0x413921fbu && high < 0x7ff00000u) return false;
#if defined(__SSE2__) || defined(_M_X64)
  const unsigned mxcsr = _mm_getcsr();
  _mm_setcsr(0x1f80u);
#endif
  cpu_.fpr[1] = cosine ? M::MslCos(x) : M::MslSin(x);
#if defined(__SSE2__) || defined(_M_X64)
  _mm_setcsr(mxcsr);
#endif
  CountHostEffect();
  cpu_.downcount -= 60;
  cpu_.pc = cpu_.lr & ~3u;
  return true;
}

// False while the previous stream is still being written by the guest.
bool HostRuntime::MohShadowArm() {
  if (!moh_shadow_active_) return true;
  if (moh_shadow_expected_.size() - moh_shadow_position_ > gpu_fifo_gather_size_) return false;
  // Its tail is still in the gather line.
  moh_shadow_line_start_ = 0u;
  MohParticlesShadowLine();
  moh_shadow_active_ = false;
  return true;
}

// The matrix loop of CPartBin::Link, for one display list of a skinned
// model. Registers at the hook: r20 the list {.., u16 count at 6, u8 indices at
// 8}, r28 the bone matrices, r1+0xb4 the temporary normal matrix. No later code
// reads the registers the loop leaves behind but r21/r22, kept consistent.
bool HostRuntime::HleMohLink() {
  namespace M = GekkoAOT::Native::MohFrontline;
  static const bool enabled = RuntimeEnvBool("GEKKOAOT_MOH_NATIVE_LINK", true);
  static const bool shadow = RuntimeEnvBool("GEKKOAOT_MOH_PARTICLES_SHADOW", false);
  if (!enabled || cpu_.pc != M::kLinkHookPc) return false;
  if (moh_link_epoch_ != fixed_exec_route_epoch_) {
    moh_link_epoch_ = fixed_exec_route_epoch_;
    const auto matches = [&](std::uint32_t start, std::uint32_t size, std::uint64_t hash) {
      const auto* code = memory_.Resolve(start, size);
      return code && M::CodeHash(code, size) == hash;
    };
    moh_link_admitted_ = matches(GEKKOAOT_MOH_LINK_START, GEKKOAOT_MOH_LINK_SIZE, GEKKOAOT_MOH_LINK_HASH) &&
                         matches(GEKKOAOT_MOH_INVXPOSE_START, GEKKOAOT_MOH_INVXPOSE_SIZE, GEKKOAOT_MOH_INVXPOSE_HASH) &&
                         matches(GEKKOAOT_MOH_GXMTX_START, GEKKOAOT_MOH_GXMTX_SIZE, GEKKOAOT_MOH_GXMTX_HASH);
    std::fprintf(stderr, "GEKKOAOT_MOH_NATIVE_LINK_V392=%u shadow=%u\n", moh_link_admitted_ ? 1u : 0u, shadow ? 1u : 0u);
  }
  if (!moh_link_admitted_) return false;
  constexpr std::uint32_t kHid2PairedLoadStore = 0x80000000u | 0x20000000u;  // LSQE | PSE
  if (!(cpu_.msr & 0x2000u) || (cpu_.fpscr & 0xfbu) != 0u || cpu_.gqr[0] != 0u ||
      (cpu_.hid2 & kHid2PairedLoadStore) != kHid2PairedLoadStore)
    return false;
  if (!native_gx_.Ready() || !CpuFifoTargetsGpu() || cpu_fifo_gather_size_ != 0u) return false;
  if ((pi_.FifoBase() != 0u || pi_.FifoEnd() != 0u) &&
      !GekkoAOT::NativeHW::CP::CanDirectSubmit(
          native_hw_.CP(), native_gx_.Ready(), cp_.GPLinkEnabled(), cp_.GPReadEnabled(),
          cp_.BreakpointActiveForHost() || cp_.RegisterDrainPending(), cp_.FifoReadWriteDistance(),
          cp_.FifoReadPointer() == cp_.FifoWritePointer()))
    return false;
  const auto* list = memory_.Resolve(cpu_.gpr[20], 8u);
  if (!list) return false;
  const unsigned count = (unsigned(list[6]) << 8u) | list[7];
  if (count == 0u || count > 64u) return false;
  const auto* indices = memory_.Resolve(cpu_.gpr[20] + 8u, count);
  const auto* matrices = memory_.Resolve(cpu_.gpr[28], 256u * 0x30u);
  auto* normal = memory_.Resolve(cpu_.gpr[1] + 0xb4u, 0x30u);
  if (!indices || !matrices || !normal) return false;
  if (moh_particles_buffer_.empty()) moh_particles_buffer_.resize(0x10000u);
#if defined(__SSE2__) || defined(_M_X64)
  const unsigned mxcsr = _mm_getcsr();
  _mm_setcsr(0x1f80u);
#endif
  if (shadow) {
    std::uint8_t scratch[0x30];
    std::memcpy(scratch, normal, 0x30u);
    const bool armed = MohShadowArm();
    if (armed) {
      moh_shadow_expected_.resize(count * M::kLinkBytesPerBone);
      M::EmitListMatrices(matrices, indices, count, scratch, moh_shadow_expected_.data());
      moh_shadow_position_ = 0u;
      moh_shadow_line_start_ = gpu_fifo_gather_size_;
      moh_shadow_active_ = true;
      ++moh_shadow_calls_;
    }
#if defined(__SSE2__) || defined(_M_X64)
    _mm_setcsr(mxcsr);
#endif
    return false;
  }
  const auto size = M::EmitListMatrices(matrices, indices, count, normal, moh_particles_buffer_.data());
#if defined(__SSE2__) || defined(_M_X64)
  _mm_setcsr(mxcsr);
#endif
  const auto before = gpu_fifo_bursts_;
  (void)QueueGpuFifoStream(moh_particles_buffer_.data(), size, M::kLinkHookPc);
  if (gpu_fifo_bursts_ != before) BumpHostWriteEpoch(&cpu_);
  CountHostEffect();
  cpu_.gpr[19] = indices[count - 1u];
  cpu_.gpr[21] = count;
  cpu_.gpr[22] = count * 3u;
  cpu_.downcount -= std::int64_t(count * 125u);
  cpu_.pc = M::kLinkExitPc;
  return true;
}

void HostRuntime::MohParticlesShadowLine() {
  const std::size_t room = gpu_fifo_gather_.size() - moh_shadow_line_start_;
  const std::size_t left = moh_shadow_expected_.size() - moh_shadow_position_;
  const std::size_t count = std::min(room, left);
  const auto* actual = gpu_fifo_gather_.data() + moh_shadow_line_start_;
  const auto* expected = moh_shadow_expected_.data() + moh_shadow_position_;
  for (std::size_t i = 0; i < count; ++i) {
    if (actual[i] == expected[i]) continue;
    static unsigned logged = 0u;
    if (logged++ < 24u) {
      // Stream offset, then the 4 bytes around it on both sides.
      const std::size_t at = moh_shadow_position_ + i;
      std::fprintf(stderr,
                   "GEKKOAOT_MOH_PARTICLES_DIFF_V391 call=%llu offset=%zu size=%zu expected=%02x actual=%02x\n",
                   static_cast<unsigned long long>(moh_shadow_calls_), at,
                   moh_shadow_expected_.size(), expected[i], actual[i]);
    }
    ++moh_shadow_mismatches_;
  }
  moh_shadow_bytes_ += count;
  moh_shadow_position_ += count;
  moh_shadow_line_start_ = 0u;
  if (moh_shadow_position_ == moh_shadow_expected_.size()) moh_shadow_active_ = false;
}

// EA MPEG-2 GC_IDCT(short* block): see ea_mpeg_hle.h for the exactness
// argument. Every precondition the guest routine depends on is checked; a
// mismatch declines and the AOT body runs instead.
bool HostRuntime::HleEaGcIdct() {
  namespace Ea = GekkoAOT::Native::EaMpeg;
  const std::uint32_t pc = cpu_.pc;
  if (!ea_idct_valid_ || ea_idct_pc_ != pc || ea_idct_epoch_ != fixed_exec_route_epoch_) {
    Ea::IdctPlan plan;
    ea_idct_valid_ = Ea::MatchGcIdct(pc, [&](std::uint32_t address, std::uint32_t* word) {
      const auto* bytes = memory_.Resolve(address, 4u);
      if (!bytes) return false;
      *word = ReadGuestBE32(bytes);
      return true;
    }, &plan);
    ea_idct_pc_ = pc;
    ea_idct_epoch_ = fixed_exec_route_epoch_;
    ea_idct_table_ = plan.table;
    ea_idct_gqr7_ = plan.gqr7;
    static unsigned logs = 0;
    if (logs++ < 4u)
      std::fprintf(stderr, "GEKKOAOT_GAME_LIBRARY_HLE_V387=1 routine=ea-gc-idct pc=%08x verified=%u table=%08x gqr7=%08x\n",
                   pc, ea_idct_valid_ ? 1u : 0u, plan.table, plan.gqr7);
  }
  if (!ea_idct_valid_) return false;
  // FP available, round-to-nearest, no enabled FP exception (NI is harmless:
  // no value here can be subnormal), float GQR0 and paired loads/stores on.
  constexpr std::uint32_t kHid2PairedLoadStore = 0x80000000u | 0x20000000u;  // LSQE | PSE
  if (!(cpu_.msr & 0x2000u) || (cpu_.fpscr & 0xfbu) != 0u || cpu_.gqr[0] != 0u ||
      (cpu_.hid2 & kHid2PairedLoadStore) != kHid2PairedLoadStore)
    return false;
  auto* block = memory_.Resolve(cpu_.gpr[3], 128u);
  const auto* table = memory_.Resolve(ea_idct_table_, 32u);
  if (!block || !table) return false;
  float k[8];
  for (unsigned i = 0; i < 8u; ++i) {
    const std::uint32_t bits = ReadGuestBE32(table + 4u * i);
    std::memcpy(&k[i], &bits, 4u);
    if (!std::isfinite(k[i])) return false;
  }
  std::int16_t coefficients[64];
  for (unsigned i = 0; i < 64u; ++i)
    coefficients[i] = std::int16_t((unsigned(block[2u * i]) << 8) | block[2u * i + 1u]);
  Ea::GcIdct(coefficients, k, ea_idct_gqr7_);
  for (unsigned i = 0; i < 64u; ++i) {
    const auto value = std::uint16_t(coefficients[i]);
    block[2u * i] = std::uint8_t(value >> 8);
    block[2u * i + 1u] = std::uint8_t(value);
  }
  cpu_.gqr[7] = ea_idct_gqr7_;
  cpu_.pc = cpu_.lr;
  ++ea_idct_calls_;
  ++hle_calls_;
  return true;
}

bool HostRuntime::TryNativeStrcmpFast() {
  static const bool strcmp_fast_path =
      RuntimeEnvBool("GEKKOAOT_STRCMP_FAST_PATH", true) &&
      !DisabledSimpleServices().test(static_cast<unsigned>(SimpleHleKind::Strcmp));
  // GEKKOAOT_STRCMP_CALLERS=1 (diagnostic): top return addresses.
  static const bool strcmp_callers = RuntimeEnvBool("GEKKOAOT_STRCMP_CALLERS", false);
  if (strcmp_callers) {
    static std::unordered_map<std::uint32_t, std::uint64_t> callers;
    static std::uint64_t total = 0;
    ++callers[cpu_.lr];
    if ((++total & 0x3fffffu) == 0u) {
      std::vector<std::pair<std::uint64_t, std::uint32_t>> top;
      for (const auto& [lr, n] : callers) top.emplace_back(n, lr);
      std::sort(top.begin(), top.end(), std::greater<>());
      std::string line;
      for (std::size_t i = 0; i < top.size() && i < 12u; ++i) {
        char item[48];
        std::snprintf(item, sizeof(item), " %08x:%llu", top[i].second, static_cast<unsigned long long>(top[i].first));
        line += item;
      }
      std::fprintf(stderr, "GEKKOAOT_STRCMP_CALLERS total=%llu%s\n", static_cast<unsigned long long>(total), line.c_str());
    }
  }
  if (strcmp_fast_path) {
    const std::uint32_t a = cpu_.gpr[3], b = cpu_.gpr[4];
    const auto& mem1 = memory_.Mem1();
    if ((a & 0xc0000000u) == 0x80000000u && (b & 0xc0000000u) == 0x80000000u) {
      const std::uint32_t oa = a & 0x3fffffffu, ob = b & 0x3fffffffu;
      if (oa < mem1.size() && ob < mem1.size()) {
        const std::size_t limit = mem1.size() - std::max(oa, ob);
        const std::uint8_t* pa = mem1.data() + oa;
        const std::uint8_t* pb = mem1.data() + ob;
        for (std::size_t i = 0; i < limit; ++i) {
          const std::uint8_t ca = pa[i], cb = pb[i];
          if (ca != cb || ca == 0u) {
            cpu_.gpr[3] = static_cast<std::uint32_t>(int(ca) - int(cb));
            cpu_.pc = cpu_.lr;
            ++hle_calls_;
            ++simple_hle_kind_calls_[static_cast<std::size_t>(SimpleHleKind::Strcmp)];
            return true;
          }
        }
      }
    }
  }
  return false;
}

template<SimpleHleKind Kind>
std::uint32_t HostRuntime::NativeSimpleService(CPUState* cpu) {
  auto* self=Self(cpu);if(!self)return 0;
  const auto slot=static_cast<unsigned>(Kind);
  const auto entry=cpu->pc;
  if(!self->AdmitNativeSdkKind(Kind)){++self->native_sdk_direct_declines_[slot];self->RecordNativeSdkEntry(slot,entry,false);return 0;}
  if constexpr(Kind==SimpleHleKind::Strcmp)if(self->TryNativeStrcmpFast()) {
    ++self->native_sdk_direct_calls_[slot];self->RecordNativeSdkEntry(slot,entry,true);return 1u;
  }
  const bool handled=self->DispatchSimpleHleNative<static_cast<int>(Kind)>(Kind,cpu->pc);
  if(handled){
    ++self->native_sdk_direct_calls_[static_cast<unsigned>(Kind)];
    if(self->track_host_effects_)++self->host_effects_;
    const auto token=kStaticDirectSimpleBase|unsigned(Kind);
    if(!IsChainTransparentLeaf(token) &&
       !(self->gx_matrix_chain_enabled_ && IsChainTransparentGxMatrix(token)))BumpHostWriteEpoch(cpu);
  }
  if(!handled)++self->native_sdk_direct_declines_[slot];
  self->RecordNativeSdkEntry(slot,entry,handled);
  return handled?1u:0u;
}
template<GekkoAOT::NativeOS::Kind Kind>
std::uint32_t HostRuntime::NativeOsService(CPUState* cpu) {
  auto* self=Self(cpu);if(!self)return 0;
  const auto slot=static_cast<unsigned>(SimpleHleKind::Count)+static_cast<unsigned>(Kind);
  const auto entry=cpu->pc;
  if(!self->AdmitNativeOsKind(Kind)){++self->native_sdk_direct_declines_[slot];self->RecordNativeSdkEntry(slot,entry,false);return 0;}
  const bool handled=self->DispatchOsHleNative<static_cast<int>(Kind)>(Kind,cpu->pc);
  if(handled){
    ++self->native_sdk_direct_calls_[static_cast<unsigned>(SimpleHleKind::Count)+static_cast<unsigned>(Kind)];
    if(self->track_host_effects_)++self->host_effects_;
    BumpHostWriteEpoch(cpu);
  }
  if(!handled)++self->native_sdk_direct_declines_[slot];
  self->RecordNativeSdkEntry(slot,entry,handled);
  return handled?1u:0u;
}
const GekkoAOTNativeSdk& HostRuntime::NativeSdkServices() {
  static_assert(unsigned(SimpleHleKind::Count)+unsigned(GekkoAOT::NativeOS::Kind::Count)==GekkoAOTNativeSdkCount);
#define SDK_SIMPLE(kind, name, abi) static_assert(unsigned(SimpleHleKind::kind)==GekkoAOTSdkSimple_##kind);
#define SDK_OS(kind, name, abi) static_assert(unsigned(SimpleHleKind::Count)+unsigned(GekkoAOT::NativeOS::Kind::kind)==GekkoAOTSdkOs_##kind);
#include "sdk/native_services.def"
#undef SDK_SIMPLE
#undef SDK_OS
  static const GekkoAOTNativeSdk services{2u,sizeof(GekkoAOTNativeSdk),GekkoAOTNativeSdkCount,0u,gekkoaot_native_sdk_registry_hash(),{
#define SDK_SIMPLE(kind, name, abi) &HostRuntime::NativeSimpleService<SimpleHleKind::kind>,
#define SDK_OS(kind, name, abi) &HostRuntime::NativeOsService<GekkoAOT::NativeOS::Kind::kind>,
#include "sdk/native_services.def"
#undef SDK_SIMPLE
#undef SDK_OS
  }};
  return services;
}

} // namespace GekkoAOT::Native
