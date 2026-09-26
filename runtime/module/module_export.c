// SPDX-License-Identifier: GPL-3.0-or-later
#include "generated.h"
#include "core/module_abi.h"

#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
#include <cpuid.h>
#endif

#if defined(DOLRECOMP_MODULE_HAVE_X86_64_V3)
static int host_has_x86_64_v3(void) {
#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
  unsigned eax,ebx,ecx,edx;
  if (!__get_cpuid(1,&eax,&ebx,&ecx,&edx)) return 0;
  const unsigned req=(1u<<12)|(1u<<22)|(1u<<27)|(1u<<28)|(1u<<29);
  if ((ecx&req)!=req) return 0;
  unsigned xlo,xhi; __asm__ volatile("xgetbv":"=a"(xlo),"=d"(xhi):"c"(0));
  (void)xhi; if ((xlo&6u)!=6u) return 0;
  if (!__get_cpuid_count(7,0,&eax,&ebx,&ecx,&edx)) return 0;
  if ((ebx&((1u<<3)|(1u<<5)|(1u<<8)))!=((1u<<3)|(1u<<5)|(1u<<8))) return 0;
  return __get_cpuid(0x80000001u,&eax,&ebx,&ecx,&edx) && (ecx&(1u<<5));
#else
  return 0;
#endif
}
#endif

static int selected_dispatch(CPUState* ctx, u32 address) {
#if defined(DOLRECOMP_MODULE_HAVE_X86_64_V3)
  // CPUID/XGETBV used to run at every guest dispatch. That turns an otherwise
  // tiny indexed AOT dispatch into a serializing host-CPU query on every edge.
  // The host ISA cannot change while the process is running, so resolve it once.
  static int cached_x86_64_v3 = -1;
  if (cached_x86_64_v3 < 0)
    cached_x86_64_v3 = host_has_x86_64_v3();
  if (cached_x86_64_v3 != 0)
    return dolrecomp_call__x86_64_v3(ctx,address);
#endif
  return dolrecomp_call(ctx,address);
}

#define GEKKOAOT_GLOBAL_INDIRECT_QUERY 0xFFFFFFF9u
#define GEKKOAOT_GLOBAL_INDIRECT_PENDING 0xFCu

void dolrecomp_indirect_dispatch(CPUState* ctx, u32 address) {
  if (selected_dispatch(ctx,address)) return;
  if (!ctx || !ctx->host_call) return;

  // v87: local AOT dispatchers only know their own image. Forward unresolved
  // bctr/bctrl targets to the host-owned global router so callbacks, vtables
  // and returns may cross main.dol, fixed overlays and live side modules.
  const u32 saved_addr=ctx->external_addr;
  const u32 saved_value=ctx->external_value;
  const u8 saved_rid=ctx->external_rid;
  ctx->external_addr=address;
  ctx->external_value=address;
  ctx->external_rid=GEKKOAOT_GLOBAL_INDIRECT_PENDING;
  (void)ctx->host_call(ctx,GEKKOAOT_GLOBAL_INDIRECT_QUERY);
  ctx->external_addr=saved_addr;
  ctx->external_value=saved_value;
  ctx->external_rid=saved_rid;
}

// Native event-horizon superchain.  DolRecomp's wrappers deliberately return
// at budget exits and at ordinary top-level function returns.  Returning all
// the way through the shared-library ABI and HostRuntime for every such edge
// was measurable overhead.  Keep dispatching entirely inside the game module
// until the runtime-provided cycle horizon is reached.
//
// Correctness barriers:
//  * ExternalRead/ExternalWrite and real HLE calls bump the tiny host epochs in
//    CPUState.  We yield immediately after that wrapper, preserving the old
//    hardware/HLE timing boundary.
//  * synchronous exceptions, PC=0 and failed native lookup also yield.
//  * a hard hop cap prevents malformed zero-cycle control flow from spinning.
static uint64_t module_charged_cycles(const CPUState* ctx) {
  if (!ctx || ctx->downcount >= 0) return 0;
  return (uint64_t)(-(ctx->downcount + 1)) + 1u;
}

static int module_dispatch(CPUState* ctx, uint32_t address) {
  if (!ctx) return 0;
  const uint64_t horizon = ctx->cycle_budget > 0 ? (uint64_t)ctx->cycle_budget : 0u;
  if (horizon == 0u) return selected_dispatch(ctx,address);

  enum { kMaxNativeChain = 1024 };
  uint32_t pc = address;
  unsigned hops = 0;
  int progressed = 0;

  while (hops++ < kMaxNativeChain) {
    const uint8_t read_epoch = ctx->external_read_count;
    const uint8_t write_epoch = ctx->external_write_count;
    const int64_t before_downcount = ctx->downcount;
    const uint32_t before_pc = ctx->pc;

    if (!selected_dispatch(ctx,pc))
      return progressed ? 1 : 0;
    progressed = 1;

    if (ctx->exception != 0u || ctx->pc == 0u) break;
    if (ctx->external_read_count != read_epoch ||
        ctx->external_write_count != write_epoch) break;
    if (module_charged_cycles(ctx) >= horizon) break;

    // Defensive backstop for a malformed native wrapper that reports success
    // without consuming cycles or changing control flow.
    if (ctx->pc == before_pc && ctx->downcount == before_downcount) break;
    pc = ctx->pc;
  }
  return progressed;
}

static void on_state_loaded(CPUState* ctx) { ppc_fpscr_control_updated(ctx); }

#include "module_tables.inc"

static const GekkoAOTModuleDesc s_desc = {
  GEKKOAOT_MODULE_ABI_VERSION,
  GEKKOAOT_CPU_ABI_VERSION,
  (uint32_t)sizeof(CPUState),
  MODULE_GAME_ID,
  DOLRECOMP_ENTRY_POINT,
  module_dispatch,
  on_state_loaded,
  s_code_ranges, GEKKOAOT_CODE_RANGE_COUNT,
  s_smc_ranges, GEKKOAOT_SMC_RANGE_COUNT,
  s_chunk_ranges, GEKKOAOT_CHUNK_RANGE_COUNT,
  s_chunk_hashes,
  0, 0
};

#if defined(_WIN32)
#define GEKKOAOT_EXPORT __declspec(dllexport)
#elif defined(__GNUC__) || defined(__clang__)
#define GEKKOAOT_EXPORT __attribute__((visibility("default")))
#else
#define GEKKOAOT_EXPORT
#endif

// Optional capability query keeps the v3 module descriptor layout stable.
// Cached modules built before memory-restart support lack this symbol and
// remain on the legacy SDK VM compatibility aperture.
GEKKOAOT_EXPORT uint32_t gekkoaot_module_capabilities(void) {
#if defined(GEKKOAOT_AOT_RESTARTABLE_MEMORY)
  return GEKKOAOT_MODULE_CAP_MEMORY_RESTART;
#else
  return 0u;
#endif
}

#if defined(GEKKOAOT_PGO_GENERATE)
extern int __llvm_profile_write_file(void);
#endif

GEKKOAOT_EXPORT int gekkoaot_flush_profile(void) {
#if defined(GEKKOAOT_PGO_GENERATE)
  return __llvm_profile_write_file();
#else
  return 0;
#endif
}

GEKKOAOT_EXPORT const GekkoAOTModuleDesc* gekkoaot_get_module(void) { return &s_desc; }
