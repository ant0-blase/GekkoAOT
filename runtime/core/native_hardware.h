#pragma once
// SPDX-License-Identifier: GPL-3.0-or-later
// Optional module binding; neither CPUState nor the v3 descriptor changes.
#include <stdint.h>
typedef struct CPUState CPUState;
typedef uint64_t (*GekkoAOTNativeRead)(CPUState*, uint32_t, uint8_t);
typedef void (*GekkoAOTNativeWrite)(CPUState*, uint32_t, uint64_t, uint8_t);
typedef struct GekkoAOTNativeHardware {
  uint32_t version;
  uint32_t size;
  GekkoAOTNativeRead cp_read16;
  GekkoAOTNativeRead vi_read16;
  GekkoAOTNativeWrite pi_write32;
  GekkoAOTNativeWrite wgpipe_write;
} GekkoAOTNativeHardware;
typedef int (*GekkoAOTBindNativeHardware)(const GekkoAOTNativeHardware*);

// Optional v244 service. On success only a private partial gather is changed;
// CPUState, clocks, epochs, published GPU commands and IRQs are untouched.
// On failure there are no effects and the caller must use the full service.
typedef uint32_t (*GekkoAOTTryWgpipeWrite)(CPUState*, uint32_t, uint64_t, uint8_t, uint32_t);
typedef struct GekkoAOTNativeWgpipeTry {
  uint32_t version;
  uint32_t size;
  GekkoAOTTryWgpipeWrite try_write;
} GekkoAOTNativeWgpipeTry;
typedef int (*GekkoAOTBindNativeWgpipeTry)(const GekkoAOTNativeWgpipeTry*);

// v280: scalar read without publishing guest GPR/FPR/control state. Success
// has bit 63 set and a <=32-bit value. The service may update only the read
// epoch; failure returns zero without effects. No owner pointer is global.
typedef uint64_t (*GekkoAOTTryStableMmioRead)(CPUState*, uint32_t, uint8_t);
typedef struct GekkoAOTNativeStableMmio {
  uint32_t version;
  uint32_t size;
  GekkoAOTTryStableMmioRead try_read;
} GekkoAOTNativeStableMmio;
typedef int (*GekkoAOTBindNativeStableMmio)(const GekkoAOTNativeStableMmio*);

// v287: immutable host reservation base, resolved for the supplied CPU owner.
// NULL preserves biased MEM1/MEM2 pointer lowering. No CPU state, scheduling,
// clocks, resource epochs or hardware state are read/written by this service.
typedef uint8_t* (*GekkoAOTNativeAddressBase)(CPUState*);
typedef struct GekkoAOTNativeAddressWindow {
  uint32_t version;
  uint32_t size;
  GekkoAOTNativeAddressBase get_base;
} GekkoAOTNativeAddressWindow;
typedef int (*GekkoAOTBindNativeAddressWindow)(const GekkoAOTNativeAddressWindow*);

// v304: an owner-specific, revocable view of the flat compatibility VM aperture.
// data is immutable for the owner's lifetime; size becomes zero BEFORE real
// paging is enabled. Generated code must reload size on every admission, so a
// guest SDR1 write in the same AOT function cannot retain the flat mapping.
//
// v322 (services version 2): the same view also mirrors the modeled Gekko DTLB
// for the PAGED aperture. paged_pages[(ea - 0x7e000000) >> 12] is 0 or a MEM1
// frame | 1 (read) | 2 (write; PTE C already set) | way << 2, published only
// while the DTLB holds that translation. Generated code may use it when
// SR7 == paged_sr and MSR[DR,PR] == {1,0}, and must store the way into
// paged_mru[EA[14:19]] (the 2-way LRU state) on every such access.
typedef struct GekkoAOTFlatVmemView {
  uint8_t* data;
  uint32_t size;
  uint32_t paged_sr;
  const uint32_t* paged_pages;
  uint8_t* paged_mru;
} GekkoAOTFlatVmemView;
typedef const GekkoAOTFlatVmemView* (*GekkoAOTGetFlatVmemView)(CPUState*);
typedef struct GekkoAOTNativeFlatVmem {
  uint32_t version;
  uint32_t size;
  GekkoAOTGetFlatVmemView get_view;
} GekkoAOTNativeFlatVmem;
typedef int (*GekkoAOTBindNativeFlatVmem)(const GekkoAOTNativeFlatVmem*);

// v308: immutable owner-specific locked-cache RAM. Unlike paged VMEM this
// aperture has a fixed extent and is also the authoritative DMA backing.
// A missing binding or disabled policy returns NULL for original fallback.
typedef struct GekkoAOTNativeLockedCache {
  uint32_t version;
  uint32_t size;
  GekkoAOTNativeAddressBase get_base;
} GekkoAOTNativeLockedCache;
typedef int (*GekkoAOTBindNativeLockedCache)(const GekkoAOTNativeLockedCache*);
