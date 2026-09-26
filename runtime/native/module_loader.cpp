// SPDX-License-Identifier: GPL-3.0-or-later
#include "native/module_loader.h"

#include <cstring>
#include <limits>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace GekkoAOT::Native {
namespace {

bool RangesValid(const GekkoAOTRange* ranges, std::uint32_t count) {
  if (!ranges || count == 0) return false;
  for (std::uint32_t i = 0; i < count; ++i) {
    if (ranges[i].start >= ranges[i].end) return false;
    if (i != 0 && ranges[i - 1].end > ranges[i].start) return false;
  }
  return true;
}

bool AddressCovered(const GekkoAOTRange* ranges, std::uint32_t count, std::uint32_t address) {
  for (std::uint32_t i = 0; i < count; ++i)
    if (address >= ranges[i].start && address < ranges[i].end) return true;
  return false;
}

bool ChunksTileCode(const GekkoAOTModuleDesc* d) {
  if (!RangesValid(d->chunk_ranges, d->num_chunk_ranges) || !d->chunk_hashes) return false;
  std::uint32_t chunk = 0;
  for (std::uint32_t code = 0; code < d->num_code_ranges; ++code) {
    std::uint32_t cursor = d->code_ranges[code].start;
    while (chunk < d->num_chunk_ranges && d->chunk_ranges[chunk].start < d->code_ranges[code].end) {
      const auto r = d->chunk_ranges[chunk];
      if (r.start != cursor || r.end > d->code_ranges[code].end) return false;
      cursor = r.end;
      ++chunk;
    }
    if (cursor != d->code_ranges[code].end) return false;
  }
  return chunk == d->num_chunk_ranges;
}

bool RelModulesValid(const GekkoAOTModuleDesc* d) {
  if (d->num_rel_modules == 0) return d->rel_modules == nullptr;
  if (!d->rel_modules) return false;
  for (std::uint32_t i = 0; i < d->num_rel_modules; ++i) {
    const auto& module = d->rel_modules[i];
    if (module.module_id == 0 || module.section_count == 0 ||
        module.section_info_offset < 0x40u || module.file_size < 0x40u ||
        !module.sections || module.num_sections == 0) return false;
    bool has_code_section = false;
    for (std::uint32_t j = 0; j < module.num_sections; ++j) {
      const auto& section = module.sections[j];
      const std::uint64_t end = std::uint64_t(section.linked_start) + section.size;
      if (section.module_id != module.module_id || section.section_index >= module.section_count ||
          section.size == 0 || section.linked_start < 0x80000000u ||
          end > 0x81800000ull) return false;
      if (AddressCovered(d->code_ranges, d->num_code_ranges, section.linked_start))
        has_code_section = true;
    }
    if (!has_code_section) return false;
  }
  return true;
}

#ifdef _WIN32
std::string WindowsError() {
  const DWORD code = GetLastError();
  if (!code) return {};
  LPSTR buffer = nullptr;
  const DWORD size = FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                                        FORMAT_MESSAGE_IGNORE_INSERTS,
                                    nullptr, code, 0, reinterpret_cast<LPSTR>(&buffer), 0, nullptr);
  std::string out = size && buffer ? std::string(buffer, size) : ("Win32 error " + std::to_string(code));
  if (buffer) LocalFree(buffer);
  return out;
}
#endif

} // namespace

const char* ModuleStatusName(ModuleStatus status) {
  switch (status) {
  case ModuleStatus::Ok: return "ok";
  case ModuleStatus::LibraryOpenFailed: return "library open failed";
  case ModuleStatus::EntryPointMissing: return "gekkoaot_get_module missing";
  case ModuleStatus::NullDescriptor: return "null descriptor";
  case ModuleStatus::ModuleAbiMismatch: return "module ABI mismatch";
  case ModuleStatus::CpuAbiMismatch: return "CPU ABI mismatch";
  case ModuleStatus::CpuStateSizeMismatch: return "CPU state size mismatch";
  case ModuleStatus::InvalidGameId: return "invalid game ID";
  case ModuleStatus::GameIdMismatch: return "game ID mismatch";
  case ModuleStatus::MissingDispatch: return "missing dispatch function";
  case ModuleStatus::InvalidCodeRanges: return "invalid code ranges";
  case ModuleStatus::InvalidSmcRanges: return "invalid SMC ranges";
  case ModuleStatus::InvalidChunks: return "invalid chunks";
  case ModuleStatus::EntryPointUncovered: return "entry point is not covered by code ranges";
  case ModuleStatus::InvalidRelModules: return "invalid REL module metadata";
  case ModuleStatus::MemoryRestartRequired: return "active VM pages require memory restart support";
  }
  return "unknown module status";
}

