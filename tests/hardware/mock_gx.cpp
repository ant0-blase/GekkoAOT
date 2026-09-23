// SPDX-License-Identifier: GPL-3.0-or-later
// Test double for the native renderer ABI, not a GPU implementation.
#include <cstdint>
#include <vector>
#ifdef _WIN32
#define EXPORT extern "C" __declspec(dllexport)
#else
#define EXPORT extern "C" __attribute__((visibility("default")))
#endif
namespace { std::vector<std::uint8_t> bytes; }
using Resolve = bool (*)(void*, std::uint32_t, std::uint32_t, std::uint32_t,
                         std::uint32_t, const void**, std::uint32_t*);
EXPORT bool gekkoaot_native_gx_init(Resolve, void*) { bytes.clear(); return true; }
EXPORT void gekkoaot_native_gx_write(std::uint64_t value, std::uint8_t size, std::uint32_t) {
  for (unsigned i = 0; i < size; ++i) bytes.push_back(value >> ((size - i - 1) * 8));
}
EXPORT void gekkoaot_native_gx_write_burst(const std::uint8_t* data, std::uint32_t size, std::uint32_t) {
  bytes.insert(bytes.end(), data, data + size);
}
EXPORT void gekkoaot_native_gx_present() {}
EXPORT void gekkoaot_native_gx_present_xfb(std::uint32_t, std::uint32_t) {}
EXPORT void gekkoaot_native_gx_shutdown() {}
EXPORT std::uint32_t gekkoaot_test_gx_bytes() { return bytes.size(); }
EXPORT std::uint8_t gekkoaot_test_gx_byte(std::uint32_t i) { return bytes.at(i); }
