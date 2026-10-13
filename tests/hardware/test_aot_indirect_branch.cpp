// SPDX-License-Identifier: GPL-3.0-or-later
#include "native/runtime.h"
#include "native/rel_identity.h"

#include <cstdlib>
#include <iostream>
#include <memory>
#include <vector>
#include <chrono>

#define CHECK(x) do { if (!(x)) { std::cerr << __LINE__ << ": " #x "\n"; std::exit(1); } } while (false)

using GekkoAOT::Native::HostRuntime;

static void VerifyRelSyntheticAddressValidation() {
  GekkoAOTRange range{0x81800100u, 0x81800200u};
  std::uint64_t hash = 0;
  GekkoAOTRelSection section{235u, 1u, range.start, 0x100u};
  GekkoAOTRelModule rel{235u, 3u, 2u, 0x4cu, 0x200u, &section, 1u};
  GekkoAOTModuleDesc descriptor{};
  descriptor.abi_version = GEKKOAOT_MODULE_ABI_VERSION;
  descriptor.cpu_abi_version = GEKKOAOT_CPU_ABI_VERSION;
  descriptor.cpu_state_size = sizeof(CPUState);
  descriptor.game_id[0] = 'T';
  descriptor.entry_point = range.start;
  descriptor.dispatch = +[](CPUState*, std::uint32_t) { return 1; };
  descriptor.code_ranges = descriptor.chunk_ranges = &range;
  descriptor.num_code_ranges = descriptor.num_chunk_ranges = 1;
  descriptor.chunk_hashes = &hash;
  descriptor.rel_modules = &rel;
  descriptor.num_rel_modules = 1;
  using GekkoAOT::Native::ValidateModule;
  using GekkoAOT::Native::ModuleStatus;
  CHECK(ValidateModule(&descriptor) == ModuleStatus::Ok);
  section.size = 0x80000000u; // Reject overflow in the synthetic namespace.
  CHECK(ValidateModule(&descriptor) == ModuleStatus::InvalidRelModules);
  section.size = 0x100u;
  section.section_index = 2u;
  CHECK(ValidateModule(&descriptor) == ModuleStatus::InvalidRelModules);
  section.section_index = 1u;
  section.module_id = 236u;
  CHECK(ValidateModule(&descriptor) == ModuleStatus::InvalidRelModules);
}

static void VerifyRelImportIdentity() {
  std::vector<std::uint8_t> ram(0x1000u);
  const auto put = [&](std::size_t at, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) ram[at+i] = value >> (24u-i*8u);
  };
  put(0x128u, 0x80000200u); put(0x12cu, 8u);
  put(0x200u, 0u); put(0x204u, 0x80000300u);
  // External DOL ADDR32 import followed by END, intact after OSLink.
  ram[0x302u] = 1u; put(0x304u, 0x8035a50cu); ram[0x30au] = 203u;
  std::uint64_t hash = 0xcbf29ce484222325ull;
  for (unsigned i=0; i<16; ++i) { hash ^= ram[0x300u+i]; hash *= 1099511628211ull; }
  const GekkoAOTRelImportProof proof{0u, 16u, hash};
  CHECK(GekkoAOT::Native::MatchRelImportProofs(ram, 0x100u, &proof, 1u));
  // Same section sizes and executable bytes, different main-program imports.
  put(0x304u, 0x80367930u);
  CHECK(!GekkoAOT::Native::MatchRelImportProofs(ram, 0x100u, &proof, 1u));
  put(0x304u, 0x8035a50cu);
  put(0x128u, 0x100u); put(0x204u, 0x200u); // Before OSLink, relative pointers.
  CHECK(GekkoAOT::Native::MatchRelImportProofs(ram, 0x100u, &proof, 1u));
  put(0x204u, 0x80000ff8u);
  CHECK(!GekkoAOT::Native::MatchRelImportProofs(ram, 0x100u, &proof, 1u));
}

