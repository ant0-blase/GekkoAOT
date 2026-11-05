// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// SDK instruction bodies for tests of the structural proofs and the native
// services they admit. The repository stores only fingerprints of these
// bodies (runtime/sdk/word_proof.h), so tests read the words from a local
// directory outside the repository, taken from the developer's own
// executables:
//
//   GEKKOAOT_SDK_FIXTURES=<dir>  <dir>/<name>.txt holds the body as
//                                whitespace-separated hex words
//
// Without it, tests that need a body exit with kSdkFixtureSkip, which ctest
// reports as skipped (SKIP_RETURN_CODE).
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

#include "../../runtime/sdk/gx_pixel_service.h"

inline constexpr int kSdkFixtureSkip = 77;

inline std::optional<std::vector<std::uint32_t>> SdkFixtureWords(const char* name) {
  const char* dir = std::getenv("GEKKOAOT_SDK_FIXTURES");
  if (!dir || !*dir)
    return std::nullopt;
  std::ifstream in(std::string(dir) + "/" + name + ".txt");
  if (!in)
    return std::nullopt;
  std::vector<std::uint32_t> words;
  std::string token;
  while (in >> token)
    words.push_back(static_cast<std::uint32_t>(std::stoul(token, nullptr, 16)));
  return words;
}

// Loads a body or ends the test as skipped.
inline std::vector<std::uint32_t> RequireSdkFixture(const char* name) {
  auto words = SdkFixtureWords(name);
  if (!words) {
    std::printf("SDK fixture %s not available (GEKKOAOT_SDK_FIXTURES); skipped\n", name);
    std::exit(kSdkFixtureSkip);
  }
  return *words;
}

// Loads a fixed-size body or ends the test as skipped.
template <std::size_t N>
inline std::array<std::uint32_t, N> SdkFixtureArray(const char* name) {
  const auto words = RequireSdkFixture(name);
  std::array<std::uint32_t, N> out{};
  if (words.size() != N) {
    std::fprintf(stderr, "SDK fixture %s: expected %zu words, got %zu\n", name, N, words.size());
    std::exit(1);
  }
  for (std::size_t i = 0; i < N; ++i) out[i] = words[i];
  return out;
}

inline const std::vector<std::uint32_t>& GxPixelFixture(GekkoAOTSdk::GxPixelFamily family) {
  static const std::vector<std::uint32_t> bodies[] = {
      RequireSdkFixture("gx_pixel_0"), RequireSdkFixture("gx_pixel_1"),
      RequireSdkFixture("gx_pixel_2"), RequireSdkFixture("gx_pixel_3")};
  return bodies[static_cast<unsigned>(family)];
}

inline const std::vector<std::uint32_t>& GxSetArrayFixture() {
  static const auto words = RequireSdkFixture("gx_set_array");
  return words;
}

inline const std::vector<std::uint32_t>& GxSetArrayLeafFixture() {
  static const auto words = RequireSdkFixture("gx_set_array_leaf");
  return words;
}
