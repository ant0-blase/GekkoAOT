#pragma once
// SPDX-License-Identifier: GPL-3.0-or-later

#include "core/cpu_state.h"
#include "core/module_abi.h"
#include "core/native_hardware.h"

#include <cstdint>
#include <filesystem>
#include <string>

namespace GekkoAOT::Native {

enum class ModuleStatus {
  Ok = 0,
  LibraryOpenFailed,
  EntryPointMissing,
  NullDescriptor,
  ModuleAbiMismatch,
  CpuAbiMismatch,
  CpuStateSizeMismatch,
  InvalidGameId,
  GameIdMismatch,
  MissingDispatch,
  InvalidCodeRanges,
  InvalidSmcRanges,
  InvalidChunks,
  EntryPointUncovered,
  InvalidRelModules,
  MemoryRestartRequired,
};

const char* ModuleStatusName(ModuleStatus status);
ModuleStatus ValidateModule(const GekkoAOTModuleDesc* descriptor, const char* expected_game_id = nullptr);

class ModuleLibrary final {
public:
  ModuleLibrary() = default;
  ~ModuleLibrary();
  ModuleLibrary(const ModuleLibrary&) = delete;
  ModuleLibrary& operator=(const ModuleLibrary&) = delete;

  ModuleStatus Open(const std::filesystem::path& path, const char* expected_game_id = nullptr);
  // Flush compiler-rt PGO counters from an instrumented game module.
  // Returns 0 on success, -1 when the module does not expose the hook.
  int FlushProfile() const;
  bool BindNativeHardware(const GekkoAOTNativeHardware& services) const;
  bool BindNativeWgpipeTry(const GekkoAOTNativeWgpipeTry& services) const;
  bool BindNativeAddressWindow(const GekkoAOTNativeAddressWindow& services) const;
  bool BindNativeFlatVmem(const GekkoAOTNativeFlatVmem& services) const;
  bool BindNativeLockedCache(const GekkoAOTNativeLockedCache& services) const;
  bool BindNativeStableMmio(const GekkoAOTNativeStableMmio& services) const;
  void Close();
  bool IsOpen() const { return descriptor_ != nullptr; }
  const GekkoAOTModuleDesc* Descriptor() const { return descriptor_; }
  bool SupportsMemoryRestart() const { return (capabilities_ & GEKKOAOT_MODULE_CAP_MEMORY_RESTART) != 0u; }
  std::uint32_t Capabilities() const { return capabilities_; }
  const GekkoAOTRelImportProof* RelImportProofs(std::uint32_t id, std::uint32_t* count) const {
    *count = 0;
    return rel_import_proofs_ ? rel_import_proofs_(id, count) : nullptr;
  }
  const std::string& LastError() const { return last_error_; }

private:
  void* handle_ = nullptr;
  const GekkoAOTModuleDesc* descriptor_ = nullptr;
  std::uint32_t capabilities_ = 0;
  GekkoAOTRelImportProofsFn rel_import_proofs_ = nullptr;
  std::string last_error_;
};

} // namespace GekkoAOT::Native
