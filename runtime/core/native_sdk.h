// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdint.h>
typedef struct CPUState CPUState;
// Optional compiler-to-service ABI, independent of CPUState and module v3.
// Each slot names a statically specialized service, never a synthetic token.
typedef uint32_t (*GekkoAOTNativeSdkService)(CPUState*);
typedef enum GekkoAOTNativeSdkSlot {
#define SDK_SIMPLE(kind, name, abi) GekkoAOTSdkSimple_##kind,
#define SDK_OS(kind, name, abi) GekkoAOTSdkOs_##kind,
#include "sdk/native_services.def"
#undef SDK_SIMPLE
#undef SDK_OS
  GekkoAOTNativeSdkCount
} GekkoAOTNativeSdkSlot;
typedef struct GekkoAOTNativeSdk {
  uint32_t version, size, count, reserved;
  uint64_t registry_hash;
  GekkoAOTNativeSdkService entries[GekkoAOTNativeSdkCount];
} GekkoAOTNativeSdk;
typedef int (*GekkoAOTBindNativeSdk)(const GekkoAOTNativeSdk*);
// Hash the names, domains and ABI recipes as well as slot order. Equal counts
// alone cannot make a cached module safe after a registry edit.
static inline uint64_t gekkoaot_native_sdk_registry_hash(void) {
  static const char schema[] =
#define SDK_SIMPLE(kind, name, abi) "simple_" #kind ":" name ":" abi ";"
#define SDK_OS(kind, name, abi) "os_" #kind ":" name ":" abi ";"
#include "sdk/native_services.def"
#undef SDK_SIMPLE
#undef SDK_OS
  ;
  uint64_t value=UINT64_C(14695981039346656037);
  for(unsigned i=0;i<sizeof(schema)-1;++i){value^=(unsigned char)schema[i];value*=UINT64_C(1099511628211);}
  return value;
}
