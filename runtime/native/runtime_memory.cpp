// SPDX-License-Identifier: GPL-3.0-or-later
// HostRuntime: guest memory, paged VM, MMIO and EFB access, instruction fallback.
#include "native/runtime_internal.h"

namespace GekkoAOT::Native {

void HostRuntime::RefreshNativeInput(bool poll_si) {
#if defined(GEKKOAOT_NATIVE_SDL_INPUT)
  if (!native_input_ready_) return;
  GekkoAOT::Input::HostInput().Poll(native_pads_);
  ++native_input_samples_;
  ++native_pad_publications_;
  if (poll_si) {
    si_.PollNow();
    ++native_si_auto_polls_;
    SyncNativeSIInterrupt();
  }
#else
  (void)poll_si;
#endif
}

bool HostRuntime::NativeExiSelect(void* user, std::uint32_t channel,
                                  std::uint8_t chip_select) {
  auto* self = static_cast<HostRuntime*>(user);
  if (self && self->native_hw_.EXI())
    return self->native_exi_host_.Select(channel, chip_select);
  return false;
}

bool HostRuntime::NativeExiPresent(void* user, std::uint32_t channel,
                                   std::uint8_t chip_select) {
  auto* self = static_cast<HostRuntime*>(user);
  return self && self->native_hw_.EXI() &&
         self->native_exi_host_.Present(channel, chip_select);
}

bool HostRuntime::NativeExiImmediate(void* user, std::uint32_t channel,
                                     std::uint8_t chip_select,
                                     std::uint8_t direction, std::uint8_t length,
                                     std::uint32_t* data) {
  auto* self = static_cast<HostRuntime*>(user);
  return self && self->native_hw_.EXI() &&
         self->native_exi_host_.Immediate(channel, chip_select, direction, length, data);
}

bool HostRuntime::NativeExiDma(void* user, std::uint32_t channel,
                               std::uint8_t chip_select, std::uint8_t direction,
                               std::uint32_t guest_address, std::uint32_t length) {
  auto* self = static_cast<HostRuntime*>(user);
  if (!self || !self->native_hw_.EXI()) return false;
  auto* bytes = self->memory_.Resolve(guest_address, length);
  if (!bytes) return false;
  const bool handled = self->native_exi_host_.Dma(channel, chip_select,
                                                  direction, bytes, length);
  if (handled && direction == 0u)
    self->native_gx_.NotifyMemoryWrite(guest_address, length);
  return handled;
}

bool HostRuntime::NativePadPoll(void* user, std::uint32_t channel,
                                std::uint32_t* hi, std::uint32_t* lo) {
  auto* self = static_cast<HostRuntime*>(user);
  if (!self || !hi || !lo || channel >= self->native_pads_.size()) return false;
  const auto& pad = self->native_pads_[channel];
  if (!pad.connected) return false;
  *hi = PadHighWord(pad);
  *lo = PadLowWord(pad, self->native_pad_mode_[channel]);
  return true;
}

int HostRuntime::NativePadBuffer(void* user, std::uint32_t channel, std::uint8_t* buffer,
                                 std::uint32_t request_length,
                                 std::uint32_t expected_response_length) {
  auto* self = static_cast<HostRuntime*>(user);
  if (!self || !buffer || request_length == 0u || channel >= self->native_pads_.size()) return -1;

  if (GekkoAOT::NativeHW::SI::RefreshOnTransfer(
          self->native_hw_.SI(), self->native_input_ready_)) {
    const auto now = std::chrono::steady_clock::now();
    if (self->native_si_last_transfer_sample_ == std::chrono::steady_clock::time_point{} ||
        now - self->native_si_last_transfer_sample_ >= std::chrono::milliseconds(1)) {
      self->RefreshNativeInput(false);
      ++self->native_si_transfer_samples_;
      self->native_si_last_transfer_sample_ = now;
    }
  }
  // The compatibility path consumes the already-latched SI state. Sampling SDL here
  // makes the host device cadence proportional to uncapped guest PAD traffic,
  // so one physical press can be observed as several fresh samples before the
  // next GameCube hardware tick. The hardware-time sampler in
  // AdvanceRuntimeCycles() is the sole owner of host -> guest input updates.
  const auto& pad = self->native_pads_[channel];
  if (!pad.connected) return -1;

  const std::uint8_t command = buffer[0];
  if (command == 0x00u || command == 0xffu) {
    if (expected_response_length < 3u) return -1;
    buffer[0] = static_cast<std::uint8_t>(kSiGcController >> 24);
    buffer[1] = static_cast<std::uint8_t>(kSiGcController >> 16);
    buffer[2] = static_cast<std::uint8_t>(kSiGcController >> 8);
    return 3;
  }
  if (command == 0x40u) {
    if (expected_response_length < 8u) return -1;
    const std::uint32_t hi = PadHighWord(pad);
    const std::uint32_t lo = PadLowWord(pad, self->native_pad_mode_[channel]);
    for (unsigned i = 0; i < 4; ++i) {
      buffer[i] = static_cast<std::uint8_t>(hi >> (24u - i * 8u));
      buffer[4u + i] = static_cast<std::uint8_t>(lo >> (24u - i * 8u));
    }
    return 8;
  }
  if (command == 0x41u || command == 0x42u) {
    if (expected_response_length < 10u) return -1;
    buffer[0] = 0; buffer[1] = 0;
    buffer[2] = PadAxis(pad.stick_x); buffer[3] = PadAxis(pad.stick_y);
    buffer[4] = PadAxis(pad.sub_x); buffer[5] = PadAxis(pad.sub_y);
    buffer[6] = pad.left; buffer[7] = pad.right; buffer[8] = 0; buffer[9] = 0;
    return 10;
  }
  if (command == 0x1du) return 0;
  return -1;
}

void HostRuntime::NativePadDirect(void* user, std::uint32_t channel,
                                  std::uint32_t command, bool polling_enabled) {
  auto* self = static_cast<HostRuntime*>(user);
  if (!self || channel >= self->native_pad_mode_.size()) return;
  const std::uint8_t opcode = static_cast<std::uint8_t>(command >> 16);
  const std::uint8_t parameter2 = static_cast<std::uint8_t>(command >> 8);
  const std::uint8_t parameter1 = static_cast<std::uint8_t>(command);
  (void)parameter1;
  if (opcode != 0x40u) return;
  if (!polling_enabled && self->native_pad_mode_[channel] != parameter2) {
    static unsigned mode_logs = 0;
    if (mode_logs++ < 16u)
      std::fprintf(stderr, "GEKKOAOT_NATIVE_SI_PAD_MODE channel=%u mode=%u previous=%u\n",
                   channel, unsigned(parameter2), unsigned(self->native_pad_mode_[channel]));
  }
  if (!polling_enabled) self->native_pad_mode_[channel] = parameter2;
#if defined(GEKKOAOT_NATIVE_SDL_INPUT)
  if (self->native_input_ready_)
    GekkoAOT::Input::HostInput().Rumble(channel, parameter1 <= 2u ? parameter1 : 0u);
#endif
}

void HostRuntime::ClearDataTlb() {
  for (auto& set : data_tlb_)
    for (auto& way : set) way = {};
  data_tlb_stamp_ = 0;
  ClearPagedView();
  auto mru = memory_.PagedViewMru();
  std::fill(mru.begin(), mru.end(), std::uint8_t{0});
}

void HostRuntime::InvalidateDataTlb(std::uint32_t address) {
  // Gekko tlbie selects a 2-way TLB set with EA[14:19] and invalidates both
  // ways.  A 64-call sweep therefore invalidates all 128 data-TLB entries.
  auto& set = data_tlb_[(address >> 12u) & 0x3fu];
  DropPagedView(set[0]);
  DropPagedView(set[1]);
  set[0].valid = false;
  set[1].valid = false;
  ++data_tlb_invalidations_;
}

// Generated code reads resident VM-aperture translations from this
// mirror instead of calling ExternalRead/Write for every paged access. It is
// exactly the modeled DTLB: published when an entry is filled (or hit again
// after a stale drop), removed when the entry is evicted or tlbie'd, and the
// write bit appears only once the PTE C bit has been set by the slow path.
void HostRuntime::ClearPagedView() {
  auto pages = memory_.PagedViewPagesTable();
  std::fill(pages.begin(), pages.end(), 0u);
}

void HostRuntime::DropPagedView(const DataTlbEntry& entry) {
  if (!entry.valid || !AddressSpace::IsVmem(entry.ea_page) ||
      entry.vsid != (memory_.PagedViewSr() & 0x00ffffffu))
    return;
  memory_.PagedViewPagesTable()[(entry.ea_page - AddressSpace::FakeVmemBase) >> 12u] = 0u;
}

void HostRuntime::PublishPagedView(const DataTlbEntry& entry, unsigned way) {
  if (!entry.valid || !native_flat_vmem_enabled_ || !native_paged_view_enabled_ ||
      !AddressSpace::IsVmem(entry.ea_page) ||
      (cpu_.msr & (kPpcMsrPr | 0x10u)) != 0x10u || (vmem_bat_modes_ & 2u) != 0u)
    return;
  const std::uint32_t segment = cpu_.sr[AddressSpace::FakeVmemBase >> 28u];
  if ((segment & 0x80000000u) != 0u || entry.vsid != (segment & 0x00ffffffu)) return;
  if (segment != memory_.PagedViewSr()) {
    ClearPagedView();
    memory_.SetPagedViewSr(segment);
  }
  const auto mem1 = memory_.Mem1().size();
  if (mem1 < 0x1000u || entry.physical_page > mem1 - 0x1000u) return;
  // Supervisor access (PR=0 is required by the generated-code admission).
  const bool key = (segment & 0x40000000u) != 0u;
  const unsigned pp = entry.pte_lower & 3u;
  const bool readable = !key || pp != 0u;
  const bool writable = pp == 2u || (!key && pp != 3u);
  memory_.PagedViewPagesTable()[(entry.ea_page - AddressSpace::FakeVmemBase) >> 12u] =
      readable ? (entry.physical_page | 1u |
                  ((writable && (entry.pte_lower & 0x80u) != 0u) ? 2u : 0u) | (way << 2u))
               : 0u;
}

void HostRuntime::RefreshVmemBatModes() {
  constexpr std::uint32_t aperture_variable_bits = AddressSpace::RetailFakeVmemSize - 1u;
  std::uint8_t modes = 0;
  for (unsigned index = 0; index < 4; ++index) {
    const auto upper = spr_state_[536u + index * 2u];
    const auto offset_mask = ((upper & 0x1ffcu) << 15u) | 0x1ffffu;
    const auto compared_bits = ~offset_mask & ~aperture_variable_bits;
    // Only bits fixed across the complete aligned VM aperture can exclude a
    // BAT. This stays conservative for partial overlaps and unusual BL masks.
    if (((upper ^ AddressSpace::FakeVmemBase) & compared_bits) == 0u)
      modes |= static_cast<std::uint8_t>(upper & 3u);
  }
  vmem_bat_modes_ = modes;
  // A supervisor BAT over the aperture takes precedence over the page table;
  // the generated-code DTLB mirror does not model BATs, so withdraw it.
  if ((modes & 2u) != 0u) ClearPagedView();
}

HostRuntime::DataAddressTranslation HostRuntime::TranslatePagedDataAddress(std::uint32_t address, bool write) {
  using Status = PageTranslation::Status;
  const bool user = (cpu_.msr & kPpcMsrPr) != 0u;
  const std::uint32_t segment = cpu_.sr[address >> 28u];
  if (segment & 0x80000000u) return {Status::DirectStore};
  const std::uint32_t vsid = segment & 0x00ffffffu;
  const std::uint32_t ea_page = address & 0xfffff000u;
  const std::uint32_t set_index = (address >> 12u) & 0x3fu;
  auto& set = data_tlb_[set_index];
  for (unsigned way = 0; way < 2u; ++way) {
    auto& entry = set[way];
    if (!entry.valid || entry.ea_page != ea_page || entry.vsid != vsid) continue;
    ++data_tlb_hits_;
    entry.stamp = ++data_tlb_stamp_;
    // 2-way LRU state, shared with generated-code mirror hits.
    memory_.PagedViewMru()[set_index] = static_cast<std::uint8_t>(way);
    if (AddressSpace::IsVmem(address) &&
        memory_.PagedViewPagesTable()[(ea_page - AddressSpace::FakeVmemBase) >> 12u] == 0u)
      PublishPagedView(entry, way);
    const bool key = (segment & (user ? 0x20000000u : 0x40000000u)) != 0u;
    const unsigned pp = entry.pte_lower & 3u;
    const bool readable = !key || pp != 0u;
    const bool writable = pp == 2u || (!key && pp != 3u);
    if (!readable || (write && !writable)) return {Status::Protection};

    // A read fill already set R in the backing PTE.  C is different: on a
    // first store through a resident C=0 entry, Gekko re-walks the HPT so the
    // page-table C bit becomes architecturally visible.  If software removed
    // the PTE before that store, the stale TLB entry must not silently dirty
    // the old physical frame; the access takes the normal page fault instead.
    //
    // Dolphin models this as TLBLookupResult::UpdateC followed by a page-table
    // walk.  CoD Finest Hour's VM pager depends on the same boundary when it
    // evicts allocator pages to ARAM: losing C lets a dirty page be discarded
    // and later restored with stale heap links.
    if (write && (entry.pte_lower & 0x80u) == 0u) {
      return WalkPagedDataAddress(address, true, &entry);
    }

    return {Status::Mapped, entry.physical_page | (address & 0xfffu)};
  }

  return WalkPagedDataAddress(address, write);
}

// Keep the rich PTE result and its stack temporary out of the resident DTLB
// path. Table walks and first-write C updates retain the same architectural
// effects, but ordinary cached loads/stores only return two host register words.
#if defined(_MSC_VER)
__declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
__attribute__((noinline))
#endif
HostRuntime::DataAddressTranslation HostRuntime::WalkPagedDataAddress(
    std::uint32_t address, bool write, DataTlbEntry* changed_entry) {
  using Status = PageTranslation::Status;
  const bool user = (cpu_.msr & kPpcMsrPr) != 0u;
  const std::uint32_t segment = cpu_.sr[address >> 28u];
  auto result = TranslatePage(memory_, address, segment, spr_state_[25u], user,
                              write ? PageAccess::Write : PageAccess::Read);
  if (changed_entry) {
    if (result.status == Status::Mapped) {
      changed_entry->pte_lower |= 0x180u;
      auto& changed_set = data_tlb_[(address >> 12u) & 0x3fu];
      PublishPagedView(*changed_entry, changed_entry == &changed_set[0] ? 0u : 1u);
    }
    return {result.status, result.physical};
  }
  if (result.status != Status::Mapped || result.pte_address == 0u)
    return {result.status, result.physical};

  const std::uint32_t vsid = segment & 0x00ffffffu;
  const std::uint32_t ea_page = address & 0xfffff000u;
  const std::uint32_t set_index = (address >> 12u) & 0x3fu;
  auto& set = data_tlb_[set_index];
  auto& mru = memory_.PagedViewMru()[set_index];
  unsigned way = !set[0].valid ? 0u : !set[1].valid ? 1u : (mru ^ 1u) & 1u;
  DataTlbEntry* victim = &set[way];
  DropPagedView(*victim);
  *victim = {true, ea_page, vsid, result.physical & 0xfffff000u,
             result.pte_address, result.pte_upper, result.pte_lower, ++data_tlb_stamp_};
  mru = static_cast<std::uint8_t>(way);
  PublishPagedView(*victim, way);
  ++data_tlb_fills_;
  return {result.status, result.physical};
}

HostRuntime::DataAddressTranslation HostRuntime::TranslateVmemAddress(std::uint32_t address, bool write) {
  using Status = PageTranslation::Status;
  if ((cpu_.msr & 0x10u) == 0u) return {Status::Mapped, address};
  const bool user = (cpu_.msr & kPpcMsrPr) != 0u;
  if ((vmem_bat_modes_ & (user ? 1u : 2u)) == 0u && AddressSpace::IsVmem(address))
    return TranslatePagedDataAddress(address, write);
  // BAT translation takes precedence over the hashed page table.
  for (unsigned index = 0; index < 4; ++index) {
    const auto upper = spr_state_[536u + index * 2u];
    const auto lower = spr_state_[537u + index * 2u];
    if ((upper & (user ? 1u : 2u)) == 0u) continue;
    const auto offset_mask = ((upper & 0x1ffcu) << 15u) | 0x1ffffu;
    if ((address & ~offset_mask) != (upper & 0xfffe0000u & ~offset_mask)) continue;
    const auto pp = lower & 3u;
    if (pp == 0u || (write && pp != 2u)) return {Status::Protection};
    return {Status::Mapped, (lower & 0xfffe0000u & ~offset_mask) | (address & offset_mask)};
  }
  return TranslatePagedDataAddress(address, write);
}

bool HostRuntime::AccessPagedVmem(std::uint32_t address, std::uint64_t* value,
                                  std::uint8_t size, bool write) {
  if (!memory_.PagedVmem() || !AddressSpace::IsVmem(address)) return false;
  if (size != 1u && size != 2u && size != 4u && size != 8u) {
    SetFault(write ? FaultKind::InvalidMemoryWrite : FaultKind::InvalidMemoryRead, address, size);
    return true;
  }

  using Status = PageTranslation::Status;
  const auto translation_fault = [&](std::uint32_t ea, Status status) {
    if (status == Status::InvalidTable) {
      SetFault(write ? FaultKind::InvalidMemoryWrite : FaultKind::InvalidMemoryRead,
               ea, spr_state_[25u]);
      return;
    }
    const auto old_msr = cpu_.msr;
    last_dsi_pc_ = cpu_.pc;
    last_dsi_lr_ = cpu_.lr;
    last_dsi_msr_ = old_msr;
    // With MSR[RI]=0 the SDK vector routes the DSI to
    // OSDefaultExceptionHandler ("Non-recoverable Exception 2"), which is fatal.
    if ((old_msr & 0x2u) == 0u && nonrecoverable_dsi_reports_ < 8u) {
      ++nonrecoverable_dsi_reports_;
      std::fprintf(stderr,
                   "GEKKOAOT_VM_DSI_NONRECOVERABLE_V321=1 pc=%08x lr=%08x msr=%08x ea=%08x write=%u r1=%08x\n",
                   cpu_.pc, cpu_.lr, old_msr, ea, write ? 1u : 0u, cpu_.gpr[1]);
    }
    cpu_.dar = ea;
    cpu_.dsisr = (write ? 0x02000000u : 0u) |
        (status == Status::Missing ? 0x40000000u :
         status == Status::DirectStore ? 0x04000000u : 0x08000000u);
    cpu_.srr0 = cpu_.pc;
    cpu_.srr1 = old_msr & kPpcMsrRfiMask;
    cpu_.msr = ((old_msr & ~kPpcMsrLe) |
                ((old_msr & kPpcMsrIle) ? kPpcMsrLe : 0u)) &
               ~kPpcExceptionEntryClearMask;
    cpu_.pc = (old_msr & 0x40u) ? 0xfff00300u : 0x300u;
    cpu_.exception |= kPpcExcDsi;
    NoteDsiTrace('R', last_dsi_pc_, cpu_.exception);
    BumpHostReadEpoch(&cpu_);
  };

  // Scalar VM traffic overwhelmingly stays inside one 4 KiB page.  The
  // old generic path built an array of up to eight host byte pointers and then
  // copied the value one byte at a time even after translation had already
  // proven the physical frame contiguous.  Keep the cross-page preflight below
  // for the rare boundary case, but make ordinary 1/2/4/8-byte accesses one
  // translation + one compact endian load/store.
  if (size <= 0x1000u - (address & 0xfffu)) {
    const auto translation = TranslateVmemAddress(address, write);
    if (translation.status != Status::Mapped) {
      translation_fault(address, translation.status);
      return true;
    }
    if (translation.physical >= memory_.Mem1().size() ||
        size > memory_.Mem1().size() - translation.physical) {
      SetFault(write ? FaultKind::InvalidMemoryWrite : FaultKind::InvalidMemoryRead,
               address, translation.physical);
      return true;
    }

    auto* p = memory_.Mem1().data() + translation.physical;
    if (write) {
      const std::uint64_t v = *value;
      switch (size) {
      case 1u:
        p[0] = static_cast<std::uint8_t>(v);
        break;
      case 2u:
        p[0] = static_cast<std::uint8_t>(v >> 8u);
        p[1] = static_cast<std::uint8_t>(v);
        break;
      case 4u:
        p[0] = static_cast<std::uint8_t>(v >> 24u);
        p[1] = static_cast<std::uint8_t>(v >> 16u);
        p[2] = static_cast<std::uint8_t>(v >> 8u);
        p[3] = static_cast<std::uint8_t>(v);
        break;
      case 8u:
        p[0] = static_cast<std::uint8_t>(v >> 56u);
        p[1] = static_cast<std::uint8_t>(v >> 48u);
        p[2] = static_cast<std::uint8_t>(v >> 40u);
        p[3] = static_cast<std::uint8_t>(v >> 32u);
        p[4] = static_cast<std::uint8_t>(v >> 24u);
        p[5] = static_cast<std::uint8_t>(v >> 16u);
        p[6] = static_cast<std::uint8_t>(v >> 8u);
        p[7] = static_cast<std::uint8_t>(v);
        break;
      }
    } else {
      switch (size) {
      case 1u:
        *value = p[0];
        break;
      case 2u:
        *value = (std::uint64_t(p[0]) << 8u) | std::uint64_t(p[1]);
        break;
      case 4u:
        *value = (std::uint64_t(p[0]) << 24u) |
                 (std::uint64_t(p[1]) << 16u) |
                 (std::uint64_t(p[2]) << 8u) |
                 std::uint64_t(p[3]);
        break;
      case 8u:
        *value = (std::uint64_t(p[0]) << 56u) |
                 (std::uint64_t(p[1]) << 48u) |
                 (std::uint64_t(p[2]) << 40u) |
                 (std::uint64_t(p[3]) << 32u) |
                 (std::uint64_t(p[4]) << 24u) |
                 (std::uint64_t(p[5]) << 16u) |
                 (std::uint64_t(p[6]) << 8u) |
                 std::uint64_t(p[7]);
        break;
      }
    }
    return true;
  }

  // A scalar that straddles a page boundary must preflight every participating
  // page before the first store. Adjacent virtual pages need not map to
  // adjacent physical frames, and a fault on page two must leave page one
  // untouched.
  std::array<std::uint8_t*, 8> bytes{};
  for (unsigned offset = 0; offset < size;) {
    const auto ea = address + offset;
    const auto translation = TranslateVmemAddress(ea, write);
    if (translation.status != Status::Mapped) {
      translation_fault(ea, translation.status);
      return true;
    }
    const unsigned count = std::min<unsigned>(size - offset, 0x1000u - (ea & 0xfffu));
    if (translation.physical >= memory_.Mem1().size() ||
        count > memory_.Mem1().size() - translation.physical) {
      SetFault(write ? FaultKind::InvalidMemoryWrite : FaultKind::InvalidMemoryRead,
               ea, translation.physical);
      return true;
    }
    for (unsigned i = 0; i < count; ++i)
      bytes[offset + i] = memory_.Mem1().data() + translation.physical + i;
    offset += count;
  }
  if (write) {
    for (unsigned i = 0; i < size; ++i)
      *bytes[i] = std::uint8_t(*value >> ((size - 1u - i) * 8u));
  } else {
    *value = 0;
    for (unsigned i = 0; i < size; ++i) *value = (*value << 8u) | *bytes[i];
  }
  return true;
}

const GekkoAOTNativeHardware& HostRuntime::NativeHardwareServices() {
  static const GekkoAOTNativeHardware services{
      1u, sizeof(GekkoAOTNativeHardware), NativeCPRead16, NativeVIRead16,
      NativePIWrite32, NativeWgpipeWrite};
  return services;
}

const GekkoAOTNativeAddressWindow& HostRuntime::NativeAddressWindowServices(bool published) {
  static const GekkoAOTNativeAddressWindow services{1u, sizeof(GekkoAOTNativeAddressWindow), NativeAddressBase};
  static const GekkoAOTNativeAddressWindow direct{1u, sizeof(GekkoAOTNativeAddressWindow), PublishedNativeAddressBase};
  return published ? direct : services;
}
std::uint8_t* HostRuntime::NativeAddressBase(CPUState* cpu) {
  auto* self = Self(cpu);
  return self && self->native_address_base_enabled_ ? self->memory_.NativeAddressBase() : nullptr;
}
std::uint8_t* HostRuntime::PublishedNativeAddressBase(CPUState* cpu) {
  auto* self = Self(cpu);
  return self ? self->published_native_address_base_ : nullptr;
}

const GekkoAOTNativeLockedCache& HostRuntime::NativeLockedCacheServices() {
  static const GekkoAOTNativeLockedCache services{1u,sizeof(GekkoAOTNativeLockedCache),NativeLockedCacheBase};
  return services;
}
std::uint8_t* HostRuntime::NativeLockedCacheBase(CPUState* cpu) {
  auto* self=Self(cpu);
  return self && self->native_locked_cache_enabled_ ?
      static_cast<std::uint8_t*>(self->lc_.Pointer(HW::LC::NativeLC::Base,HW::LC::NativeLC::Size)):nullptr;
}

const GekkoAOTNativeFlatVmem& HostRuntime::NativeFlatVmemServices() {
  static const GekkoAOTNativeFlatVmem services{2u, sizeof(GekkoAOTNativeFlatVmem), NativeFlatVmemView};
  return services;
}
const GekkoAOTFlatVmemView* HostRuntime::NativeFlatVmemView(CPUState* cpu) {
  auto* self = Self(cpu);
  return self && self->native_flat_vmem_enabled_ ? self->memory_.FlatVmemView() : nullptr;
}

const GekkoAOTNativeStableMmio& HostRuntime::NativeStableMmioServices() {
  static const GekkoAOTNativeStableMmio services{
      1u, sizeof(GekkoAOTNativeStableMmio), NativeTryStableMmioRead};
  return services;
}

std::uint64_t HostRuntime::NativeTryStableMmioRead(CPUState* cpu, std::uint32_t address,
                                                std::uint8_t size) {
  auto* self = Self(cpu);
  const auto physical = AddressSpace::ToPhysical(address);
  // Only CP/PE reads observe GP progress; PI INTSR/INTMR need just the
  // already-published PE events (PeEventPending sends them to the slow path).
  const bool gp_progress = physical < 0x0c002000u;
  if (!self || !self->native_hw_.Enabled() || cpu->exception ||
      self->fault_.kind != FaultKind::None ||
      (gp_progress && self->native_gx_.WritesPending()) || self->native_gx_.PeEventPending() ||
      self->native_gx_.PeEventOverflow() || self->cp_.RegisterDrainPending()) return 0u;
  std::uint64_t value = 0u;
  bool handled = false;
  if (size == 2u && !(physical & 1u)) {
    if (self->native_hw_.CP() && physical >= 0x0c000000u && physical < 0x0c001000u) {
      handled = self->cp_.Read(address, size, &value);
      if (handled) {
        self->SyncNativeCPInterrupt();
        // An unchanged CP status read in a tight loop waits for other
        // guest activity (THUG's GP watchdog thread spins on GXGetGPStatus
        // for the FIFO high watermark): park it like other device polls.
        if (physical == 0x0c000000u && self->cp_poll_park_enabled_) self->NoteMmioPoll(address, value);
      }
    } else if (physical >= 0x0c001000u && physical <= 0x0c00100eu) {
      // PE config/control/token are scalar mirrors. Bounding box and EFB
      // accesses need GPU work and cannot enter this state-free service.
      handled = self->pe_.Read(address, size, &value);
      if (handled) self->SyncNativePEInterrupts();
    }
  } else if (size == 4u && self->native_hw_.PI() &&
             (physical == 0x0c003000u || physical == 0x0c003004u)) {
    handled = self->pi_.Read(address, size, &value);
  }
  if (!handled) return 0u;
  self->CountHostEffect();
  self->RecordMmioRead(address);
  BumpHostReadEpoch(cpu);
  return (std::uint64_t{1} << 63u) | value;
}

std::uint64_t HostRuntime::NativeCPRead16(CPUState* cpu, std::uint32_t address,
                                         std::uint8_t size) {
  auto* self = Self(cpu);
  if (self) self->CountHostEffect();
  if (!self || size != 2u || (address & 1u) != 0u || !self->cp_.Handles(address))
    return ExternalRead(cpu, address, size);
  // Same CP/PE visibility boundary, event epoch, telemetry and IRQ as the bus.
  (void)self->native_gx_.FlushWrites(GX::HostBridge::SyncReason::MmioRead);
  self->SyncNativeGXInterrupts();
  BumpHostReadEpoch(cpu);
  self->RecordMmioRead(address);
  std::uint64_t value = 0u;
  if (self->cp_.Read(address, size, &value)) {
    self->SyncNativeCPInterrupt();
    return value;
  }
  self->SetFault(FaultKind::MmioRead, address);
  return 0u;
}

std::uint64_t HostRuntime::NativeVIRead16(CPUState* cpu, std::uint32_t address,
                                         std::uint8_t size) {
  auto* self = Self(cpu);
  if (self) self->CountHostEffect();
  const auto offset = AddressSpace::ToPhysical(address) - HW::VI::NativeVI::BasePhysical;
  if (!self || size != 2u || (offset & 1u) != 0u || offset > 0x74u)
    return ExternalRead(cpu, address, size);
  BumpHostReadEpoch(cpu);
  self->ServiceTimedMmioPoll(address);
  self->RecordMmioRead(address);
  std::uint64_t value = 0u;
  if (self->vi_.Read(address, size, &value)) return value;
  self->SetFault(FaultKind::MmioRead, address);
  return 0u;
}

void HostRuntime::NativePIWrite32(CPUState* cpu, std::uint32_t address,
                                  std::uint64_t value, std::uint8_t size) {
  auto* self = Self(cpu);
  if (self) self->CountHostEffect();
  if (!self || size != 4u || (address & 3u) != 0u || !self->pi_.Handles(address)) {
    ExternalWrite(cpu, address, value, size);
    return;
  }
  (void)self->native_gx_.FlushWrites(GX::HostBridge::SyncReason::MmioWrite);
  self->SyncNativeGXInterrupts();
  BumpHostWriteEpoch(cpu);
  if (self->pi_.Write(address, value, size)) return;
  self->SetFault(FaultKind::MmioWrite, address, static_cast<std::uint32_t>(value));
}

void HostRuntime::ServiceTimedMmioPoll(std::uint32_t address,bool counter) {
  const auto timed_mmio_physical = AddressSpace::ToPhysical(address);
  // Do not hit the host clock on every load.  Tight guest poll loops execute
  // this branch extremely often, so 1/64 reads is still far above any
  // GameCube device cadence while keeping the fix cheap.
  static std::uint32_t timed_mmio_poll_divider = 0u;
  // Native performance-counter reads are also time-visible boundaries. A
  // tiny elapsed-time wait can stay entirely inside one AOT function; leaving
  // its counter frozen until that function exits needlessly burns the core.
  const bool sample=counter ? (++native_counter_sample_divider_&7u)==0u :
                             (++timed_mmio_poll_divider&63u)==0u;
  if (sample) {
    constexpr std::uint64_t kTimedPollSampleCredit =
        (GekkoAOT::HW::VI::NativeVI::CpuClockHz / 16000u) + 1u;
    const std::uint64_t timed_cycles =
        TakeRealtimeHardwareCycles(kTimedPollSampleCredit, true);

    if (timed_cycles != 0u) {
      perf_hardware_cycles_total_ += timed_cycles;

      // Keep CPU timers and asynchronous DVD completion moving even if the
      // generated function never reaches a normal HostRuntime boundary.
      if(!native_only_) ServiceDiscCompletion(timed_cycles);
      // A generated AOT function can stay inside a timed MMIO poll without
      // returning its retired-cycle charge. Retain real-time TB/DEC progress
      // at this exceptional in-function service point to prevent infinite
      // polling. These cycles must NOT accelerate VI/audio differently.
      AdvanceCpuTimers(timed_cycles);

      if(!native_only_) {si_.AdvanceCycles(timed_cycles);SyncNativeSIInterrupt();}

      vi_.AdvanceCycles(timed_cycles);
      if (vi_.ConsumeInterruptUpdate())
        SyncNativeVIInterrupt();

      // NativeSI has no per-cycle advance method.  Match the runtime's
      // existing ~120 Hz host-input cadence and publish a fresh SI sample.
      static std::uint64_t timed_si_phase = 0u;
      timed_si_phase += timed_cycles;
      const std::uint64_t kTimedSiPollCycles = NativeInputPeriod();
      if (timed_si_phase >= kTimedSiPollCycles) {
        timed_si_phase %= kTimedSiPollCycles;
#if defined(GEKKOAOT_NATIVE_SDL_INPUT)
        if (native_input_ready_)
          RefreshNativeInput(!native_only_);
        else {
          if(!native_only_) {si_.PollNow();SyncNativeSIInterrupt();}
        }
#else
        if(!native_only_) {si_.PollNow();SyncNativeSIInterrupt();}
#endif
      }

      // Nemo's hot loop polls AI from inside one AOT wrapper.  Advancing
      // these two domains here lets the sample counter / IRQ state change
      // without waiting for that wrapper to return.
      dsp_.AdvanceCycles(timed_cycles);
      ai_.AdvanceCycles(timed_cycles);
      SyncNativeDSPInterrupt();
      SyncNativeAIInterrupt();

      static unsigned timed_mmio_logs = 0u;
      if (!counter && timed_mmio_logs++ < 64u)
        std::fprintf(stderr,
                     "GEKKOAOT_TIMED_MMIO_POLL_V203=1 ea=%08x physical=%08x cycles=%llu pc=%08x lr=%08x action=advance-realtime-devices-inside-aot-poll\n",
                     address, timed_mmio_physical,
                     static_cast<unsigned long long>(timed_cycles),
                     cpu_.pc, cpu_.lr);
      if(counter && runtime_profile_enabled_) {
        static unsigned counter_samples=0;
        if(counter_samples++<16u)
          std::fprintf(stderr,"GEKKOAOT_NATIVE_COUNTER_CLOCK_V318=1 pc=%08x elapsed_cycles=%llu source=host-monotonic within=aot\n",
            address,static_cast<unsigned long long>(timed_cycles));
      }
    }
  }
}

void HostRuntime::RecordMmioRead(std::uint32_t address) {
  ++mmio_reads_;
  if (address == last_mmio_read_)
    ++same_mmio_read_streak_;
  else {
    last_mmio_read_ = address;
    same_mmio_read_streak_ = 1;
  }
}

std::uint64_t HostRuntime::ExternalRead(CPUState* cpu, std::uint32_t address, std::uint8_t size) {
  if (auto* self = Self(cpu)) {
    self->CountHostEffect();
    if (self->runtime_profile_enabled_) {
      ++self->external_reads_[ExternalClass(address)];
      self->NoteExternalOther(address, false);
    }
    std::uint64_t value = 0;
    const std::uint32_t physical = AddressSpace::ToPhysical(address);
    const bool flipper_mmio = physical >= 0x0c000000u && physical < 0x0c007000u;
    // ExternalRead is overwhelmingly Flipper MMIO in retail hot loops.
    // Once the physical page is in the fixed hardware aperture it cannot alias
    // MEM1/MEM2, paged VMEM or locked cache, so skip those generic probes.
    if (!flipper_mmio) {
      if (self->AccessPagedVmem(address, &value, size, false)) return value;

      // DolRecomp resolves the normal cached/uncached MEM1 aliases directly,
      // but low physical addresses used by Nintendo's exception/OS code fall
      // through to the external bus. Treat any address that the standalone
      // AddressSpace can actually resolve as RAM before trying hardware MMIO.
      if (ReadAddressSpaceValue(self->memory_, address, size, &value)) return value;
    }

    // v161 WGPIPE host batching may defer renderer parsing for a handful of
    // completed gather lines. Guest reads of CP/PE are ordering points: a
    // token, draw-done or FIFO status produced by those commands must be
    // visible before the load completes. VI/AI/etc do not depend on renderer
    // command consumption and therefore do not force a flush.
    //
    // PI reads are not GP progress observations. INTSR/INTMR show the
    // interrupts already raised (PE events published by the GX worker are
    // folded in by SyncNativeGXInterrupts) and the CPU-FIFO pointers are host
    // state. The OS external-interrupt dispatcher reads INTSR on every VI/AI/
    // DSP/SI interrupt; draining the whole GX queue there stalled the guest
    // for ~1 ms a few hundred times per second.
    if (flipper_mmio) {
      const auto page = physical & 0xfffff000u;
      if (page == HW::CP::NativeCP::BasePhysical ||
          page == HW::PE::NativePE::BasePhysical) {
        (void)self->native_gx_.FlushWrites(GX::HostBridge::SyncReason::MmioRead);
        self->SyncNativeGXInterrupts();
      } else if (page == HW::PI::NativePI::BasePhysical) {
        self->SyncNativeGXInterrupts();
      }
    }

    // Any access that leaves ordinary MEM1/MEM2 is a timing-visible host
    // boundary.  The in-module superchain checks this epoch after the current
    // native wrapper returns and yields to HostRuntime before chaining again.
    BumpHostReadEpoch(cpu);

    // A compiled PPC function can contain an entire MMIO polling loop.
    // In that case the wrapper never returns to AdvanceRuntimeCycles(), so the
    // realtime device the guest is waiting for never advances.  Nemo hits this
    // on AI sample/status polling; Rayman 3 can sit on SI status polling.
    //
    // Sample host time from inside time-driven MMIO loops and advance only the
    // standalone devices whose state is allowed to change with elapsed time.
    // TakeRealtimeHardwareCycles() remains the single owner of the host clock,
    // so the next normal runtime boundary observes only the remaining delta
    // (no double advancement).
    const std::uint32_t timed_mmio_physical = AddressSpace::ToPhysical(address);
    if(self->RejectHardwareAccess(address,false)) return 0;
    const bool timed_mmio_poll =
        // VI beam / retrace / interrupt state.
        (timed_mmio_physical >= 0x0c002000u &&
         timed_mmio_physical <  0x0c003000u) ||
        // DSP/ARAM/audio-DMA state.
        (timed_mmio_physical >= 0x0c005000u &&
         timed_mmio_physical <  0x0c005100u) ||
        // DI deferred command completion.
        (timed_mmio_physical >= 0x0c006000u &&
         timed_mmio_physical <  0x0c006028u) ||
        // SI controller/status polling.
        (timed_mmio_physical >= 0x0c006400u &&
         timed_mmio_physical <  0x0c006500u) ||
        // AI control/sample counter/interrupt timing.
        (timed_mmio_physical >= 0x0c006c00u &&
         timed_mmio_physical <  0x0c006c10u);

    if (timed_mmio_poll) self->ServiceTimedMmioPoll(address);

    // Keep low-cost polling telemetry for black-screen diagnosis.  A tight
    // SDK hardware wait is valid guest code, so never terminate it here; the
    // final run result merely reports which MMIO register dominated the tail.
    self->RecordMmioRead(address);

    if (!flipper_mmio && self->lc_.Read(address, size, &value)) return value;

    // Flipper MMIO blocks are non-overlapping fixed physical windows.
    // The old generic path asked every device whether it owned each address,
    // which turned hot CP/VI/PI polling into 6-10 range checks plus calls per
    // guest load. Decode the 4 KiB hardware page once and only enter the owner.
    switch (physical & 0xfffff000u) {
    case HW::CP::NativeCP::BasePhysical:
      // A released CP breakpoint defers the GP drain to the next host
      // boundary (the release usually happens in the CP interrupt handler).
      // A thread that then polls CP state (EE on, same register again) waits
      // for that drain: the hardware GP keeps reading during the wait. THUG
      // spun ~12M CP status reads/s until the AOT cycle guard reached a host
      // boundary. Serve the pending drain to such a poll.
      if (self->cp_poll_drain_enabled_ && self->cp_.RegisterDrainPending() &&
          (cpu->msr & kPpcMsrEe) != 0u && self->same_mmio_read_streak_ > 1u) {
        self->cp_.ServiceRegisterDrain();
        self->SyncNativeCPInterrupt();
        ++self->cp_poll_drains_;
        if (self->cp_poll_drains_ <= 4u)
          std::fprintf(stderr, "GEKKOAOT_CP_POLL_DRAIN_V366=1 ea=%08x pc=%08x drains=%llu action=serve-deferred-gp-drain\n",
                       address, cpu->pc, static_cast<unsigned long long>(self->cp_poll_drains_));
      }
      if (self->cp_.Read(address, size, &value)) {
        self->SyncNativeCPInterrupt();
        if (physical == 0x0c000000u && self->cp_poll_park_enabled_) self->NoteMmioPoll(address, value);
        // Diagnostic: GEKKOAOT_CP_POLL_TRACE=1 samples hot CP register polls.
        static const bool cp_trace = RuntimeEnvBool("GEKKOAOT_CP_POLL_TRACE", false);
        if (cp_trace) {
          static std::uint64_t cp_reads = 0; static unsigned window = 0, windows = 0;
          if ((++cp_reads & 0xfffffu) == 0u && windows < 24u) { window = 12u; ++windows; }
          if (window) {
            --window;
            std::fprintf(stderr, "GEKKOAOT_CP_POLL_TRACE ea=%08x value=%llx pc=%08x lr=%08x msr=%x "
                         "rw=%08x rp=%08x wp=%08x ctl=%04x\n", address,
                         static_cast<unsigned long long>(value), cpu->pc, cpu->lr, cpu->msr,
                         self->cp_.FifoReadWriteDistance(), self->cp_.FifoReadPointer(),
                         self->cp_.FifoWritePointer(), self->cp_.Control());
          }
        }
        return value;
      }
      break;
    case HW::PE::NativePE::BasePhysical:
      // SDK IRQ handlers consume a single notification before acknowledging
      // it. Repeated token polling with EE disabled instead observes the
      // renderer's latest register, without requiring interrupt delivery.
      if (physical == 0x0c00100eu && self->same_mmio_read_streak_ > 1u &&
          !(cpu->msr & kPpcMsrEe) && !self->pe_notification_irq_active_)
        self->SyncNativeGXInterrupts(true);
      if (physical >= 0x0c001010u && physical <= 0x0c001016u &&
          ((size == 2u && (physical & 1u) == 0u) ||
           (size == 4u && (physical & 3u) == 0u))) {
        std::array<std::uint16_t, 4> bounds;
        if (self->native_gx_.ReadBoundingBox(&bounds)) self->pe_.SetBoundingBox(bounds);
      }
      if (self->pe_.Read(address, size, &value)) {
        static const bool trace_pe=RuntimeEnvBool("GEKKOAOT_TRACE_PE",false);
        static unsigned reads=0;
        if(trace_pe&&physical==0x0c00100eu&&reads++<64u)
          std::fprintf(stderr,"GEKKOAOT_TRACE_PE event=read-token value=%llx pc=%08x lr=%08x\n",static_cast<unsigned long long>(value),cpu->pc,cpu->lr);
        self->SyncNativePEInterrupts();
        return value;
      }
      break;
    case HW::VI::NativeVI::BasePhysical:
      if (self->vi_.Read(address, size, &value)) return value;
      break;
    case HW::PI::NativePI::BasePhysical:
      if (self->pi_.Read(address, size, &value)) return value;
      break;
    case HW::MI::NativeMI::BasePhysical:
      if (self->mi_.Read(address, size, &value)) return value;
      break;
    case DSP::NativeDSP::BasePhysical:
      if (self->dsp_.Read(address, size, &value)) {
        self->SyncNativeDSPInterrupt();
        self->NoteMmioPoll(address, value);
        return value;
      }
      break;
    case 0x0c006000u:
      // DI/SI/EXI/AI share the final Flipper MMIO page but occupy disjoint
      // sub-windows. Use the high byte within the page as a second-level key.
      switch (physical & 0x00000f00u) {
      case 0x000u:
        if (self->di_.Read(address, size, &value)) {
          self->SyncNativeDIInterrupt();
          self->NoteMmioPoll(address, value);
          return value;
        }
        break;
      case 0x400u:
        if (self->si_.Read(address, size, &value)) {
          self->SyncNativeSIInterrupt();
          self->NoteMmioPoll(address, value);
          return value;
        }
        break;
      case 0x800u:
        if (self->exi_.ReadRegister(address, size, &value)) {
          self->SyncNativeEXIInterrupt();
          self->NoteMmioPoll(address, value);
          return value;
        }
        break;
      case 0xc00u:
        if (self->ai_.Read(address, size, &value)) {
          self->SyncNativeAIInterrupt();
          self->NoteMmioPoll(address, value);
          return value;
        }
        break;
      default:
        break;
      }
      break;
    default:
      break;
    }
    if (physical >= 0x08000000u && physical < 0x0c000000u) {
      // CPU-visible EFB aperture, matching the Flipper/Dolphin address layout:
      //   x = (addr & 0xfff) >> 2, y = (addr >> 12) & 0x3ff
      //   bit 22 clear -> color, bit 22 set -> depth
      //   bit 23 set   -> combined Z+color (still fail-closed)
      // v162 added depth for GXPeekZ; v163 closes the normal 32-bit color read
      // side for GXPeekARGB while keeping unknown aperture semantics explicit.
      if (size == 4u && (physical & 0x00800000u) == 0u && self->native_gx_.Ready()) {
        const std::uint16_t x =
            static_cast<std::uint16_t>((physical & 0x00000fffu) >> 2u);
        const std::uint16_t y =
            static_cast<std::uint16_t>((physical >> 12u) & 0x000003ffu);

        if ((physical & 0x00400000u) != 0u) {
          std::uint32_t z = 0u;
          if (self->native_gx_.PeekEfbZ(x, y, &z)) {
            static unsigned efb_z_logs = 0u;
            if (efb_z_logs++ < 32u)
              std::fprintf(stderr,
                           "GEKKOAOT_EFB_Z_PEEK_V162=1 ea=%08x physical=%08x x=%u y=%u z=%06x pc=%08x lr=%08x source=aurora-depth-snapshot\n",
                           address, physical, unsigned(x), unsigned(y),
                           z & 0x00ffffffu, cpu->pc, cpu->lr);
            return z & 0x00ffffffu;
          }
        } else {
          std::uint32_t argb = 0u;
          if (self->native_gx_.PeekEfbArgb(x, y, &argb)) {
            // PE_ALPHAREAD is CPU-visible state, so apply it here after the
            // renderer has reproduced the current EFB pixel-format precision.
            // 0=force 00, 1=force FF, 2=return the stored alpha.
            const std::uint16_t alpha_read = self->pe_.AlphaReadMode();
            if (alpha_read == 0u)
              argb &= 0x00ffffffu;
            else if (alpha_read == 1u)
              argb |= 0xff000000u;
            static unsigned efb_color_logs = 0u;
            if (efb_color_logs++ < 32u)
              std::fprintf(stderr,
                           "GEKKOAOT_EFB_COLOR_PEEK_V163=1 ea=%08x physical=%08x x=%u y=%u argb=%08x alpha_read=%u pc=%08x lr=%08x source=aurora-color-snapshot\n",
                           address, physical, unsigned(x), unsigned(y), argb,
                           unsigned(alpha_read), cpu->pc, cpu->lr);
            return argb;
          }
        }
      }

      static unsigned efb_read_logs = 0;
      if (efb_read_logs++ < 32u)
        std::fprintf(stderr,
                     "GEKKOAOT_EFB_APERTURE_V163=0 op=read ea=%08x physical=%08x size=%u pc=%08x lr=%08x reason=unsupported-cpu-efb-access action=fault\n",
                     address, physical, unsigned(size), cpu->pc, cpu->lr);
    }
    if (LooksLikeInvalidGameCubeRamAddress(self->memory_, address)) {
      std::fprintf(stderr,
                   "GEKKOAOT_INVALID_GUEST_EA_V86=1 op=read ea=%08x physical=%08x size=%u pc=%08x lr=%08x sp=%08x r3=%08x r4=%08x r9=%08x r10=%08x r11=%08x action=fault\n",
                   address, AddressSpace::ToPhysical(address), unsigned(size),
                   cpu->pc, cpu->lr, cpu->gpr[1], cpu->gpr[3], cpu->gpr[4],
                   cpu->gpr[9], cpu->gpr[10], cpu->gpr[11]);
      self->SetFault(FaultKind::InvalidMemoryRead, address, size);
    } else {
      self->SetFault(FaultKind::MmioRead, address);
    }
  }
  return 0;
}

bool HostRuntime::CpuFifoTargetsGpu() const {
  // GXBeginDisplayList swaps only the CPU FIFO (PI) to a temporary MEM1
  // recording buffer while the GP FIFO (CP) remains the render FIFO.  The two
  // FIFO objects therefore have different base/end pairs during DL capture.
  // Do not require GPLinkEnabled here: SDK code can briefly toggle link/read
  // bits while keeping the same CPU/GP FIFO object, and those writes still
  // belong to the live graphics stream in the current synchronous backend.
  return pi_.FifoBase() == cp_.FifoBase() && pi_.FifoEnd() == cp_.FifoEnd();
}

bool HostRuntime::CaptureWriteGatherToCpuFifo(std::uint64_t value, std::uint8_t size) {
  if (size != 1u && size != 2u && size != 4u && size != 8u) return false;

  const std::uint32_t base = pi_.FifoBase();
  const std::uint32_t end = pi_.FifoEnd();
  if (base == 0u && end == 0u) return false;

  // There is one physical write-gather pipe, regardless of which PI FIFO is
  // selected. GXBeginDisplayList switches PI before resetting WPAR; without
  // that reset, residual bytes reach the FIFO selected when a line completes.
  if (gpu_fifo_gather_size_ != 0u) {
    if (cpu_fifo_gather_size_ != 0u) {
      SetFault(FaultKind::MmioWrite, 0x0c008000u, gpu_fifo_gather_size_);
      return true;
    }
    std::copy_n(gpu_fifo_gather_.begin(), gpu_fifo_gather_size_, cpu_fifo_gather_.begin());
    cpu_fifo_gather_size_ = gpu_fifo_gather_size_;
    gpu_fifo_gather_size_ = 0u;
  }
  if (cpu_fifo_gather_size_ != 0u &&
      (cpu_fifo_gather_base_ != base || cpu_fifo_gather_end_ != end)) {
    if (const char* setting = std::getenv("GEKKOAOT_NATIVE_WPAR_TRACE");
        setting && setting[0] == '1' && setting[1] == '\0')
      std::fprintf(stderr,
                   "GEKKOAOT_NATIVE_GX_WG_SWITCH_V105=1 bytes=%u old=%08x-%08x new=%08x-%08x\n",
                   cpu_fifo_gather_size_, cpu_fifo_gather_base_, cpu_fifo_gather_end_,
                   base, end);
  }
  cpu_fifo_gather_base_ = base;
  cpu_fifo_gather_end_ = end;

  for (std::uint8_t i = 0; i < size; ++i) {
    cpu_fifo_gather_[cpu_fifo_gather_size_++] =
        static_cast<std::uint8_t>(value >> ((size - 1u - i) * 8u));

    if (cpu_fifo_gather_size_ != cpu_fifo_gather_.size()) continue;

    const std::uint32_t write_pointer = pi_.FifoWritePointer();
    auto* destination = memory_.Resolve(write_pointer, cpu_fifo_gather_.size());
    if (!destination) {
      std::fprintf(stderr,
                   "GEKKOAOT_NATIVE_GX_DL_CAPTURE_ERROR=1 reason=unmapped-cpu-fifo write=%08x base=%08x end=%08x\n",
                   write_pointer, base, end);
      SetFault(FaultKind::MmioWrite, write_pointer, 32u);
      cpu_fifo_gather_size_ = 0u;
      return true;
    }

    std::memcpy(destination, cpu_fifo_gather_.data(), cpu_fifo_gather_.size());
    pi_.AdvanceCpuFifoWritePointer();
    ++cpu_fifo_capture_bursts_;
    cpu_fifo_gather_size_ = 0u;

    if (!cpu_fifo_capture_logged_) {
      cpu_fifo_capture_logged_ = true;
      std::fprintf(stderr,
                   "GEKKOAOT_NATIVE_GX_DL_CAPTURE_V1=1 mode=write-gather-to-cpu-fifo base=%08x end=%08x first_write=%08x burst=32\n",
                   base, end, write_pointer);
    }
  }

  return true;
}

bool HostRuntime::ConsumeGpuFifoBurst(void* user, std::uint32_t address) {
  auto* self = static_cast<HostRuntime*>(user);
  if (!self->native_gx_.Ready()) return true;
  const auto* bytes = self->memory_.Resolve(address, 32u);
  if (!bytes || !self->native_gx_.WriteBurst(bytes, 32u, self->cpu_.pc)) {
    self->SetFault(FaultKind::MmioWrite, address, 32u);
    return false;
  }
  return true;
}

bool HostRuntime::ConsumeGpuFifoBurstBatch(void* user, std::uint32_t address,
                                               std::uint32_t burst_count) {
  auto* self = static_cast<HostRuntime*>(user);
  if (!self || burst_count == 0u) return false;
  if (!self->native_gx_.Ready()) return true;

  // NativeCP guarantees that a batch never crosses the guest FIFO wrap
  // point and is never used while a CP breakpoint is active.  That makes the
  // corresponding MEM1 span contiguous and lets the renderer consume up to
  // 4096 bytes with one DSO transition / parser entry instead of one per line.
  constexpr std::uint32_t kBurstBytes = 32u;
  constexpr std::uint32_t kMaxBatchBursts = 128u;
  const std::uint32_t count = std::min(burst_count, kMaxBatchBursts);
  const std::uint32_t bytes_count = count * kBurstBytes;
  const auto* bytes = self->memory_.Resolve(address, bytes_count);
  if (!bytes || !self->native_gx_.WriteBurst(bytes, bytes_count, self->cpu_.pc)) {
    self->SetFault(FaultKind::MmioWrite, address, bytes_count);
    return false;
  }
  return true;
}

bool HostRuntime::QueueGpuFifoGather(std::uint64_t value, std::uint8_t size,
                                     std::uint32_t guest_pc) {
  if (size != 1u && size != 2u && size != 4u && size != 8u) return false;
  if (!native_gx_.Ready()) return false;
  if (gpu_fifo_gather_size_ == 0u) gpu_fifo_first_pc_ = guest_pc;

  // Backend-neutral WGPIPE append fast path. Only consume a store when it
  // is guaranteed to remain strictly inside the current 32-byte gather line.
  // The store that completes a line (or could straddle it) always falls through
  // to the original path below, preserving PI/CP visibility, breakpoints, burst
  // accounting, renderer submission and host-write epoch semantics exactly.
  if (fast_wgpipe_enabled_) {
    const std::size_t room = gpu_fifo_gather_.size() - gpu_fifo_gather_size_;
    if (size == 4u && room > 4u) {
      auto* out = gpu_fifo_gather_.data() + gpu_fifo_gather_size_;
      const auto word = static_cast<std::uint32_t>(value);
      out[0] = static_cast<std::uint8_t>(word >> 24u);
      out[1] = static_cast<std::uint8_t>(word >> 16u);
      out[2] = static_cast<std::uint8_t>(word >> 8u);
      out[3] = static_cast<std::uint8_t>(word);
      gpu_fifo_gather_size_ += 4u;
      return true;
    }
    if (size == 8u && room > 8u) {
      auto* out = gpu_fifo_gather_.data() + gpu_fifo_gather_size_;
      out[0] = static_cast<std::uint8_t>(value >> 56u);
      out[1] = static_cast<std::uint8_t>(value >> 48u);
      out[2] = static_cast<std::uint8_t>(value >> 40u);
      out[3] = static_cast<std::uint8_t>(value >> 32u);
      out[4] = static_cast<std::uint8_t>(value >> 24u);
      out[5] = static_cast<std::uint8_t>(value >> 16u);
      out[6] = static_cast<std::uint8_t>(value >> 8u);
      out[7] = static_cast<std::uint8_t>(value);
      gpu_fifo_gather_size_ += 8u;
      return true;
    }
  }

  // Materialize the big-endian store once, then copy contiguous spans into the
  // 32-byte gather line. This removes the branch/shift/store loop from the
  // common 32/64-bit WGPIPE path while preserving line-straddling stores.
  std::uint8_t ordered[8];
  // 32-bit WGPIPE stores dominate retail GX traffic. Avoid the generic
  // shift loop on that path; the compiler can fold these fixed shifts into one
  // register sequence and the following memcpy becomes a single 32-bit store.
  if (size == 4u) {
    const auto word = static_cast<std::uint32_t>(value);
    ordered[0] = static_cast<std::uint8_t>(word >> 24u);
    ordered[1] = static_cast<std::uint8_t>(word >> 16u);
    ordered[2] = static_cast<std::uint8_t>(word >> 8u);
    ordered[3] = static_cast<std::uint8_t>(word);
  } else if (size == 8u) {
    ordered[0] = static_cast<std::uint8_t>(value >> 56u);
    ordered[1] = static_cast<std::uint8_t>(value >> 48u);
    ordered[2] = static_cast<std::uint8_t>(value >> 40u);
    ordered[3] = static_cast<std::uint8_t>(value >> 32u);
    ordered[4] = static_cast<std::uint8_t>(value >> 24u);
    ordered[5] = static_cast<std::uint8_t>(value >> 16u);
    ordered[6] = static_cast<std::uint8_t>(value >> 8u);
    ordered[7] = static_cast<std::uint8_t>(value);
  } else if (size == 2u) {
    ordered[0] = static_cast<std::uint8_t>(value >> 8u);
    ordered[1] = static_cast<std::uint8_t>(value);
  } else {
    ordered[0] = static_cast<std::uint8_t>(value);
  }

  return QueueGpuFifoBytes(ordered,size,guest_pc);
}

bool HostRuntime::QueueGpuFifoBytes(const std::uint8_t* ordered,std::size_t size,
                                    std::uint32_t guest_pc) {
  if (!ordered || !size || !native_gx_.Ready()) return false;
  if (gpu_fifo_gather_size_ == 0u) gpu_fifo_first_pc_ = guest_pc;
  std::size_t consumed = 0u;
  while (consumed < size) {
    const std::size_t room = gpu_fifo_gather_.size() - gpu_fifo_gather_size_;
    const std::size_t chunk = std::min<std::size_t>(room, size - consumed);
    std::memcpy(gpu_fifo_gather_.data() + gpu_fifo_gather_size_, ordered + consumed, chunk);
    gpu_fifo_gather_size_ += static_cast<std::uint32_t>(chunk);
    consumed += chunk;
    if (gpu_fifo_gather_size_ != gpu_fifo_gather_.size()) continue;
    if (moh_shadow_active_) MohParticlesShadowLine();

    if (pi_.FifoBase() != 0u || pi_.FifoEnd() != 0u) {
      const auto destination = pi_.FifoWritePointer();
      if (cp_.GPLinkEnabled() && cp_.FifoEnd() >= cp_.FifoBase() &&
          cp_.FifoReadWriteDistance() >= cp_.FifoEnd() - cp_.FifoBase() + 32u &&
          debug_fifo_overrun_) {
        std::fprintf(stderr,
                     "GEKKOAOT_FIFO_OVERRUN pc=%08x msr=%08x read=%08x write=%08x "
                     "distance=%08x control=%04x status=%04x pi_cause=%08x pi_mask=%08x "
                     "pe_token=%04x action=stop-before-overwrite\n",
                     guest_pc, cpu_.msr, cp_.FifoReadPointer(), destination,
                     cp_.FifoReadWriteDistance(), unsigned(cp_.Control()), unsigned(cp_.Status()),
                     pi_.InterruptCauseValue(), pi_.InterruptMaskValue(), unsigned(pe_.Token()));
        gpu_fifo_gather_size_ = 0u;
        SetFault(FaultKind::MmioWrite, destination, 32u);
        return true;
      }
      auto* bytes = memory_.Resolve(destination, gpu_fifo_gather_.size());
      if (!bytes) {
        gpu_fifo_gather_size_ = 0u;
        SetFault(FaultKind::MmioWrite, destination, 32u);
        return true;
      }
      const bool direct_cp = GekkoAOT::NativeHW::CP::CanDirectSubmit(
          native_hw_.CP(), native_gx_.Ready(), cp_.GPLinkEnabled(), cp_.GPReadEnabled(),
          cp_.BreakpointActiveForHost() || cp_.RegisterDrainPending(), cp_.FifoReadWriteDistance(),
          cp_.FifoReadPointer() == cp_.FifoWritePointer());

      // Keep the guest FIFO mirror exact even on the direct path. That makes
      // GX diagnostics/display-list tools and unusual guest reads observe the
      // same RAM bytes while avoiding the normal write->resolve->read callback
      // round trip into Aurora for the overwhelmingly common empty-FIFO case.
      std::memcpy(bytes, gpu_fifo_gather_.data(), gpu_fifo_gather_.size());
      pi_.AdvanceCpuFifoWritePointer();

      if (direct_cp) {
        if (!native_gx_.WriteBurst(gpu_fifo_gather_.data(), 32u, gpu_fifo_first_pc_)) {
          gpu_fifo_gather_size_ = 0u;
          SetFault(FaultKind::MmioWrite, destination, 32u);
          return true;
        }
        if (!cp_.NotifyDirectGatherWrite(32u)) {
          // State changed between policy evaluation and accounting. This is a
          // single-threaded guest boundary, but fail closed rather than silently
          // desynchronizing CP. The exact path handles the next gather line.
          SetFault(FaultKind::MmioWrite, destination, 32u);
          gpu_fifo_gather_size_ = 0u;
          return true;
        }
        SyncNativeCPInterrupt();
        static bool direct_cp_logged = false;
        if (!direct_cp_logged) {
          direct_cp_logged = true;
          std::fprintf(stderr,
                       "GEKKOAOT_NATIVE_HW_CP_DIRECT_V2=1 mode=wgpipe->aurora mirror=guest-fifo fallback=backlog+breakpoint\n");
        }
      } else if (cp_.NotifyGatherWrite(32u)) {
        SyncNativeCPInterrupt();
        if (cp_.BreakpointActiveForHost()) (void)native_gx_.FlushWrites();
      }
    } else {
      if (!native_gx_.WriteBurst(gpu_fifo_gather_.data(), 32u, gpu_fifo_first_pc_)) {
        gpu_fifo_gather_size_ = 0u;
        SetFault(FaultKind::MmioWrite, 0x0c008000u, 32u);
        return true;
      }
    }
    ++gpu_fifo_bursts_;
    gpu_fifo_gather_size_ = 0u;
    gpu_fifo_first_pc_ = guest_pc;
    if (!gpu_fifo_burst_logged_) {
      gpu_fifo_burst_logged_ = true;
      std::fprintf(stderr,
                   "GEKKOAOT_NATIVE_GX_GPU_WG_BURST_V14=1 burst=32 abi=bulk mode=chunk-copy\n");
    }
  }
  return true;
}

// The FIFO stream of a native service. While CP stays in its direct state
// whole lines reach the guest FIFO mirror and the renderer in one piece; partial
// lines and every other CP state take the exact QueueGpuFifoBytes path.
bool HostRuntime::QueueGpuFifoStream(const std::uint8_t* bytes, std::size_t size,
                                     std::uint32_t guest_pc) {
  if (gpu_fifo_gather_size_ != 0u) {
    const std::size_t head =
        std::min<std::size_t>(size, gpu_fifo_gather_.size() - gpu_fifo_gather_size_);
    if (!QueueGpuFifoBytes(bytes, head, guest_pc)) return false;
    bytes += head;
    size -= head;
  }
  while (size >= 32u && fault_.kind == FaultKind::None &&
         (pi_.FifoBase() != 0u || pi_.FifoEnd() != 0u) &&
         GekkoAOT::NativeHW::CP::CanDirectSubmit(
             native_hw_.CP(), native_gx_.Ready(), cp_.GPLinkEnabled(), cp_.GPReadEnabled(),
             cp_.BreakpointActiveForHost() || cp_.RegisterDrainPending(), cp_.FifoReadWriteDistance(),
             cp_.FifoReadPointer() == cp_.FifoWritePointer())) {
    const auto destination = pi_.FifoWritePointer();
    std::size_t lines = size / 32u;
    if (destination <= pi_.FifoEnd())
      lines = std::min<std::size_t>(lines, (pi_.FifoEnd() - destination) / 32u + 1u);
    else
      lines = 1u;
    auto* mirror = memory_.Resolve(destination, std::uint32_t(lines * 32u));
    if (!mirror) break;
    std::memcpy(mirror, bytes, lines * 32u);
    if (!native_gx_.WriteBurst(bytes, std::uint32_t(lines * 32u), guest_pc)) {
      SetFault(FaultKind::MmioWrite, destination, 32u);
      return true;
    }
    for (std::size_t line = 0u; line < lines; ++line) {
      pi_.AdvanceCpuFifoWritePointer();
      if (!cp_.NotifyDirectGatherWrite(32u)) {
        SetFault(FaultKind::MmioWrite, destination, 32u);
        return true;
      }
    }
    SyncNativeCPInterrupt();
    gpu_fifo_bursts_ += lines;
    bytes += lines * 32u;
    size -= lines * 32u;
  }
  return size == 0u || QueueGpuFifoBytes(bytes, size, guest_pc);
}

#if defined(_MSC_VER)
__forceinline
#elif defined(__GNUC__) || defined(__clang__)
__attribute__((always_inline)) inline
#endif
bool HostRuntime::TryGatherPartial(std::uint64_t value, std::uint8_t size, std::uint32_t pc) {
  if (!fast_wgpipe_enabled_ || !native_gx_.Ready() || cpu_fifo_gather_size_ != 0u ||
      !CpuFifoTargetsGpu() ||
      (size != 1u && size != 2u && size != 4u && size != 8u) ||
      gpu_fifo_gather_size_ + size >= gpu_fifo_gather_.size() ||
      (size == 4u && (value == 0xc274d554u ||
                     (pc == 0x800228c0u && value == 0x80000000u))))
    return false;
  if (gpu_fifo_gather_size_ == 0u) gpu_fifo_first_pc_ = pc;
  auto* out = gpu_fifo_gather_.data() + gpu_fifo_gather_size_;
  switch (size) {
  case 1u:
    out[0] = static_cast<std::uint8_t>(value);
    break;
  case 2u:
    out[0] = static_cast<std::uint8_t>(value >> 8u);
    out[1] = static_cast<std::uint8_t>(value);
    break;
  case 4u:
    out[0] = static_cast<std::uint8_t>(value >> 24u);
    out[1] = static_cast<std::uint8_t>(value >> 16u);
    out[2] = static_cast<std::uint8_t>(value >> 8u);
    out[3] = static_cast<std::uint8_t>(value);
    break;
  case 8u:
    out[0] = static_cast<std::uint8_t>(value >> 56u);
    out[1] = static_cast<std::uint8_t>(value >> 48u);
    out[2] = static_cast<std::uint8_t>(value >> 40u);
    out[3] = static_cast<std::uint8_t>(value >> 32u);
    out[4] = static_cast<std::uint8_t>(value >> 24u);
    out[5] = static_cast<std::uint8_t>(value >> 16u);
    out[6] = static_cast<std::uint8_t>(value >> 8u);
    out[7] = static_cast<std::uint8_t>(value);
    break;
  }
  gpu_fifo_gather_size_ += size;
  return true;
}

bool HostRuntime::QueueSdkFifoPacket(const std::uint8_t* bytes,std::size_t size) {
  if (!native_gx_.Ready() || !bytes || size==0u || size>53u) return false;
  ++native_sdk_gx_packets_;
  const auto bump_lines=[&](std::uint64_t before,std::uint64_t after) {
    while (before++<after) BumpHostWriteEpoch(&cpu_);
  };
  if (!CpuFifoTargetsGpu()) {
    for (std::size_t off=0;off<size;) {
      const auto remaining=size-off;
      const std::uint8_t chunk=remaining>=8u?8u:remaining>=4u?4u:remaining>=2u?2u:1u;
      std::uint64_t word=0u;
      for (unsigned i=0;i<chunk;++i) word=(word<<8u)|bytes[off+i];
      const auto before=cpu_fifo_capture_bursts_;
      if (!CaptureWriteGatherToCpuFifo(word,chunk)) return false;
      bump_lines(before,cpu_fifo_capture_bursts_);
      if (fault_.kind!=FaultKind::None) return false;
      off+=chunk;
    }
    return true;
  }
  if (cpu_fifo_gather_size_) {
    if (gpu_fifo_gather_size_) {
      SetFault(FaultKind::MmioWrite,0xcc008000u,cpu_fifo_gather_size_);return false;
    }
    std::copy_n(cpu_fifo_gather_.begin(),cpu_fifo_gather_size_,gpu_fifo_gather_.begin());
    gpu_fifo_gather_size_=cpu_fifo_gather_size_;gpu_fifo_first_pc_=cpu_.pc;
    cpu_fifo_gather_size_=0u;
  }
  const auto before=gpu_fifo_bursts_;
  const auto accepted=QueueGpuFifoBytes(bytes,size,cpu_.pc);
  bump_lines(before,gpu_fifo_bursts_);
  return accepted && fault_.kind==FaultKind::None;
}

const GekkoAOTNativeWgpipeTry& HostRuntime::NativeWgpipeTryServices() {
  static const GekkoAOTNativeWgpipeTry services{
      2u, sizeof(GekkoAOTNativeWgpipeTry), NativeTryWgpipeWrite, NativeWgpipeWindow};
  return services;
}

// Modules built before v372 accept only the version-1 layout.
const GekkoAOTNativeWgpipeTry& HostRuntime::NativeWgpipeTryServicesV1() {
  static const GekkoAOTNativeWgpipeTry services{
      1u, GEKKOAOT_NATIVE_WGPIPE_TRY_V1_SIZE, NativeTryWgpipeWrite, nullptr};
  return services;
}

// GEKKOAOT_WGPIPE_INLINE_GATHER_V372: generated code appends mid-line
// gather stores itself; TryGatherPartial still admits the first store of
// every line and the store that completes it. Host-effect counting (idle
// loop proof, runtime profile) and the glyph diagnostics need every store.
const GekkoAOTWgpipeWindow* HostRuntime::NativeWgpipeWindow(CPUState* cpu) {
  static const bool enabled = RuntimeEnvBool("GEKKOAOT_WGPIPE_INLINE", true) &&
      !std::getenv("GEKKOAOT_GX_DIAG_GLYPH_PC") && !std::getenv("GEKKOAOT_GX_DIAG_GLYPH_STATE");
  auto* self = Self(cpu);
  if (!enabled || !self || self->track_host_effects_ || !self->fast_wgpipe_enabled_) return nullptr;
  self->wgpipe_window_ = {self->gpu_fifo_gather_.data(), &self->gpu_fifo_gather_size_};
  static bool logged = false;
  if (!logged) {
    logged = true;
    std::fprintf(stderr, "GEKKOAOT_WGPIPE_INLINE_GATHER_V372=1 append=mid-line host=line-start+line-complete\n");
  }
  return &self->wgpipe_window_;
}

std::uint32_t HostRuntime::NativeTryWgpipeWrite(CPUState* cpu, std::uint32_t address,
    std::uint64_t value, std::uint8_t size, std::uint32_t pc) {
  auto* self = Self(cpu);
  if (self) self->CountHostEffect();
  if (!self || cpu->exception != 0u ||
      !GekkoAOT::GX::HostBridge::IsWriteGatherPipe(address)) return 0u;
  for(const auto& provider:self->reciprocal_camera_providers_)
    if(pc>=provider.projection_sdk && pc<provider.projection_sdk+164u)return 0u;
  // Only owner and pending-exception fields are read; there is no state publication.
  // In particular the current instruction's PC is an explicit argument.
  return self->TryGatherPartial(value, size, pc) ? 1u : 0u;
}

template<bool ProvenWgpipe>
void HostRuntime::WriteGatherOrExternal(CPUState* cpu, std::uint32_t address, std::uint64_t value,
                                      std::uint8_t size) {
  auto* self = Self(cpu);
  if (self) self->CountHostEffect();
  if(self && !self->reciprocal_camera_providers_.empty() &&
     (ProvenWgpipe || GekkoAOT::GX::HostBridge::IsWriteGatherPipe(address)))
    self->ServiceReciprocalFrustumSubmission();
  if (self && (ProvenWgpipe || GekkoAOT::GX::HostBridge::IsWriteGatherPipe(address)) &&
      self->TryGatherPartial(value, size, cpu->pc)) return;
  ExternalWriteSlow(cpu, address, value, size);
}

void HostRuntime::ExternalWrite(CPUState* cpu, std::uint32_t address, std::uint64_t value,
                                std::uint8_t size) {
  WriteGatherOrExternal<false>(cpu, address, value, size);
}

void HostRuntime::NativeWgpipeWrite(CPUState* cpu, std::uint32_t address,
                                    std::uint64_t value, std::uint8_t size) {
  // The LLVM proof admits only scalar stores in the fixed WG aperture.
  // Queue completion, RAM display-list capture and IRQ restart use the exact
  // slow path. No paired-store atomicity or quantisation is assumed here.
  WriteGatherOrExternal<true>(cpu, address, value, size);
}

#if defined(_MSC_VER)
__declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
__attribute__((noinline))
#endif
void HostRuntime::ExternalWriteSlow(CPUState* cpu, std::uint32_t address, std::uint64_t value,
                                std::uint8_t size) {
  if (auto* self = Self(cpu)) {
    if (self->runtime_profile_enabled_) {
      ++self->external_writes_[ExternalClass(address)];
      self->NoteExternalOther(address, true);
    }
    // v136b hot path: WGPIPE is a fixed MMIO aperture and cannot alias MEM1,
    // MEM2, paged VMEM or locked cache. Most draw-heavy ExternalWrite calls
    // land here, so handle it before generic address-space probes.
    // Partial lines remain inside the native superchain; only a complete
    // 32-byte gather line becomes a host-visible CP/GPU boundary.
    if (GekkoAOT::GX::HostBridge::IsWriteGatherPipe(address)) {
      // The generated memory-service boundary has materialized the current
      // instruction and all dirty state. Exit BEFORE accepting any bytes so
      // RFI retries exactly this store, retaining the partial physical gather.
      if (self->fifo_irq_fence_enabled_ &&
          self->cp_.FifoReadWriteDistance() > self->cp_.FifoHiWatermark() &&
          self->TryTakeFifoWriteInterrupt()) return;
      if (size == 4u && value == 0xc274d554u &&
          std::getenv("GEKKOAOT_GX_DIAG_GLYPH_PC"))
        std::fprintf(stderr, "GEKKOAOT_GX_DIAG_GLYPH_PC pc=%08x value=%08llx gather=%u pi_write=%08x\n",
                     cpu->pc, static_cast<unsigned long long>(value),
                     self->gpu_fifo_gather_size_, self->pi_.FifoWritePointer());
      if (size == 4u && cpu->pc == 0x800228c0u &&
          (value == 0xc274d554u || value == 0x80000000u) &&
          std::getenv("GEKKOAOT_GX_DIAG_GLYPH_STATE")) {
        static bool seen_normal = false, seen_zero = false;
        bool& seen = value == 0xc274d554u ? seen_normal : seen_zero;
        if (!seen) {
          seen = true;
          const std::uint32_t object = cpu->gpr[31];
          const auto* saved_lr_bytes = self->memory_.Resolve(cpu->gpr[1] + 180u, 4u);
          const std::uint32_t saved_lr = saved_lr_bytes
              ? (std::uint32_t(saved_lr_bytes[0]) << 24u) |
                    (std::uint32_t(saved_lr_bytes[1]) << 16u) |
                    (std::uint32_t(saved_lr_bytes[2]) << 8u) | saved_lr_bytes[3]
              : 0u;
          std::fprintf(stderr, "GEKKOAOT_GX_DIAG_GLYPH_STATE pc=%08x x=%08llx object=%08x sp=%08x caller=%08x r20=%08x r23=%08x r29=%08x r30=%08x f26=%.9g f27=%.9g f30=%.9g f31=%.9g bytes=",
                       cpu->pc, static_cast<unsigned long long>(value), object, cpu->gpr[1], saved_lr,
                       cpu->gpr[20], cpu->gpr[23], cpu->gpr[29], cpu->gpr[30],
                       cpu->fpr[26], cpu->fpr[27], cpu->fpr[30], cpu->fpr[31]);
          const auto* object_bytes = self->memory_.Resolve(object, 112u);
          if (object_bytes) for (unsigned i = 0; i < 112u; ++i)
            std::fprintf(stderr, "%02x", object_bytes[i]);
          std::fprintf(stderr, " caller_bytes=");
          const auto* caller_bytes = self->memory_.Resolve(cpu->gpr[20], 64u);
          if (caller_bytes) for (unsigned i = 0; i < 64u; ++i)
            std::fprintf(stderr, "%02x", caller_bytes[i]);
          std::fprintf(stderr, " caller_stack=");
          const auto* caller_stack = self->memory_.Resolve(cpu->gpr[1] + 176u + 32u, 40u);
          if (caller_stack) for (unsigned i = 0; i < 40u; ++i)
            std::fprintf(stderr, "%02x", caller_stack[i]);
          std::fprintf(stderr, "\n");
        }
      }
      // The CPU write-gather pipe always targets the currently selected PI CPU
      // FIFO.  During GXBeginDisplayList that FIFO is a temporary RAM buffer,
      // not the CP/GP render FIFO.  Record complete 32-byte gather bursts into
      // MEM1 and leave Aurora untouched; GXCallDisplayList will later replay
      // the recorded bytes through the native CALL_DL path.
      if (!self->CpuFifoTargetsGpu()) {
        const std::uint64_t before = self->cpu_fifo_capture_bursts_;
        if (self->CaptureWriteGatherToCpuFifo(value, size)) {
          if (self->cpu_fifo_capture_bursts_ != before) BumpHostWriteEpoch(cpu);
          return;
        }
      } else if (self->cpu_fifo_gather_size_ != 0u) {
        if (self->gpu_fifo_gather_size_ != 0u) {
          self->SetFault(FaultKind::MmioWrite, address, self->cpu_fifo_gather_size_);
          return;
        }
        std::copy_n(self->cpu_fifo_gather_.begin(), self->cpu_fifo_gather_size_,
                    self->gpu_fifo_gather_.begin());
        self->gpu_fifo_gather_size_ = self->cpu_fifo_gather_size_;
        self->gpu_fifo_first_pc_ = cpu->pc;
        self->cpu_fifo_gather_size_ = 0u;
      }

      // The overwhelmingly common retail GX store is a 32-bit WGPIPE
      // write that does not complete the current 32-byte gather line. Keep
      // that partial-line case in this already-hot ExternalWrite path instead
      // of entering the much larger QueueGpuFifoGather() helper. The store
      // that completes the line still uses the authoritative helper below, so
      // PI/CP visibility, host-write epochs, breakpoints and renderer ordering
      // remain byte-for-byte identical.
      if (self->fast_wgpipe_enabled_ && self->native_gx_.Ready() &&
          (size == 1u || size == 2u || size == 4u || size == 8u)) {
        const std::size_t room =
            self->gpu_fifo_gather_.size() - self->gpu_fifo_gather_size_;
        if (room > size) {
          if (self->gpu_fifo_gather_size_ == 0u) self->gpu_fifo_first_pc_ = cpu->pc;
          auto* out = self->gpu_fifo_gather_.data() + self->gpu_fifo_gather_size_;
          // CoD Finest Hour's hottest renderer loop uses `sth` to the
          // write-gather pipe.  The old v184 shortcut only covered 32-bit
          // writes, sending every 8/16/64-bit partial store through the larger
          // QueueGpuFifoGather() path. Keep each scalar width branchless after
          // selection so the existing 32-bit path does not regress.
          switch (size) {
          case 1u:
            out[0] = static_cast<std::uint8_t>(value);
            break;
          case 2u:
            out[0] = static_cast<std::uint8_t>(value >> 8u);
            out[1] = static_cast<std::uint8_t>(value);
            break;
          case 4u:
            out[0] = static_cast<std::uint8_t>(value >> 24u);
            out[1] = static_cast<std::uint8_t>(value >> 16u);
            out[2] = static_cast<std::uint8_t>(value >> 8u);
            out[3] = static_cast<std::uint8_t>(value);
            break;
          case 8u:
            out[0] = static_cast<std::uint8_t>(value >> 56u);
            out[1] = static_cast<std::uint8_t>(value >> 48u);
            out[2] = static_cast<std::uint8_t>(value >> 40u);
            out[3] = static_cast<std::uint8_t>(value >> 32u);
            out[4] = static_cast<std::uint8_t>(value >> 24u);
            out[5] = static_cast<std::uint8_t>(value >> 16u);
            out[6] = static_cast<std::uint8_t>(value >> 8u);
            out[7] = static_cast<std::uint8_t>(value);
            break;
          }
          self->gpu_fifo_gather_size_ += size;
          return;
        }
      }

      const std::uint64_t before = self->gpu_fifo_bursts_;
      if (self->QueueGpuFifoGather(value, size, cpu->pc)) {
        // A complete write-gather line is the actual hardware visibility
        // boundary. Partial lines stay inside the native superchain.
        if (self->gpu_fifo_bursts_ != before) BumpHostWriteEpoch(cpu);
        return;
      }
    }
    const std::uint32_t physical = AddressSpace::ToPhysical(address);
    const bool flipper_mmio = physical >= 0x0c000000u && physical < 0x0c007000u;
    // Fixed Flipper MMIO pages cannot alias guest RAM or locked cache.
    // Skip three generic mapping probes on every CP/PE/VI/PI/MI/DSP/DI/SI/EXI/AI write.
    if (!flipper_mmio) {
      if (self->AccessPagedVmem(address, &value, size, true)) return;
      // See ExternalRead(): compiled OS interrupt handlers may carry physical
      // MEM1 pointers out of the low-memory vector while translation is restored.
      if (WriteAddressSpaceValue(self->memory_, address, value, size)) return;
      if (self->lc_.Write(address, value, size)) {
        BumpHostWriteEpoch(cpu);
        return;
      }
    }

    if(self->RejectHardwareAccess(address,true)) return;
    if (flipper_mmio) {
      const auto page = physical & 0xfffff000u;
      if (page == HW::CP::NativeCP::BasePhysical ||
          page == HW::PE::NativePE::BasePhysical ||
          page == HW::PI::NativePI::BasePhysical) {
        (void)self->native_gx_.FlushWrites(GX::HostBridge::SyncReason::MmioWrite);
        self->SyncNativeGXInterrupts();
      }
    }

    BumpHostWriteEpoch(cpu);
    switch (physical & 0xfffff000u) {
    case HW::CP::NativeCP::BasePhysical:
      if (self->cp_.Write(address, value, size)) {
        self->SyncNativeCPInterrupt();
        return;
      }
      break;
    case HW::PE::NativePE::BasePhysical:
      if (self->pe_.Write(address, value, size)) {
        self->SyncNativePEInterrupts();
        return;
      }
      break;
    case HW::VI::NativeVI::BasePhysical:
      if (self->vi_.Write(address, value, size)) {
        if (self->vi_.ConsumeInterruptUpdate()) self->SyncNativeVIInterrupt();
        return;
      }
      break;
    case HW::PI::NativePI::BasePhysical:
      if (self->pi_.Write(address, value, size)) return;
      break;
    case HW::MI::NativeMI::BasePhysical:
      if (self->mi_.Write(address, value, size)) return;
      break;
    case DSP::NativeDSP::BasePhysical:
      if (self->dsp_.Write(address, value, size)) {
        self->SyncNativeDSPInterrupt();
        return;
      }
      break;
    case 0x0c006000u:
      switch (physical & 0x00000f00u) {
      case 0x000u:
        if (self->di_.Write(address, value, size)) {
          if (self->di_async_completion_pending_ &&
              (self->di_.DMAControl() & 1u) == 0u) {
            self->di_async_completion_pending_ = false;
            self->di_io_request_queued_ = false;
            self->di_async_cycles_remaining_ = 0u;
            self->di_async_staging_.clear();
            static unsigned cancel_logs = 0u;
            if (EnvTraceDvd() && cancel_logs++ < 32u)
              std::fprintf(stderr,
                           "GEKKOAOT_NATIVE_DI_ASYNC_V98=1 phase=cancel-immediate seq=%llu pc=%08x\n",
                           static_cast<unsigned long long>(self->di_async_sequence_), cpu->pc);
          }
          self->SyncNativeDIInterrupt();
          return;
        }
        break;
      case 0x400u:
        if (self->si_.Write(address, value, size)) {
          self->SyncNativeSIInterrupt();
          return;
        }
        break;
      case 0x800u:
        if (self->exi_.WriteRegister(address, value, size)) {
          self->SyncNativeEXIInterrupt();
          return;
        }
        break;
      case 0xc00u:
        if (self->ai_.Write(address, value, size)) {
          self->SyncNativeAIInterrupt();
          return;
        }
        break;
      default:
        break;
      }
      break;
    default:
      break;
    }
    if (physical >= 0x08000000u && physical < 0x0c000000u) {
      if (size == 4u && (physical & 0x00800000u) == 0u &&
          self->native_gx_.Ready()) {
        const auto x = static_cast<std::uint16_t>((physical & 0xfffu) >> 2u);
        const auto y = static_cast<std::uint16_t>((physical >> 12u) & 0x3ffu);
        if (self->native_gx_.PokeEfb(x, y, static_cast<std::uint32_t>(value),
                                     (physical & 0x00400000u) != 0u)) {
          static unsigned efb_poke_logs = 0u;
          if (efb_poke_logs++ < 16u)
            std::fprintf(stderr,
                         "GEKKOAOT_EFB_POKE_V164=1 ea=%08x x=%u y=%u depth=%u value=%08x pc=%08x\n",
                         address, unsigned(x), unsigned(y),
                         (physical & 0x00400000u) != 0u ? 1u : 0u,
                         static_cast<std::uint32_t>(value), cpu->pc);
          BumpHostWriteEpoch(cpu);
          return;
        }
      }
      static unsigned efb_write_logs = 0;
      if (efb_write_logs++ < 32u)
        std::fprintf(stderr,
                     "GEKKOAOT_EFB_APERTURE_V87=0 op=write ea=%08x physical=%08x size=%u value=%016llx pc=%08x lr=%08x reason=cpu-efb-access-not-implemented action=fault\n",
                     address, physical, unsigned(size),
                     static_cast<unsigned long long>(value), cpu->pc, cpu->lr);
    }
    if (LooksLikeInvalidGameCubeRamAddress(self->memory_, address)) {
      std::fprintf(stderr,
                   "GEKKOAOT_INVALID_GUEST_EA_V86=1 op=write ea=%08x physical=%08x size=%u value=%016llx pc=%08x lr=%08x sp=%08x r3=%08x r4=%08x r9=%08x r10=%08x r11=%08x action=fault\n",
                   address, AddressSpace::ToPhysical(address), unsigned(size),
                   static_cast<unsigned long long>(value), cpu->pc, cpu->lr,
                   cpu->gpr[1], cpu->gpr[3], cpu->gpr[4], cpu->gpr[9],
                   cpu->gpr[10], cpu->gpr[11]);
      self->SetFault(FaultKind::InvalidMemoryWrite, address,
                     static_cast<std::uint32_t>(value));
    } else {
      self->SetFault(FaultKind::MmioWrite, address, static_cast<std::uint32_t>(value));
    }
  }
}

std::uint32_t HostRuntime::ExternalRead32(CPUState* cpu, std::uint32_t address, std::uint8_t) {
  return static_cast<std::uint32_t>(ExternalRead(cpu, address, 4));
}

void HostRuntime::ExternalWrite32(CPUState* cpu, std::uint32_t address, std::uint32_t value,
                                  std::uint8_t) {
  ExternalWrite(cpu, address, value, 4);
}

void* HostRuntime::ExternalPointer(CPUState* cpu, std::uint32_t address, std::uint32_t size) {
  auto* self = Self(cpu);
  if (!self) return nullptr;
  // Essentially every NativeOS/SDK pointer lives in MEM1/MEM2. Resolve
  // normal RAM first so the hot path avoids probing the locked-cache aperture
  // on every guest structure read. LC addresses do not alias AddressSpace, so
  // this preserves the exact fallback semantics for 0xE0000000..0xE003FFFF.
  if (void* pointer = self->memory_.Resolve(address, size)) return pointer;
  // Native SDK objects may reside in the game's paged heap. A raw host
  // pointer is valid only when every guest page is writable and physically
  // contiguous; never expose the unrelated flat VM backing after SDR1 setup.
  if (self->memory_.PagedVmem() && AddressSpace::IsVmem(address)) {
    std::vector<GuestSpan> spans;
    if (self->TranslateGuestRange(address, size, true, spans) != GuestRangeStatus::Ok || spans.empty())
      return nullptr;
    auto* next = spans.front().host;
    for (const auto& span : spans) {
      if (span.host != next) return nullptr;
      next += span.length;
    }
    return spans.front().host;
  }
  return self->lc_.Pointer(address, size);
}

void HostRuntime::InstructionFallback(CPUState* cpu, std::uint32_t instruction,
                                      std::uint32_t address) {
  auto* self = Self(cpu);
  if (!self) return;
  self->CountHostEffect();

  // Keep the standalone fallback useful as a correctness safety net for AOT
  // backends that emitted ppc_fallback_instruction() for floating-point memory
  // operations.  These instructions are still executed natively here; there is
  // no PowerPC interpreter behind this path.  Current pinned DolRecomp lowers
  // them directly, so a clean/fresh module should normally never hit this code.
  //
  // This also makes stale modules deterministic instead of spinning forever at
  // e.g. NDDEMO 0x80062AC0 (lfs f0,0(r4)).
  const auto read32 = [&](std::uint32_t ea, std::uint32_t* value) {
    if (self->memory_.Read32(ea, value)) return true;
    *value = static_cast<std::uint32_t>(ExternalRead(cpu, ea, 4u));
    return self->fault_.kind == FaultKind::None && cpu->exception == 0u;
  };
  const auto read64 = [&](std::uint32_t ea, std::uint64_t* value) {
    if (self->memory_.Read64(ea, value)) return true;
    *value = ExternalRead(cpu, ea, 8u);
    return self->fault_.kind == FaultKind::None && cpu->exception == 0u;
  };
  const auto write32 = [&](std::uint32_t ea, std::uint32_t value) {
    if (self->memory_.Write32(ea, value)) return true;
    ExternalWrite(cpu, ea, value, 4u);
    return self->fault_.kind == FaultKind::None && cpu->exception == 0u;
  };
  const auto write64 = [&](std::uint32_t ea, std::uint64_t value) {
    if (self->memory_.Write64(ea, value)) return true;
    ExternalWrite(cpu, ea, value, 8u);
    return self->fault_.kind == FaultKind::None && cpu->exception == 0u;
  };
  const auto load_float = [&](std::uint32_t fr, std::uint32_t ea, bool single) {
    if (single) {
      std::uint32_t bits = 0;
      if (!read32(ea, &bits)) return false;
      float value = 0.0f;
      std::memcpy(&value, &bits, sizeof(value));
      const double expanded = static_cast<double>(value);
      cpu->fpr[fr] = expanded;
      // Gekko lfs fills both halves of the paired-single register.
      cpu->ps1[fr] = expanded;
      return true;
    }
    std::uint64_t bits = 0;
    if (!read64(ea, &bits)) return false;
    double value = 0.0;
    std::memcpy(&value, &bits, sizeof(value));
    // lfd writes PS0 only; PS1 is architecturally preserved.
    cpu->fpr[fr] = value;
    return true;
  };
  const auto store_float = [&](std::uint32_t fr, std::uint32_t ea, bool single) {
    if (single) {
      const float value = static_cast<float>(cpu->fpr[fr]);
      std::uint32_t bits = 0;
      std::memcpy(&bits, &value, sizeof(bits));
      return write32(ea, bits);
    }
    std::uint64_t bits = 0;
    std::memcpy(&bits, &cpu->fpr[fr], sizeof(bits));
    return write64(ea, bits);
  };
  const auto finish_fp_memory = [&](std::uint32_t fr, std::uint32_t ra,
                                    std::uint32_t ea, bool load, bool single,
                                    bool update) {
    const bool ok = load ? load_float(fr, ea, single)
                         : store_float(fr, ea, single);
    if (!ok) return true;
    if (update) cpu->gpr[ra] = ea;
    cpu->pc = address + 4u;
    return true;
  };

  const std::uint32_t primary = instruction >> 26;
  // AOT can route a conditional branch through this callback when a live
  // image's instruction at the target PC differs from the compiled image
  // (for example, Rising Sun's NativeOS exception/context path).  Execute the
  // guest branch with the same CR/CTR/AA/LK semantics as the normal native
  // branch path instead of treating it as an unsupported instruction.
  if (primary == 16u) { // bc / bcl and aliases such as beq/bne/bdnz
    const std::uint32_t bo = (instruction >> 21) & 31u;
    const std::uint32_t bi = (instruction >> 16) & 31u;
    bool ctr_ok = true;
    if ((bo & 0x4u) == 0u) {
      cpu->ctr -= 1u;
      ctr_ok = (cpu->ctr != 0u) != ((bo & 0x2u) != 0u);
    }
    const bool cr_ok = (bo & 0x10u) != 0u ||
                       CrBit(cpu, bi) == ((bo & 0x8u) != 0u);
    if ((instruction & 1u) != 0u) cpu->lr = address + 4u;
    if (ctr_ok && cr_ok) {
      const auto displacement = static_cast<std::int32_t>(
          static_cast<std::int16_t>(instruction & 0xfffcu));
      const std::uint32_t base = (instruction & 2u) != 0u ? 0u : address;
      cpu->pc = base + static_cast<std::uint32_t>(displacement);
    } else {
      cpu->pc = address + 4u;
    }
    return;
  }

  if (primary >= 24u && primary <= 29u) {
    const std::uint32_t rs = (instruction >> 21) & 31u;
    const std::uint32_t ra = (instruction >> 16) & 31u;
    const std::uint32_t immediate = instruction & 0xffffu;
    std::uint32_t value = cpu->gpr[rs];
    switch (primary) {
    case 24u: value |= immediate; break;                         // ori
    case 25u: value |= immediate << 16; break;                   // oris
    case 26u: value ^= immediate; break;                         // xori
    case 27u: value ^= immediate << 16; break;                   // xoris
    case 28u: value &= immediate; break;                         // andi.
    case 29u: value &= immediate << 16; break;                   // andis.
    }
    cpu->gpr[ra] = value;
    if (primary == 28u || primary == 29u) SetCr0FromResult(cpu, value);
    cpu->pc = address + 4u;
    return;
  }

  if (primary == kPpcPrimaryX) {
    const std::uint32_t xo = (instruction >> 1) & 0x3ffu;
    if (xo == 444u) { // or (mr is or rA,rS,rS)
      const std::uint32_t rs = (instruction >> 21) & 31u;
      const std::uint32_t ra = (instruction >> 16) & 31u;
      const std::uint32_t rb = (instruction >> 11) & 31u;
      const std::uint32_t value = cpu->gpr[rs] | cpu->gpr[rb];
      cpu->gpr[ra] = value;
      if ((instruction & 1u) != 0u) SetCr0FromResult(cpu, value);
      cpu->pc = address + 4u;
      return;
    }
  }

  if (primary >= 48u && primary <= 55u) {
    const std::uint32_t fr = (instruction >> 21) & 31u;
    const std::uint32_t ra = (instruction >> 16) & 31u;
    const bool update = (primary & 1u) != 0u;
    const bool load = primary <= 51u;
    const bool single = primary == 48u || primary == 49u ||
                        primary == 52u || primary == 53u;
    if (update && ra == 0u) {
      self->SetFault(FaultKind::InstructionFallback, address, instruction);
      return;
    }
    const auto displacement = static_cast<std::int32_t>(
        static_cast<std::int16_t>(instruction & 0xffffu));
    const std::uint32_t base = ra == 0u && !update ? 0u : cpu->gpr[ra];
    const std::uint32_t ea = base + static_cast<std::uint32_t>(displacement);
    (void)finish_fp_memory(fr, ra, ea, load, single, update);
    return;
  }

  if (primary == kPpcPrimaryX && (instruction & 1u) == 0u) {
    const std::uint32_t xo = (instruction >> 1) & 0x3ffu;
    const std::uint32_t fr = (instruction >> 21) & 31u;
    const std::uint32_t ra = (instruction >> 16) & 31u;
    const std::uint32_t rb = (instruction >> 11) & 31u;
    bool recognized = true;
    bool load = false;
    bool single = false;
    bool update = false;
    switch (xo) {
    case 535u: load = true; single = true; break;       // lfsx
    case 567u: load = true; single = true; update = true; break;  // lfsux
    case 599u: load = true; break;                      // lfdx
    case 631u: load = true; update = true; break;       // lfdux
    case 663u: single = true; break;                    // stfsx
    case 695u: single = true; update = true; break;     // stfsux
    case 727u: break;                                   // stfdx
    case 759u: update = true; break;                    // stfdux
    case 983u: {                                        // stfiwx
      const std::uint32_t base = ra == 0u ? 0u : cpu->gpr[ra];
      const std::uint32_t ea = base + cpu->gpr[rb];
      std::uint64_t bits = 0;
      std::memcpy(&bits, &cpu->fpr[fr], sizeof(bits));
      if (write32(ea, static_cast<std::uint32_t>(bits))) cpu->pc = address + 4u;
      return;
    }
    default: recognized = false; break;
    }
    if (recognized) {
      if (update && ra == 0u) {
        self->SetFault(FaultKind::InstructionFallback, address, instruction);
        return;
      }
      const std::uint32_t base = ra == 0u && !update ? 0u : cpu->gpr[ra];
      const std::uint32_t ea = base + cpu->gpr[rb];
      (void)finish_fp_memory(fr, ra, ea, load, single, update);
      return;
    }
  }

  // Indirect branches can land on an instruction inside an AOT function
  // instead of one of its compiled CFG entry labels. DolRecomp routes such
  // interior entries through this callback, even when the opcode itself is
  // fully supported. Execute the XL branch directly so a table target that
  // points at a standalone blr can return to its caller.
  if (primary == 19u) {
    const std::uint32_t xo = (instruction >> 1) & 0x3ffu;
    const std::uint32_t bo = (instruction >> 21) & 31u;
    if (xo == 16u || (xo == 528u && (bo & 4u) != 0u)) { // bclr / bcctr
      const std::uint32_t bi = (instruction >> 16) & 31u;
      const std::uint32_t target = (xo == 16u ? cpu->lr : cpu->ctr) & ~3u;
      bool ctr_ok = true;
      if ((bo & 4u) == 0u) {
        cpu->ctr -= 1u;
        ctr_ok = (cpu->ctr != 0u) != ((bo & 2u) != 0u);
      }
      const bool cr_ok = (bo & 16u) != 0u ||
                         CrBit(cpu, bi) == ((bo & 8u) != 0u);
      if ((instruction & 1u) != 0u) cpu->lr = address + 4u;
      cpu->pc = ctr_ok && cr_ok ? target : address + 4u;
      return;
    }
  }

  std::uint8_t cache_operation = 0;
  std::uint32_t effective_address = 0;
  if (DecodeCacheControl(instruction, cpu, &cache_operation, &effective_address)) {
    CacheControl(cpu, cache_operation, effective_address, address);
    // DolRecomp Portable-C returns immediately after ppc_fallback_instruction(),
    // so resume at the following guest instruction just like a one-step
    // interpreter fallback would do.
    cpu->pc = address + 4u;
    return;
  }

  bool spr_write = false;
  std::uint16_t spr = 0;
  std::uint8_t gpr = 0;
  if (DecodeSprTransfer(instruction, &spr_write, &spr, &gpr)) {
    if (spr_write) {
      SprWrite(cpu, spr, cpu->gpr[gpr], address);
    } else {
      cpu->gpr[gpr] = SprRead(cpu, spr, address);
    }
    if (self->fault_.kind == FaultKind::None) cpu->pc = address + 4u;
    return;
  }

  self->SetFault(FaultKind::InstructionFallback, address, instruction);
}

} // namespace GekkoAOT::Native
