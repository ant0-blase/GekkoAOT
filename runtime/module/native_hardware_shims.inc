// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/native_hardware.h"
#include <stdatomic.h>

static _Atomic(GekkoAOTNativeRead) g_native_cp_read16;
static _Atomic(GekkoAOTNativeRead) g_native_vi_read16;
static _Atomic(GekkoAOTNativeWrite) g_native_pi_write32;
static _Atomic(GekkoAOTNativeWrite) g_native_wgpipe_write;

GEKKOAOT_EXPORT int gekkoaot_bind_native_hardware(const GekkoAOTNativeHardware* services) {
  if (!services || services->version != 1u || services->size != sizeof(*services) ||
      !services->cp_read16 || !services->vi_read16 || !services->pi_write32 ||
      !services->wgpipe_write) return 0;
  // Bind before dispatch. Atomic publication also permits another runtime to
  // load the same DSO: each callback finds its owner through the supplied CPU.
  atomic_store_explicit(&g_native_cp_read16, services->cp_read16, memory_order_release);
  atomic_store_explicit(&g_native_vi_read16, services->vi_read16, memory_order_release);
  atomic_store_explicit(&g_native_pi_write32, services->pi_write32, memory_order_release);
  atomic_store_explicit(&g_native_wgpipe_write, services->wgpipe_write, memory_order_release);
  return 1;
}

#define NATIVE_READ_SHIM(name, binding) \
  GEKKOAOT_EXPORT uint64_t name(CPUState* cpu, uint32_t address, uint8_t size) { \
    GekkoAOTNativeRead fn = atomic_load_explicit(&binding, memory_order_acquire); \
    if (fn) return fn(cpu, address, size); \
    return cpu && cpu->external_read ? cpu->external_read(cpu, address, size) : 0u; \
  }
#define NATIVE_WRITE_SHIM(name, binding) \
  GEKKOAOT_EXPORT void name(CPUState* cpu, uint32_t address, uint64_t value, uint8_t size) { \
    GekkoAOTNativeWrite fn = atomic_load_explicit(&binding, memory_order_acquire); \
    if (fn) fn(cpu, address, value, size); \
    else if (cpu && cpu->external_write) cpu->external_write(cpu, address, value, size); \
  }
NATIVE_READ_SHIM(gekkoaot_cp_read16, g_native_cp_read16)
NATIVE_READ_SHIM(gekkoaot_vi_read16, g_native_vi_read16)
NATIVE_WRITE_SHIM(gekkoaot_pi_write32, g_native_pi_write32)
NATIVE_WRITE_SHIM(gekkoaot_wgpipe_write, g_native_wgpipe_write)
#undef NATIVE_READ_SHIM
#undef NATIVE_WRITE_SHIM

static _Atomic(GekkoAOTTryWgpipeWrite) g_native_wgpipe_try;
GEKKOAOT_EXPORT int gekkoaot_bind_native_wgpipe_try(const GekkoAOTNativeWgpipeTry* services) {
  if (!services || services->version != 1u || services->size != sizeof(*services) ||
      !services->try_write) return 0;
  atomic_store_explicit(&g_native_wgpipe_try, services->try_write, memory_order_release);
  return 1;
}
GEKKOAOT_EXPORT uint32_t gekkoaot_try_wgpipe_write(CPUState* cpu, uint32_t address,
    uint64_t value, uint8_t size, uint32_t pc) {
  GekkoAOTTryWgpipeWrite fn = atomic_load_explicit(&g_native_wgpipe_try, memory_order_acquire);
  // A missing binding cannot call the generic callback: the caller has not
  // published guest registers yet. Its full fallback performs publication.
  return fn ? fn(cpu, address, value, size, pc) : 0u;
}

static _Atomic(GekkoAOTTryStableMmioRead) g_native_stable_mmio;
GEKKOAOT_EXPORT int gekkoaot_bind_native_stable_mmio(const GekkoAOTNativeStableMmio* services) {
  if (!services || services->version != 1u || services->size != sizeof(*services) ||
      !services->try_read) return 0;
  atomic_store_explicit(&g_native_stable_mmio, services->try_read, memory_order_release);
  return 1;
}
GEKKOAOT_EXPORT uint64_t gekkoaot_try_stable_mmio_read(CPUState* cpu, uint32_t address, uint8_t size) {
  GekkoAOTTryStableMmioRead fn = atomic_load_explicit(&g_native_stable_mmio, memory_order_acquire);
  return fn ? fn(cpu, address, size) : 0u;
}
GEKKOAOT_EXPORT uint64_t gekkoaot_stable_mmio_read(CPUState* cpu, uint32_t address, uint8_t size) {
  return cpu && cpu->external_read ? cpu->external_read(cpu, address, size) : 0u;
}

