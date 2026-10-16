#pragma once

#include <cstddef>

#include <xxhash.h>

// v367: XXH3-128 with a runtime AVX2 path for texture/TLUT content hashing.
// The distribution libxxhash (and the fetched Windows build) is compiled for
// the x86-64 baseline (SSE2); AVX2 doubles the throughput on texture-sized
// inputs. GEKKOAOT_XXH3_AVX2=0 forces the baseline library path.
XXH128_hash_t gekkoaot_xxh3_128(const void* data, std::size_t size) noexcept;
