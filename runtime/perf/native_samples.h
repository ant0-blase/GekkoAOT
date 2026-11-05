// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <memory>

namespace GekkoAOT::Perf {
// Optional host profiler, independent of frame tracing and guest clocks.
// The runtime remains portable; this backend requires desktop Linux x86-64/ARM64.
class NativeSamples {
public:
  NativeSamples();
  ~NativeSamples();
  NativeSamples(const NativeSamples&)=delete;
  void Finish();
  bool Running() const;
private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}