namespace GekkoAOT::Native {
struct HardwareTestAccess {
  static void VerifyCopiedCacheStub(HostRuntime& runtime) {
    constexpr std::uint32_t pc=0x812fff80u, sp=0x81329870u;
    constexpr std::uint32_t code[] = {
      0x3c808120u,0x38840000u,0x3ca00010u,0x38a50000u,
      0x54860034u,0x7ce42a14u,0x7ce63850u,0x38e7001fu,0x54e7d97eu,
      0x7ce903a6u,0x7c0037acu,0x38c60020u,0x4200fff8u,0x7c0004acu,
      0x4c00012cu,0x8001000cu,0x7c0803a6u,0x38210008u,0x4e800020u};
    auto& cpu=runtime.Cpu();
    for (unsigned i=0; i<19; ++i) CHECK(runtime.Memory().Write32(pc+4u*i,code[i]));
    CHECK(runtime.Memory().Write32(sp+12u,0x8120026bu));
    cpu.gpr[1]=sp; cpu.gpr[3]=0x12345678u; cpu.cr=0x12345678u;
    cpu.msr=0; cpu.pc=pc; cpu.lr=pc; cpu.ctr=42;
    std::uint64_t cycles=0;
    CHECK(runtime.DispatchCopiedCacheStub(pc,&cycles));
    CHECK(cpu.pc==0x81200268u && cpu.lr==0x8120026bu && cpu.gpr[0]==cpu.lr);
    CHECK(cpu.gpr[1]==sp+8u && cpu.ctr==0 && cpu.cr==0x12345678u);
    CHECK(cpu.gpr[3]==0x12345678u && cpu.gpr[4]==0x81200000u);
    CHECK(cpu.gpr[5]==0x100000u && cpu.gpr[6]==0x81300000u && cpu.gpr[7]==32768u);
    CHECK(cycles==16u+32768u*3u && runtime.rel_mapping_dirty_);
    // A modified instruction is arbitrary guest code, never this native leaf.
    cpu.gpr[1]=sp; cpu.pc=pc; cpu.ctr=42;
    CHECK(runtime.Memory().Write32(pc+40u,0x60000000u));
    CHECK(!runtime.DispatchCopiedCacheStub(pc,&cycles));
    CHECK(cpu.pc==pc && cpu.gpr[1]==sp && cpu.ctr==42);
    CHECK(runtime.Memory().Write32(pc+40u,code[10]));
    // Signed ADDI patches and partial first/last cache lines must be preserved.
    CHECK(runtime.Memory().Write32(pc,0x3c808121u));
    CHECK(runtime.Memory().Write32(pc+4u,0x3884ffffu));
    CHECK(runtime.Memory().Write32(pc+8u,0x3ca00000u));
    CHECK(runtime.Memory().Write32(pc+12u,0x38a50002u));
    CHECK(runtime.DispatchCopiedCacheStub(pc,&cycles));
    CHECK(cpu.gpr[4]==0x8120ffffu && cpu.gpr[5]==2u && cpu.gpr[7]==2u);
    CHECK(cpu.gpr[6]==0x81210020u && cycles==22u);
    cpu.gpr[1]=0xfffffff8u; cpu.pc=pc;
    CHECK(!runtime.DispatchCopiedCacheStub(pc,&cycles));
    CHECK(cpu.gpr[1]==0xfffffff8u && cpu.pc==pc);
    runtime.Reset();
  }
  static void VerifyDispatchProbePublication(HostRuntime& runtime) {
    auto& first = runtime.ProbeDispatchCode(0x80010000u);
    first.idle_state = 2u; first.idle_offset = -4096; first.time_kind = 3u;
    CHECK(runtime.ProbeDispatchCode(0x80010000u).idle_offset == -4096);
    // Different PCs can occupy the same direct-mapped slot. Neither kind of
    // code classification may survive the replacement.
    const auto slot = [](std::uint32_t pc) {
      return ((pc >> 2u) ^ (pc >> 13u)) & (HostRuntime::kDispatchCodeProbeCacheSize - 1u);
    };
    std::uint32_t collision = 0x80010004u;
    while (slot(collision) != slot(0x80010000u)) collision += 4u;
    const auto& replaced = runtime.ProbeDispatchCode(collision);
    CHECK(replaced.idle_state == 0u && replaced.time_kind == 0u);
    auto& second = runtime.ProbeDispatchCode(0x80010000u);
    second.idle_state = 1u; second.time_kind = 2u;
    // icbi/ICInvalidateRange of the probed line is the publication boundary.
    runtime.InvalidateInstructionRange(0x80010000u, 32u);
    const auto& published = runtime.ProbeDispatchCode(0x80010000u);
    CHECK(published.idle_state == 0u && published.time_kind == 0u);
    // The probe decodes [pc-4, pc+24): publishing the previous line reaches it
    // through pc-4, while a distant line leaves the classification intact.
    runtime.ProbeDispatchCode(0x80010000u).time_kind = 3u;
    runtime.InvalidateInstructionRange(0x8000ffe0u, 32u);
    CHECK(runtime.ProbeDispatchCode(0x80010000u).time_kind == 0u);
    runtime.ProbeDispatchCode(0x80010000u).time_kind = 3u;
    runtime.InvalidateInstructionRange(0x80020000u, 32u);
    CHECK(runtime.ProbeDispatchCode(0x80010000u).time_kind == 3u);
    // The uncached alias publishes the same physical line.
    runtime.InvalidateInstructionRange(0xc0010000u, 32u);
    CHECK(runtime.ProbeDispatchCode(0x80010000u).time_kind == 0u);
    // Physical/paged addresses (and ICFI at EA 0) keep the full flush.
    runtime.ProbeDispatchCode(0x80010000u).time_kind = 3u;
    runtime.InvalidateInstructionRange(0x00000000u, 32u);
    CHECK(runtime.ProbeDispatchCode(0x80010000u).time_kind == 0u);
    runtime.ProbeDispatchCode(0x80010000u).time_kind = 3u;
    runtime.Reset();
    CHECK(runtime.ProbeDispatchCode(0x80010000u).time_kind == 0u);
  }
  static int RelMainExit(CPUState* cpu, std::uint32_t address) {
    CHECK(address == 0x805000ccu);
    cpu->gpr[6] = 0x805a0000u; // Partial literal / scalar, not a pointer.
    cpu->gpr[30] = 0x812c0260u; // Already materialized by relocation codegen.
    cpu->lr = 0x805000d8u;
    cpu->pc = 0x80010000u;
    return 1;
  }
  static int RelFpuTrap(CPUState* cpu, std::uint32_t address) {
    CHECK(address == 0x805000ccu);
    // An interrupt between LIS and ADDI must preserve the unfinished base.
    // The BSS target is linked separately from initialized data by OSLink.
    cpu->gpr[6] = 0x805a0000u;
    cpu->gpr[30] = 0x805a3c18u;
    cpu->srr0 = 0x805000d8u;
    cpu->pc = 0x800u;
    return 1;
  }
  static void VerifyEeEnableQuery(HostRuntime& runtime) {
    // Setting MSR[EE] needs a host boundary only for an already latched
    // interrupt (or with NativeOS hooks / the policy disabled).
    runtime.Reset();
    CHECK(!runtime.EeEnableNeedsBoundary());
    runtime.decrementer_pending_ = true;
    CHECK(runtime.EeEnableNeedsBoundary());
    runtime.decrementer_pending_ = false;
    runtime.ee_pending_query_enabled_ = false;
    CHECK(runtime.EeEnableNeedsBoundary());
    runtime.ee_pending_query_enabled_ = true;
    CHECK(!runtime.EeEnableNeedsBoundary());
    runtime.Reset();
  }
  static void VerifyLeafTail(HostRuntime& runtime) {
    auto& cpu = runtime.Cpu();
    constexpr std::uint32_t pc = 0x80020000u;
    const auto put = [&](std::initializer_list<std::uint32_t> words) {
      std::uint32_t at = pc;
      for (const auto word : words) { CHECK(runtime.Memory().Write32(at, word)); at += 4u; }
    };
    // OSDisableInterrupts tail after its mtmsr: rlwinm r3,r3,17,31,31; blr.
    put({0x54638ffeu, 0x4e800020u});
    cpu.gpr[3] = 0x00009032u; cpu.lr = 0x80001234u; cpu.pc = pc;
    CHECK(runtime.TryExecuteLeafTail(pc, false) == 2u && cpu.gpr[3] == 0x00009032u);
    CHECK(runtime.TryExecuteLeafTail(pc, true) == 2u);
    CHECK(cpu.gpr[3] == 1u && cpu.pc == 0x80001234u);
    // Wrapping mask (MB > ME): rlwinm r4,r5,8,28,3 keeps bits 28..31 and 0..3.
    // addi r6,0,-2; oris r7,r6,0x1234; or r8,r4,r7; blr.
    put({0x54a44706u, 0x38c0fffeu, 0x64c71234u, 0x7c883b78u, 0x4e800020u});
    cpu.gpr[5] = 0x12345678u; cpu.lr = 0x80005678u;
    CHECK(runtime.TryExecuteLeafTail(pc, true) == 5u);
    const std::uint32_t rot = (0x12345678u << 8u) | (0x12345678u >> 24u);
    CHECK(cpu.gpr[4] == (rot & 0xf000000fu));
    CHECK(cpu.gpr[6] == 0xfffffffeu && cpu.gpr[7] == 0xfffffffeu);
    CHECK(cpu.gpr[8] == (cpu.gpr[4] | cpu.gpr[7]) && cpu.pc == 0x80005678u);
    // Record forms (CR0), loads and over-long tails are rejected untouched.
    cpu.gpr[3] = 0x9032u; cpu.pc = pc;
    put({0x54638fffu, 0x4e800020u});
    CHECK(runtime.TryExecuteLeafTail(pc, true) == 0u && cpu.gpr[3] == 0x9032u && cpu.pc == pc);
    put({0x80630000u, 0x4e800020u});
    CHECK(runtime.TryExecuteLeafTail(pc, true) == 0u && cpu.pc == pc);
    put({0x60000000u, 0x60000000u, 0x60000000u, 0x60000000u, 0x60000000u, 0x4e800020u});
    CHECK(runtime.TryExecuteLeafTail(pc, true) == 0u && cpu.pc == pc);
    runtime.Reset();
  }
  static void VerifyRelExceptionRegisters(HostRuntime& runtime) {
    const GekkoAOTRange code[] = {{0x805000ccu, 0x80500600u}};
    GekkoAOTModuleDesc rel{};
    rel.code_ranges = code;
    rel.num_code_ranges = rel.num_rel_modules = 1;
    rel.dispatch = &RelFpuTrap;
    runtime.rel_mapping_dirty_ = false;
    runtime.cached_rel_route_ = {};
    runtime.active_rel_sections_ = {
      {&rel, 1, 1, 0x805000ccu, 0x812000ecu, 0x534u},
      {&rel, 1, 5, 0x8059d220u, 0x8129d240u, 0x64a8u},
      {&rel, 1, 6, 0x805a36d8u, 0x812bfd20u, 0x12c4u}};
    RunResult result{};
    CHECK(runtime.DispatchSecondaryModule(0x812000ecu, result));
    CHECK(runtime.Cpu().pc == 0x800u);
    CHECK(runtime.Cpu().srr0 == 0x812000f8u);
    CHECK(runtime.Cpu().gpr[6] == 0x805a0000u);
    CHECK(runtime.Cpu().gpr[30] == 0x805a3c18u);
    rel.dispatch = &RelMainExit;
    runtime.secondary_module_caps_[&rel] = GEKKOAOT_MODULE_CAP_REL_LIVE_GPRS;
    runtime.cached_secondary_caps_owner_ = nullptr;
    CHECK(runtime.DispatchSecondaryModule(0x812000ecu, result));
    CHECK(runtime.Cpu().gpr[6] == 0x805a0000u);
    CHECK(runtime.Cpu().gpr[30] == 0x812c0260u);
    CHECK(runtime.Cpu().lr == 0x812000f8u);
    if (std::getenv("GEKKOAOT_BENCH_REL_BOUNDARY")) {
      for (const auto caps : {0u, GEKKOAOT_MODULE_CAP_REL_LIVE_GPRS}) {
        runtime.secondary_module_caps_[&rel] = caps;
        runtime.cached_secondary_caps_owner_ = nullptr;
        const auto start = std::chrono::steady_clock::now();
        for (unsigned i = 0; i < 500000u; ++i)
          CHECK(runtime.DispatchSecondaryModule(0x812000ecu, result));
        const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - start).count();
        std::cout << "REL boundary caps=" << caps << " ns/dispatch=" << ns/500000.0 << '\n';
      }
    }
    runtime.secondary_module_caps_.erase(&rel);
    runtime.cached_secondary_caps_owner_ = nullptr;
    runtime.active_rel_sections_.clear();
    runtime.rel_mapping_dirty_ = true;
    runtime.Cpu().pc = runtime.Cpu().srr0 = 0;
  }
  static int NestedRelCallback(CPUState* cpu, std::uint32_t address) {
    CHECK(address == 0x805000ccu);
    CHECK(cpu->lr == 0x812000f8u); // Caller address was published before entry.
    cpu->pc = cpu->lr;
    return 1;
  }
  static int RelCallback(CPUState* cpu, std::uint32_t address) {
    CHECK(address == 0x805000ccu);
    cpu->external_addr = 0x805000ccu;
    cpu->external_value = 0x805000ccu;
    cpu->external_rid = 0xfdu;
    CHECK(cpu->host_call(cpu, 0xfffffffau));
    CHECK(cpu->external_rid == 0xfeu && cpu->external_value == 0x812000ecu);
    cpu->lr = 0x805000d8u;
    cpu->external_addr = 0x813000ecu;
    CHECK(cpu->host_call(cpu, 0xfffffff9u));
    CHECK(cpu->pc == 0x812000f8u);
    cpu->pc = 0x80010000u;
    return 1;
  }
  static void VerifyLiveRelCallbacks(HostRuntime& runtime) {
    const GekkoAOTRange code[] = {{0x805000ccu, 0x80500600u}};
    GekkoAOTModuleDesc field{}, battle{};
    field.code_ranges = battle.code_ranges = code;
    field.num_code_ranges = battle.num_code_ranges = 1;
    field.num_rel_modules = battle.num_rel_modules = 1;
    field.dispatch = &RelCallback;
    battle.dispatch = &NestedRelCallback;
    runtime.rel_mapping_dirty_ = false;
    runtime.cached_rel_route_ = {};
    runtime.active_rel_sections_ = {
      {&battle, 2, 1, 0x805000ccu, 0x813000ecu, 0x534u},
      {&field, 1, 1, 0x805000ccu, 0x812000ecu, 0x534u}};
    // Equal canonical windows must retain the executing owner, including the
    // address query made from inside its generated publishing helper.
    CHECK(runtime.TranslateSecondaryAddress(&field, 0x805000d8u) == 0x812000f8u);
    CHECK(runtime.TranslateSecondaryAddress(&battle, 0x805000d8u) == 0x813000f8u);
    runtime.Cpu().pc = 0x812000ecu;
    RunResult result{};
    CHECK(runtime.DispatchSecondaryModule(0x812000ecu, result));
    CHECK(runtime.Cpu().pc == 0x80010000u && runtime.Cpu().lr == 0x812000f8u);
    runtime.Cpu().pc = 0x800u;
    runtime.Cpu().srr0 = 0x805000d8u;
    runtime.PublishSecondaryControlState(&field);
    CHECK(runtime.Cpu().srr0 == 0x812000f8u);
    const GekkoAOTModuleDesc* resolved = nullptr;
    std::uint32_t linked = 0;
    // An ambiguous legacy callback cannot silently choose the first variant.
    CHECK(!runtime.ResolveSecondaryAddress(0x805000ccu, &resolved, &linked));
    runtime.active_rel_sections_.erase(runtime.active_rel_sections_.begin());
    CHECK(runtime.ResolveSecondaryAddress(0x805000ccu, &resolved, &linked));
    CHECK(resolved == &field && linked == 0x805000ccu);
    CHECK(runtime.ResolveSecondaryAddress(0x812000ecu, &resolved, &linked));
    CHECK(resolved == &field && linked == 0x805000ccu);
    CHECK(runtime.ResolveSecondaryAddress(0x812000f0u, &resolved, &linked));
    CHECK(resolved == &field && linked == 0x805000d0u);
    CHECK(!runtime.ResolveSecondaryAddress(0x81200620u, &resolved, &linked));
    runtime.active_rel_sections_.clear();
    runtime.rel_mapping_dirty_ = true;
    CHECK(!runtime.ResolveSecondaryAddress(0x812000ecu, &resolved, &linked));
    CHECK(!runtime.cached_rel_route_.owner);
    runtime.Cpu().pc = runtime.Cpu().lr = runtime.Cpu().srr0 = 0;
  }
  static void VerifyOverlayHookIdentity(HostRuntime& runtime) {
    constexpr std::uint32_t address = 0x80022000u;
    auto& cpu = runtime.Cpu();
    runtime.Memory().Write32(address, 0x7c6c42e6u);
    runtime.Memory().Write32(address + 4u, 0x4e800020u);
    runtime.RegisterSimpleHle(SimpleHleKind::OsGetTick, address);
    std::uint64_t hash = 0xcbf29ce484222325ull;
    for (const auto byte : {0x7cu,0x6cu,0x42u,0xe6u,0x4eu,0x80u,0x00u,0x20u}) {
      hash ^= byte; hash *= 1099511628211ull;
    }
    runtime.native_hook_code_proofs_[address] = {address, address + 8u, hash, 0u, false};
    runtime.has_fixed_secondary_modules_ = true;
    const auto region_blocked = [&]() {
      cpu.external_addr = address - 16u;
      cpu.external_value = address + 32u;
      return runtime.DispatchHostCall(0xfffffffcu);
    };
    CHECK(region_blocked());
    cpu.pc = address; cpu.lr = 0x80023000u; cpu.timebase = 12345;
    CHECK(runtime.DispatchHostCall(address));CHECK(cpu.pc == cpu.lr && cpu.gpr[3] == 12345);
    // The overlay reuses the same address for unrelated instructions. Neither
    // exact HLE dispatch nor chunk admission may retain the bootstrap hook.
    runtime.Memory().Write32(address, 0x80630004u);
    ++runtime.fixed_exec_route_epoch_;
    CHECK(!region_blocked());
    cpu.pc = address;
    CHECK(!runtime.DispatchHostCall(address));CHECK(cpu.pc == address);
    // Reloading the original image re-enables both paths, including caches.
    runtime.Memory().Write32(address, 0x7c6c42e6u);
    ++runtime.fixed_exec_route_epoch_;
    CHECK(region_blocked());CHECK(runtime.DispatchHostCall(address));
    runtime.has_fixed_secondary_modules_ = false;
  }
  static void VerifyPhysicalImageRoute(HostRuntime& runtime) {
    GekkoAOTModuleDesc image{};
    runtime.has_fixed_secondary_modules_ = true;
    runtime.fixed_exec_route_cache_ = {&image, 0x80020000u, 0x80020020u, true};
    bool overlap = false;
    CHECK(runtime.SelectFixedExecutableModule(0x00020000u, &overlap) == &image);
    CHECK(overlap);
    CHECK(runtime.SelectFixedExecutableModule(0x80020010u, &overlap) == &image);
    CHECK(overlap);
    CHECK(runtime.SelectFixedExecutableModule(0xcc020000u, &overlap) == nullptr);
    runtime.has_fixed_secondary_modules_ = false;
    runtime.fixed_exec_route_cache_ = {};
  }
};
}

