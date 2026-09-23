#pragma once
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstddef>
#include <filesystem>

namespace GekkoAOT::Native {

class HostRuntime;

struct NativeSdkAutoStats {
  std::size_t scanned_words = 0;
  std::size_t direct_call_targets = 0;
  std::size_t exact_functions = 0;
  std::size_t ambiguous_functions = 0;
  std::size_t resolved_functions = 0;
  std::size_t os_candidates = 0;
  std::size_t os_hooks = 0;
  std::size_t simple_hooks = 0;
  std::size_t missing_scheduler = 0;
};

// Recover Nintendo SDK identities directly from the original PPC DOL bytes and
// register safe standalone HLE entrypoints. This deliberately does not depend
// on Dolphin/ModernGekko SymbolDB state and behaves identically for C and LLVM
// AOT modules.
NativeSdkAutoStats AutoRegisterNativeSdk(HostRuntime& runtime,
                                           const std::filesystem::path& manifest_output = {});

// Register the exact build-time SDK/HLE plan emitted by AutoRegisterNativeSdk.
// This avoids rescanning the DOL on every launch and, more importantly, gives
// the AOT compiler the same immutable interception set used by the runtime.
bool RegisterNativeSdkManifest(HostRuntime& runtime,
                               const std::filesystem::path& manifest_path);

} // namespace GekkoAOT::Native
