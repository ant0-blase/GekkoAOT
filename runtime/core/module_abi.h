#pragma once
// SPDX-License-Identifier: GPL-3.0-or-later
// Stable per-game module ABI owned by GekkoAOT.
// The ABI is intentionally independent from any emulator runtime.

#include <stdint.h>

// The module ABI only transports CPUState pointers across the shared-library
// boundary.  Keep the type opaque here so a game module can use DolRecomp's
// canonical CPUState definition from generated.h/cpu.h without redefining it.
// Host code that needs the concrete layout must include core/cpu_state.h.
#ifndef GEKKOAOT_CPU_ABI_VERSION
#define GEKKOAOT_CPU_ABI_VERSION 4u
#endif

typedef struct CPUState CPUState;

#ifdef __cplusplus
extern "C" {
#endif

#define GEKKOAOT_MODULE_ABI_VERSION 3u
#define GEKKOAOT_MODULE_CAP_MEMORY_RESTART 1u
#define GEKKOAOT_GET_MODULE_SYMBOL "gekkoaot_get_module"

typedef struct GekkoAOTRange
{
    uint32_t start;
    uint32_t end;
} GekkoAOTRange;

typedef struct GekkoAOTRelSection
{
    uint32_t module_id;
    uint32_t section_index;
    uint32_t linked_start;
    uint32_t size;
} GekkoAOTRelSection;

typedef struct GekkoAOTRelModule
{
    uint32_t module_id;
    uint32_t version;
    uint32_t section_count;
    uint32_t section_info_offset;
    uint32_t file_size;
    const GekkoAOTRelSection* sections;
    uint32_t num_sections;
} GekkoAOTRelModule;

typedef struct GekkoAOTModuleDesc
{
    uint32_t abi_version;
    uint32_t cpu_abi_version;
    uint32_t cpu_state_size;
    char game_id[8];
    uint32_t entry_point;

    int (*dispatch)(CPUState* state, uint32_t address);
    void (*on_state_loaded)(CPUState* state);

    const GekkoAOTRange* code_ranges;
    uint32_t num_code_ranges;
    const GekkoAOTRange* smc_ranges;
    uint32_t num_smc_ranges;
    const GekkoAOTRange* chunk_ranges;
    uint32_t num_chunk_ranges;
    const uint64_t* chunk_hashes;
    const GekkoAOTRelModule* rel_modules;
    uint32_t num_rel_modules;
} GekkoAOTModuleDesc;

typedef const GekkoAOTModuleDesc* (*GekkoAOTGetModuleFn)(void);

#ifdef __cplusplus
}
#endif
