// Compiled with AVX2 enabled (see CMakeLists.txt); only reached after the
// dispatcher in xxh3_dispatch.cpp has confirmed CPU and OS AVX2 support.
// XXH_INLINE_ALL gives every xxHash function internal linkage here, so this
// AVX2 instantiation cannot collide with the baseline library symbols.
#define XXH_INLINE_ALL
#include <xxhash.h>

#include <cstddef>

XXH128_hash_t gekkoaot_xxh3_128_avx2(const void* data, std::size_t size) noexcept {
  return XXH3_128bits(data, size);
}
