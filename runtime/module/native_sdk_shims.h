// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/native_sdk.h"
#include <stdatomic.h>
static _Atomic(GekkoAOTNativeSdkService) g_native_sdk_entries[GekkoAOTNativeSdkCount];
GEKKOAOT_EXPORT int gekkoaot_bind_native_sdk(const GekkoAOTNativeSdk* services) {
  if(!services || services->version!=2u || services->size!=sizeof(*services) ||
     services->count!=GekkoAOTNativeSdkCount || services->reserved ||
     services->registry_hash!=gekkoaot_native_sdk_registry_hash())return 0;
  for(unsigned i=0;i<GekkoAOTNativeSdkCount;++i)if(!services->entries[i])return 0;
  for(unsigned i=0;i<GekkoAOTNativeSdkCount;++i)
    atomic_store_explicit(&g_native_sdk_entries[i],services->entries[i],memory_order_release);
  return 1;
}
#define SDK_BINDING(domain, kind, slot) \
  GEKKOAOT_EXPORT uint32_t gekkoaot_sdk_##domain##_##kind(CPUState* cpu) { \
    GekkoAOTNativeSdkService fn=atomic_load_explicit(&g_native_sdk_entries[slot],memory_order_acquire); \
    return fn ? fn(cpu) : 0u; \
  }
#define SDK_SIMPLE(kind, name, abi) SDK_BINDING(simple,kind,GekkoAOTSdkSimple_##kind)
#define SDK_OS(kind, name, abi) SDK_BINDING(os,kind,GekkoAOTSdkOs_##kind)
#include "sdk/native_services.def"
#undef SDK_SIMPLE
#undef SDK_OS
#undef SDK_BINDING
