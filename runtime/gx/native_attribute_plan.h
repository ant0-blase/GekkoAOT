// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <cstdint>

namespace GekkoAOT::GX {
// Compile VCD once into an ordered native binding plan. Only descriptors,
// never guest pointers, resource contents or GPU ranges, survive a draw.
// VCD/VAT publication invalidates the owning layout in the bridge.
struct NativeAttributePlan {
  static constexpr unsigned First = 9, Last = 20;
  std::uint16_t mask = 0;
  std::array<std::uint8_t, 12> attributes{};
  std::uint8_t count = 0;
  std::uint8_t reserved = 0;

  bool Add(unsigned attribute) noexcept {
    if (attribute < First || attribute > Last || count == attributes.size()) return false;
    const auto bit = static_cast<std::uint16_t>(1u << (attribute - First));
    if (mask & bit) return false;
    mask |= bit;
    attributes[count++] = static_cast<std::uint8_t>(attribute);
    return true;
  }
  template<class F> void ForEach(bool native, F&& visit) const {
    if (native) {
      for (unsigned i = 0; i < count; ++i) visit(attributes[i]);
    } else {
      // Compatibility traversal: callers retain their descriptor admission.
      for (unsigned attribute = First; attribute <= Last; ++attribute) visit(attribute);
    }
  }
};
static_assert(sizeof(NativeAttributePlan) == 16);
} // namespace GekkoAOT::GX
