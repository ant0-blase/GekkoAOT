// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "core/module_abi.h"

typedef void (*GekkoAOTChunkFn)(CPUState*);
typedef struct { uint32_t start, end; GekkoAOTChunkFn fn; } GekkoAOTChunkRoute;
typedef struct {
  const GekkoAOTChunkRoute* owner;
  uint32_t address;
  GekkoAOTChunkFn fn;
} GekkoAOTChunkCacheEntry;
typedef struct { GekkoAOTChunkCacheEntry entries[256]; } GekkoAOTChunkCache;

// Only immutable module metadata is cached. Guest code identity, relocation
// ownership, IRQ state and registers stay with HostRuntime's existing guards.
// Full address and table identity distinguish collisions and module switches.
static inline GekkoAOTChunkFn gekkoaot_find_chunk(
    const GekkoAOTChunkRoute* routes, uint32_t count, uint32_t address,
    GekkoAOTChunkCache* cache) {
  GekkoAOTChunkCacheEntry* entry = NULL;
  if (cache) {
    entry = &cache->entries[((address >> 2) ^ (address >> 12)) & 255u];
    if (entry->owner == routes && entry->address == address) return entry->fn;
  }
  uint32_t lo = 0, hi = count;
  while (lo < hi) {
    const uint32_t mid = lo + ((hi - lo) >> 1);
    if (routes[mid].start <= address) lo = mid + 1; else hi = mid;
  }
  GekkoAOTChunkFn fn = NULL;
  if (lo) {
    const GekkoAOTChunkRoute* r = &routes[lo - 1];
    if (address < r->end && ((address - r->start) & 3u) == 0) fn = r->fn;
  }
  if (entry) { entry->owner = routes; entry->address = address; entry->fn = fn; }
  return fn;
}

// v295: an immutable 16-bit image maps each aligned guest instruction to its
// owning native chunk. Dense text avoids hashing, atomics and binary search.
// Zero is a gap; immutable table ownership keeps simultaneous CPUs independent.
typedef struct {
  const GekkoAOTChunkRoute* routes;
  const uint16_t* indices;
  uint32_t base, words, route_count;
} GekkoAOTInstructionRoutes;
static inline GekkoAOTChunkFn gekkoaot_find_instruction_chunk(
    const GekkoAOTInstructionRoutes* image, uint32_t address) {
  const uint32_t offset = address - image->base;
  if ((offset & 3u) || (offset >> 2u) >= image->words) return NULL;
  const uint32_t index = (uint32_t)image->indices[offset >> 2u] - 1u;
  return index < image->route_count ? image->routes[index].fn : NULL;
}

// A cache belongs to one immutable routing table, not to one CPU or thread.
// Store address + (route index + 1) in ONE atomic word so colliding writers
// cannot combine another address with this function. Zero index means a miss.
// No TLS lookup, allocation, owner state or locks occur on the hot path.
#ifdef __cplusplus
#include <atomic>
typedef std::atomic<uint64_t> GekkoAOTAtomicRoute;
#define GEKKOAOT_ROUTE_LOAD(p) ((p)->load(std::memory_order_relaxed))
#define GEKKOAOT_ROUTE_STORE(p, v) ((p)->store((v), std::memory_order_relaxed))
#else
#include <stdatomic.h>
typedef _Atomic(uint64_t) GekkoAOTAtomicRoute;
#define GEKKOAOT_ROUTE_LOAD(p) atomic_load_explicit((p), memory_order_relaxed)
#define GEKKOAOT_ROUTE_STORE(p, v) atomic_store_explicit((p), (v), memory_order_relaxed)
#endif
typedef struct {
  uint32_t first, end;
} GekkoAOTChunkPage;
typedef struct {
  const GekkoAOTChunkRoute* routes;
  GekkoAOTAtomicRoute entries[256];
  const GekkoAOTChunkPage* pages;
  uint32_t first_page, page_count;
} GekkoAOTSharedChunkCache;
static inline GekkoAOTChunkFn gekkoaot_find_shared_chunk(
    GekkoAOTSharedChunkCache* cache, uint32_t count, uint32_t address) {
  const GekkoAOTChunkRoute* routes = cache->routes;
  GekkoAOTAtomicRoute* slot = &cache->entries[((address >> 2) ^ (address >> 12)) & 255u];
  uint64_t entry = GEKKOAOT_ROUTE_LOAD(slot);
  if (entry && (uint32_t)(entry >> 32) == address) {
    const uint32_t index = (uint32_t)entry;
    return index ? routes[index - 1u].fn : NULL;
  }
  uint32_t lo = 0, hi = count;
  if (cache->pages) {
    const uint32_t page = (address >> 12) - cache->first_page;
    if (page >= cache->page_count) {
      GEKKOAOT_ROUTE_STORE(slot, (uint64_t)address << 32);
      return NULL;
    }
    lo = cache->pages[page].first;
    hi = cache->pages[page].end;
    if (lo == hi) {
      GEKKOAOT_ROUTE_STORE(slot, (uint64_t)address << 32);
      return NULL;
    }
  }
  const uint32_t first = lo;
  while (lo < hi) {
    const uint32_t mid = lo + ((hi - lo) >> 1);
    if (routes[mid].start <= address) lo = mid + 1; else hi = mid;
  }
  uint32_t index = 0;
  if (lo > first && address < routes[lo - 1u].end && ((address - routes[lo - 1u].start) & 3u) == 0)
    index = lo;
  GEKKOAOT_ROUTE_STORE(slot, ((uint64_t)address << 32) | index);
  return index ? routes[index - 1u].fn : NULL;
}
#undef GEKKOAOT_ROUTE_LOAD
#undef GEKKOAOT_ROUTE_STORE