// v327: every generated function asks for the address base, locked-cache base
// and flat-VM view in its prologue (~5% of the HPCOS AOT thread was these
// cross-library calls). The runtime fixes all three when it binds a module, so
// remember the last owner's answer; a new binding forgets it. Guest code runs
// on one CPU thread per owner.
typedef struct GekkoAOTShimCache { CPUState* cpu; const void* value; } GekkoAOTShimCache;
static GekkoAOTShimCache g_address_base_cache, g_locked_cache_cache, g_flat_vmem_cache;

static _Atomic(GekkoAOTNativeAddressBase) g_native_address_base;
GEKKOAOT_EXPORT int gekkoaot_bind_native_address_window(const GekkoAOTNativeAddressWindow* services) {
  if (!services || services->version != 1u || services->size != sizeof(*services) ||
      !services->get_base) return 0;
  g_address_base_cache.cpu = NULL;
  atomic_store_explicit(&g_native_address_base, services->get_base, memory_order_release);
  return 1;
}
GEKKOAOT_EXPORT uint8_t* gekkoaot_native_address_base(CPUState* cpu) {
  if (cpu && g_address_base_cache.cpu == cpu) return (uint8_t*)g_address_base_cache.value;
  GekkoAOTNativeAddressBase fn = atomic_load_explicit(&g_native_address_base, memory_order_acquire);
  if (fn && cpu) {
    uint8_t* base = fn(cpu);
    g_address_base_cache.value = base;
    g_address_base_cache.cpu = cpu;
    return base;
  }
  return fn ? fn(cpu) : NULL;
}

static _Atomic(GekkoAOTGetFlatVmemView) g_native_flat_vmem;
GEKKOAOT_EXPORT int gekkoaot_bind_native_flat_vmem(const GekkoAOTNativeFlatVmem* services) {
  if (!services || services->version != 2u || services->size != sizeof(*services) ||
      !services->get_view) return 0;
  g_flat_vmem_cache.cpu = NULL;
  atomic_store_explicit(&g_native_flat_vmem, services->get_view, memory_order_release);
  return 1;
}
GEKKOAOT_EXPORT const GekkoAOTFlatVmemView* gekkoaot_native_flat_vmem_view(CPUState* cpu) {
  // Generated code reads the paged mirror without a NULL test: the disabled
  // view owns an always-empty table instead.
  static const uint32_t disabled_pages[0x2000u];
  static uint8_t disabled_mru[64];
  static const GekkoAOTFlatVmemView disabled = {NULL, 0u, 0xffffffffu, disabled_pages, disabled_mru};
  if (cpu && g_flat_vmem_cache.cpu == cpu) return (const GekkoAOTFlatVmemView*)g_flat_vmem_cache.value;
  GekkoAOTGetFlatVmemView fn = atomic_load_explicit(&g_native_flat_vmem, memory_order_acquire);
  const GekkoAOTFlatVmemView* view = fn ? fn(cpu) : NULL;
  if (!view) view = &disabled;
  if (fn && cpu) {
    g_flat_vmem_cache.value = view;
    g_flat_vmem_cache.cpu = cpu;
  }
  return view;
}

static _Atomic(GekkoAOTNativeAddressBase) g_native_locked_cache;
GEKKOAOT_EXPORT int gekkoaot_bind_native_locked_cache(const GekkoAOTNativeLockedCache* services) {
  if (!services || services->version!=1u || services->size!=sizeof(*services) || !services->get_base) return 0;
  g_locked_cache_cache.cpu=NULL;
  atomic_store_explicit(&g_native_locked_cache,services->get_base,memory_order_release);
  return 1;
}
GEKKOAOT_EXPORT uint8_t* gekkoaot_native_locked_cache_base(CPUState* cpu) {
  if(cpu && g_locked_cache_cache.cpu==cpu) return (uint8_t*)g_locked_cache_cache.value;
  GekkoAOTNativeAddressBase fn=atomic_load_explicit(&g_native_locked_cache,memory_order_acquire);
  if(fn && cpu) {
    uint8_t* base=fn(cpu);
    g_locked_cache_cache.value=base;
    g_locked_cache_cache.cpu=cpu;
    return base;
  }
  return fn ? fn(cpu) : NULL;
}
