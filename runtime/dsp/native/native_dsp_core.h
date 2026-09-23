#pragma once
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstdint>
#include <memory>

namespace GekkoAOT::DSP {
class NativeDSP;

class NativeDSPCore final {
public:
  explicit NativeDSPCore(NativeDSP* owner);
  ~NativeDSPCore();
  NativeDSPCore(const NativeDSPCore&) = delete;
  NativeDSPCore& operator=(const NativeDSPCore&) = delete;

  void Reset();
  bool Available() const;
  bool RomLoaded() const;
  bool LoadTask(std::uint32_t main_address, std::uint16_t iram_address,
                std::uint16_t length_bytes, std::uint16_t dmem_length,
                std::uint16_t start_pc);
  void SetRunning(bool running);
  void NotifyExternalInterrupt();
  void AdvancePpcCycles(std::uint64_t ppc_cycles);
  std::uint16_t Pc() const;
  std::uint16_t LastOpcode() const;
  std::uint64_t ExecutedInstructions() const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
} // namespace GekkoAOT::DSP