ModuleStatus ValidateModule(const GekkoAOTModuleDesc* d, const char* expected_game_id) {
  if (!d) return ModuleStatus::NullDescriptor;
  if (d->abi_version != GEKKOAOT_MODULE_ABI_VERSION) return ModuleStatus::ModuleAbiMismatch;
  if (d->cpu_abi_version != GEKKOAOT_CPU_ABI_VERSION) return ModuleStatus::CpuAbiMismatch;
  if (d->cpu_state_size != sizeof(CPUState)) return ModuleStatus::CpuStateSizeMismatch;
  if (!std::memchr(d->game_id, '\0', sizeof(d->game_id)) || d->game_id[0] == '\0')
    return ModuleStatus::InvalidGameId;
  if (expected_game_id && std::strcmp(d->game_id, expected_game_id) != 0)
    return ModuleStatus::GameIdMismatch;
  if (!d->dispatch) return ModuleStatus::MissingDispatch;
  if (!RangesValid(d->code_ranges, d->num_code_ranges)) return ModuleStatus::InvalidCodeRanges;
  if (d->num_smc_ranges && !RangesValid(d->smc_ranges, d->num_smc_ranges))
    return ModuleStatus::InvalidSmcRanges;
  if (!ChunksTileCode(d)) return ModuleStatus::InvalidChunks;
  if (!RelModulesValid(d)) return ModuleStatus::InvalidRelModules;
  if (!AddressCovered(d->code_ranges, d->num_code_ranges, d->entry_point))
    return ModuleStatus::EntryPointUncovered;
  return ModuleStatus::Ok;
}

ModuleLibrary::~ModuleLibrary() { Close(); }

ModuleStatus ModuleLibrary::Open(const std::filesystem::path& path, const char* expected_game_id) {
  Close();
  last_error_.clear();
#ifdef _WIN32
  const std::wstring wide = path.wstring();
  HMODULE library = LoadLibraryW(wide.c_str());
  if (!library) {
    last_error_ = WindowsError();
    return ModuleStatus::LibraryOpenFailed;
  }
  handle_ = library;
  auto get_module = reinterpret_cast<GekkoAOTGetModuleFn>(
      GetProcAddress(library, GEKKOAOT_GET_MODULE_SYMBOL));
  if (!get_module) {
    last_error_ = WindowsError();
    Close();
    return ModuleStatus::EntryPointMissing;
  }
#else
  handle_ = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
  if (!handle_) {
    if (const char* error = dlerror()) last_error_ = error;
    return ModuleStatus::LibraryOpenFailed;
  }
  dlerror();
  auto get_module = reinterpret_cast<GekkoAOTGetModuleFn>(
      dlsym(handle_, GEKKOAOT_GET_MODULE_SYMBOL));
  (void)dlerror();
  if (!get_module) {
    last_error_ = "gekkoaot_get_module symbol not found";
    Close();
    return ModuleStatus::EntryPointMissing;
  }
#endif
  descriptor_ = get_module ? get_module() : nullptr;
  const ModuleStatus status = ValidateModule(descriptor_, expected_game_id);
  if (status != ModuleStatus::Ok) {
    last_error_ = ModuleStatusName(status);
    Close();
    return status;
  }
  using CapabilitiesFn = std::uint32_t (*)();
#ifdef _WIN32
  auto capabilities = reinterpret_cast<CapabilitiesFn>(
      GetProcAddress(reinterpret_cast<HMODULE>(handle_), "gekkoaot_module_capabilities"));
#else
  dlerror();
  auto capabilities = reinterpret_cast<CapabilitiesFn>(
      dlsym(handle_, "gekkoaot_module_capabilities"));
  (void)dlerror();
#endif
  capabilities_ = capabilities ? capabilities() : 0u;
  return ModuleStatus::Ok;
}

int ModuleLibrary::FlushProfile() const {
  if (!handle_) return -1;
  using FlushProfileFn = int (*)();
#ifdef _WIN32
  auto flush_profile = reinterpret_cast<FlushProfileFn>(
      GetProcAddress(reinterpret_cast<HMODULE>(handle_), "gekkoaot_flush_profile"));
#else
  dlerror();
  auto flush_profile = reinterpret_cast<FlushProfileFn>(
      dlsym(handle_, "gekkoaot_flush_profile"));
  (void)dlerror();
#endif
  return flush_profile ? flush_profile() : -1;
}

void ModuleLibrary::Close() {
  descriptor_ = nullptr;
  capabilities_ = 0u;
  if (!handle_) return;
#ifdef _WIN32
  FreeLibrary(reinterpret_cast<HMODULE>(handle_));
#else
  dlclose(handle_);
#endif
  handle_ = nullptr;
}

} // namespace GekkoAOT::Native
