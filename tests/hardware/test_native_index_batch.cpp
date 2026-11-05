#include "../../runtime/gx/native_index_batch.h"
#include <cstdlib>
#include <iostream>
#include <random>
#include <vector>
static void check(bool yes) { if (!yes) std::abort(); }
int main() {
  using GekkoAOT::GX::RebaseNativeIndexSpan;
  std::mt19937 random(302);
  for (unsigned draw = 0; draw < 100000; ++draw) {
    const std::uint32_t count = 1 + random() % 65535, base = random() % 65536;
    std::vector<std::uint16_t> values(1 + random() % 128);
    for (auto& value : values) value = random() % count;
    if (draw % 7 == 0) values.back() = static_cast<std::uint16_t>(count);
    const auto original = values;
    const bool expected = base <= 65535 - count && draw % 7 != 0;
    const bool result = RebaseNativeIndexSpan(reinterpret_cast<std::uint8_t*>(values.data()),
                                             values.size() * 2, base, count);
    check(result == expected);
    for (std::size_t i = 0; i < values.size(); ++i)
      check(values[i] == (expected ? original[i] + base : original[i]));
  }
  std::uint8_t odd[5]{};
  check(!RebaseNativeIndexSpan(odd, 5, 0, 4));
  check(!RebaseNativeIndexSpan(nullptr, 2, 0, 4));
  std::cout << "Native index batches: 100000 ordered-draw rebases, malformed/overflow rejection passed\n";
}
