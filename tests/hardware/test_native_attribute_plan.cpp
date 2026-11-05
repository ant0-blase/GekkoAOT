// SPDX-License-Identifier: GPL-3.0-or-later
#include "gx/native_attribute_plan.h"
#include <array>
#include <cstdlib>
#include <iostream>
#include <vector>
#define CHECK(x) do { if (!(x)) { std::cerr << __LINE__ << ": " #x "\n"; std::exit(1); } } while (false)
using GekkoAOT::GX::NativeAttributePlan;
int main() {
  // All legal indexed-attribute combinations, both traversals and the exact
  // ordering used by GPU storage staging. Rebuild after each VCD mutation;
  // descriptors and resource pointers are distinct inputs.
  for (unsigned bits = 0; bits < 4096; ++bits) {
    NativeAttributePlan plan;
    std::array<unsigned, 21> descriptor{};
    std::array<unsigned, 21> resource{};
    for (unsigned i = 9; i <= 20; ++i) {
      descriptor[i] = (bits >> (i - 9)) & 1u;
      resource[i] = i * 13 + bits;
      if (descriptor[i]) CHECK(plan.Add(i));
    }
    const auto before = plan;
    CHECK(!plan.Add(8) && !plan.Add(21) && !plan.Add(0xffffffffu));
    CHECK(plan.mask == before.mask && plan.count == before.count && plan.attributes == before.attributes);
    for (unsigned mutation = 0; mutation < 3; ++mutation) {
      std::vector<unsigned> expected, native, fallback;
      for (unsigned i = 9; i <= 20; ++i) {
        resource[i] ^= mutation * 131u; // fresh bindings, never cached in plan
        if (descriptor[i]) expected.push_back(resource[i]);
      }
      plan.ForEach(true, [&](unsigned i) { CHECK(descriptor[i]); native.push_back(resource[i]); });
      plan.ForEach(false, [&](unsigned i) { if (descriptor[i]) fallback.push_back(resource[i]); });
      CHECK(native == expected && fallback == expected);
    }
    for (unsigned i = 9; i <= 20; ++i) if (descriptor[i]) CHECK(!plan.Add(i));
  }
  std::cout << "4096 native VCD binding plans: exact staging order, live resource mutations, fallback traversal and admission bounds passed\n";
}