int main() {
  VerifyRelSyntheticAddressValidation();
  {
    auto probe_runtime = std::make_unique<HostRuntime>();
    GekkoAOT::Native::HardwareTestAccess::VerifyCopiedCacheStub(*probe_runtime);
    GekkoAOT::Native::HardwareTestAccess::VerifyDispatchProbePublication(*probe_runtime);
    GekkoAOT::Native::HardwareTestAccess::VerifyLeafTail(*probe_runtime);
    GekkoAOT::Native::HardwareTestAccess::VerifyEeEnableQuery(*probe_runtime);
  }
  VerifyRelImportIdentity();
  auto runtime = std::make_unique<HostRuntime>();
  auto& cpu = runtime->Cpu();
  GekkoAOT::Native::HardwareTestAccess::VerifyLiveRelCallbacks(*runtime);
  GekkoAOT::Native::HardwareTestAccess::VerifyRelExceptionRegisters(*runtime);
  GekkoAOT::Native::HardwareTestAccess::VerifyPhysicalImageRoute(*runtime);
  GekkoAOT::Native::HardwareTestAccess::VerifyOverlayHookIdentity(*runtime);

  // An indirect jump can enter an instruction inside a translated function,
  // rather than at one of its compiled basic-block labels. MKDD uses a table
  // that targets a standalone blr at the end of a short setter.
  cpu.pc = 0x80010000;
  cpu.ctr = 0x80020013;
  cpu.lr = 0x80010004;
  cpu.instruction_fallback(&cpu, 0x4e800420, cpu.pc); // bctr
  CHECK(cpu.exception == 0 && cpu.pc == 0x80020010);
  CHECK(cpu.ctr == 0x80020013 && cpu.lr == 0x80010004);
  cpu.instruction_fallback(&cpu, 0x4e800020, cpu.pc); // interior blr
  CHECK(cpu.exception == 0 && cpu.pc == 0x80010004);

  cpu.pc = 0x80011000;
  cpu.ctr = 0x80021003;
  cpu.lr = 0x80001000;
  cpu.instruction_fallback(&cpu, 0x4e800421, cpu.pc); // bctrl
  CHECK(cpu.exception == 0 && cpu.pc == 0x80021000);
  CHECK(cpu.lr == 0x80011004 && cpu.ctr == 0x80021003);
  cpu.instruction_fallback(&cpu, 0x4e800020, cpu.pc); // blr
  CHECK(cpu.exception == 0 && cpu.pc == 0x80011004);

  // bclrl tests the specified CR bit, reads the old LR as its target, then
  // writes the link address even when the branch is not taken.
  cpu.pc = 0x80020100;
  cpu.lr = 0x80030003;
  cpu.ctr = 7;
  cpu.cr = 0x20000000; // CR0.EQ = 1 (BI=2)
  cpu.instruction_fallback(&cpu, 0x4d820021, cpu.pc); // bclrl BO=12, BI=2
  CHECK(cpu.exception == 0 && cpu.pc == 0x80030000);
  CHECK(cpu.lr == 0x80020104 && cpu.ctr == 7);

  cpu.pc = 0x80020110;
  cpu.lr = 0x80030003;
  cpu.cr = 0;
  cpu.instruction_fallback(&cpu, 0x4d820021, cpu.pc);
  CHECK(cpu.exception == 0 && cpu.pc == 0x80020114);
  CHECK(cpu.lr == 0x80020114 && cpu.ctr == 7);

  // BO=0 decrements CTR and branches while CTR is nonzero and CR0.LT is 0.
  cpu.pc = 0x80020200;
  cpu.lr = 0x80031003;
  cpu.ctr = 2;
  cpu.instruction_fallback(&cpu, 0x4c000020, cpu.pc); // bclr BO=0, BI=0
  CHECK(cpu.exception == 0 && cpu.pc == 0x80031000 && cpu.ctr == 1);
  cpu.pc = 0x80020210;
  cpu.ctr = 1;
  cpu.instruction_fallback(&cpu, 0x4c000020, cpu.pc);
  CHECK(cpu.exception == 0 && cpu.pc == 0x80020214 && cpu.ctr == 0);

  // BCCTR with a decrementing BO is invalid in the PowerPC decoder.
  cpu.pc = 0x80020300;
  cpu.instruction_fallback(&cpu, 0x4c000420, cpu.pc);
  CHECK((cpu.exception & 0x80000000u) != 0);

  std::cout << "AOT interior indirect branch/return tests passed\n";
}
