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
#if defined(__linux__)
#include <elf.h>
#include <link.h>
#include <sys/mman.h>
#include <unistd.h>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <vector>
#endif

namespace GekkoAOT::Native {
namespace {

#if defined(__linux__)
// v326: the AOT main thread is frontend bound and ~19% of its cycles are iTLB
// stalls (HPCOS main thread, TopdownL3): a game module carries 100-250 MiB of
// text and its hot code touches thousands of 4 KiB pages. Move the 2 MiB-
// aligned interior of a large module's executable segment onto anonymous
// transparent huge pages. The copy is fully prepared (MADV_HUGEPAGE, bytes,
// read+execute) and then swapped over the original with one mremap, so a
// failure at any step leaves the loaded file mapping untouched. Addresses do
// not change; only the backing pages do. GEKKOAOT_HUGE_TEXT=0 disables it.
struct TextSegment {
  ElfW(Addr) base = 0;
  std::uintptr_t start = 0, end = 0;
};

int FindTextSegment(dl_phdr_info* info, size_t, void* data) {
  auto* segment = static_cast<TextSegment*>(data);
  if (info->dlpi_addr != segment->base) return 0;
  for (int i = 0; i < info->dlpi_phnum; ++i) {
    const auto& header = info->dlpi_phdr[i];
    if (header.p_type == PT_LOAD && (header.p_flags & PF_X) != 0) {
      segment->start = info->dlpi_addr + header.p_vaddr;
      segment->end = segment->start + header.p_memsz;
      break;
    }
  }
  return 1;
}

// perf cannot symbolize anonymous text; publish the module's function symbols
// for the remapped range in the JIT map format when profiling asks for it.
void WriteHugeTextPerfMap(const char* path, ElfW(Addr) base, std::uintptr_t begin,
                          std::uintptr_t end) {
  std::ifstream file(path, std::ios::binary);
  std::vector<char> image((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
  if (image.size() < sizeof(ElfW(Ehdr))) return;
  const auto* header = reinterpret_cast<const ElfW(Ehdr)*>(image.data());
  if (header->e_shoff == 0 || header->e_shoff + std::size_t(header->e_shnum) * sizeof(ElfW(Shdr)) > image.size())
    return;
  const auto* sections = reinterpret_cast<const ElfW(Shdr)*>(image.data() + header->e_shoff);
  char map_path[64];
  std::snprintf(map_path, sizeof(map_path), "/tmp/perf-%d.map", static_cast<int>(getpid()));
  std::FILE* out = std::fopen(map_path, "a");
  if (!out) return;
  for (unsigned i = 0; i < header->e_shnum; ++i) {
    if (sections[i].sh_type != SHT_SYMTAB || sections[i].sh_link >= header->e_shnum) continue;
    const auto& strings = sections[sections[i].sh_link];
    if (sections[i].sh_offset + sections[i].sh_size > image.size() ||
        strings.sh_offset + strings.sh_size > image.size()) continue;
    const auto* symbols = reinterpret_cast<const ElfW(Sym)*>(image.data() + sections[i].sh_offset);
    const std::size_t count = sections[i].sh_size / sizeof(ElfW(Sym));
    for (std::size_t k = 0; k < count; ++k) {
      const auto& symbol = symbols[k];
      if (ELF64_ST_TYPE(symbol.st_info) != STT_FUNC || symbol.st_size == 0 ||
          symbol.st_name >= strings.sh_size) continue;
      const std::uintptr_t address = base + symbol.st_value;
      if (address < begin || address >= end) continue;
      std::fprintf(out, "%lx %lx %s\n", static_cast<unsigned long>(address),
                   static_cast<unsigned long>(symbol.st_size),
                   image.data() + strings.sh_offset + symbol.st_name);
    }
  }
  std::fclose(out);
}

void RemapModuleTextToHugePages(void* handle) {
  const char* setting = std::getenv("GEKKOAOT_HUGE_TEXT");
  if (!handle || (setting && setting[0] == '0')) return;
  link_map* map = nullptr;
  if (dlinfo(handle, RTLD_DI_LINKMAP, &map) != 0 || !map) return;
  TextSegment segment{map->l_addr};
  dl_iterate_phdr(FindTextSegment, &segment);
  constexpr std::uintptr_t kHuge = std::uintptr_t{2} << 20;
  const std::uintptr_t begin = (segment.start + kHuge - 1u) & ~(kHuge - 1u);
  const std::uintptr_t end = segment.end & ~(kHuge - 1u);
  // Small modules (REL side modules, test fixtures) gain nothing.
  if (segment.start == 0u || end <= begin || end - begin < 4u * kHuge) return;
  const std::size_t bytes = end - begin;

  void* reserved = mmap(nullptr, bytes + kHuge, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (reserved == MAP_FAILED) return;
  const auto raw = reinterpret_cast<std::uintptr_t>(reserved);
  const std::uintptr_t aligned = (raw + kHuge - 1u) & ~(kHuge - 1u);
  if (aligned != raw) munmap(reserved, aligned - raw);
  if (const std::uintptr_t tail = raw + bytes + kHuge - (aligned + bytes); tail != 0u)
    munmap(reinterpret_cast<void*>(aligned + bytes), tail);
  auto* copy = reinterpret_cast<void*>(aligned);
  madvise(copy, bytes, MADV_HUGEPAGE);
  std::memcpy(copy, reinterpret_cast<const void*>(begin), bytes);
  if (mprotect(copy, bytes, PROT_READ | PROT_EXEC) != 0 ||
      mremap(copy, bytes, bytes, MREMAP_MAYMOVE | MREMAP_FIXED,
             reinterpret_cast<void*>(begin)) == MAP_FAILED) {
    munmap(copy, bytes);
    std::fprintf(stderr, "GEKKOAOT_HUGE_TEXT_V326=0 reason=remap-failed bytes=%zu\n", bytes);
    return;
  }
  const char* perf_map = std::getenv("GEKKOAOT_HUGE_TEXT_PERF_MAP");
  if (perf_map && perf_map[0] == '1')
    WriteHugeTextPerfMap(map->l_name, map->l_addr, begin, end);
  std::fprintf(stderr, "GEKKOAOT_HUGE_TEXT_V326=1 module=\"%s\" text_mib=%zu huge_mib=%zu perf_map=%u\n",
               map->l_name, static_cast<std::size_t>((segment.end - segment.start) >> 20),
               bytes >> 20, perf_map && perf_map[0] == '1' ? 1u : 0u);
}
#endif

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
          end > 0x100000000ull) return false;
      // linked_start is a compiler-assigned identity, not a live RAM address.
      // Hundreds of mutually exclusive overlays can exceed retail MEM1 in
      // this namespace. RefreshRelMappings separately validates their actual
      // OSLink addresses and sizes before publishing any runtime mapping.
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
#if defined(__linux__)
  RemapModuleTextToHugePages(handle_);
#endif
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
#ifdef _WIN32
  rel_import_proofs_ = reinterpret_cast<GekkoAOTRelImportProofsFn>(
      GetProcAddress(reinterpret_cast<HMODULE>(handle_), "gekkoaot_rel_import_proofs"));
#else
  dlerror();
  rel_import_proofs_ = reinterpret_cast<GekkoAOTRelImportProofsFn>(
      dlsym(handle_, "gekkoaot_rel_import_proofs"));
  (void)dlerror();
#endif
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

bool ModuleLibrary::BindNativeHardware(const GekkoAOTNativeHardware& services) const {
  if (!handle_) return false;
#ifdef _WIN32
  auto bind = reinterpret_cast<GekkoAOTBindNativeHardware>(
      GetProcAddress(reinterpret_cast<HMODULE>(handle_), "gekkoaot_bind_native_hardware"));
#else
  dlerror();
  auto bind = reinterpret_cast<GekkoAOTBindNativeHardware>(
      dlsym(handle_, "gekkoaot_bind_native_hardware"));
  (void)dlerror();
#endif
  return bind && bind(&services) == 1;
}

bool ModuleLibrary::BindNativeWgpipeTry(const GekkoAOTNativeWgpipeTry& services) const {
  if (!handle_) return false;
#ifdef _WIN32
  auto bind = reinterpret_cast<GekkoAOTBindNativeWgpipeTry>(
      GetProcAddress(reinterpret_cast<HMODULE>(handle_), "gekkoaot_bind_native_wgpipe_try"));
#else
  dlerror();
  auto bind = reinterpret_cast<GekkoAOTBindNativeWgpipeTry>(
      dlsym(handle_, "gekkoaot_bind_native_wgpipe_try"));
  (void)dlerror();
#endif
  return bind && bind(&services) == 1;
}

bool ModuleLibrary::BindNativeStableMmio(const GekkoAOTNativeStableMmio& services) const {
  if (!handle_) return false;
#ifdef _WIN32
  auto bind = reinterpret_cast<GekkoAOTBindNativeStableMmio>(
      GetProcAddress(reinterpret_cast<HMODULE>(handle_), "gekkoaot_bind_native_stable_mmio"));
#else
  dlerror();
  auto bind = reinterpret_cast<GekkoAOTBindNativeStableMmio>(
      dlsym(handle_, "gekkoaot_bind_native_stable_mmio"));
  (void)dlerror();
#endif
  return bind && bind(&services) == 1;
}

bool ModuleLibrary::BindNativeAddressWindow(const GekkoAOTNativeAddressWindow& services) const {
  if (!handle_) return false;
#ifdef _WIN32
  auto bind = reinterpret_cast<GekkoAOTBindNativeAddressWindow>(
      GetProcAddress(reinterpret_cast<HMODULE>(handle_), "gekkoaot_bind_native_address_window"));
#else
  dlerror();
  auto bind = reinterpret_cast<GekkoAOTBindNativeAddressWindow>(
      dlsym(handle_, "gekkoaot_bind_native_address_window"));
  (void)dlerror();
#endif
  return bind && bind(&services) == 1;
}

bool ModuleLibrary::BindNativeFlatVmem(const GekkoAOTNativeFlatVmem& services) const {
  if (!handle_) return false;
#ifdef _WIN32
  auto bind = reinterpret_cast<GekkoAOTBindNativeFlatVmem>(
      GetProcAddress(reinterpret_cast<HMODULE>(handle_), "gekkoaot_bind_native_flat_vmem"));
#else
  dlerror();
  auto bind = reinterpret_cast<GekkoAOTBindNativeFlatVmem>(
      dlsym(handle_, "gekkoaot_bind_native_flat_vmem"));
  (void)dlerror();
#endif
  if (!bind) return false;
  if (bind(&services) == 1) return true;
  // v322: modules built before the paged mirror accept only version 1. Its
  // view prefix (data, size) is unchanged, so the same getter remains valid.
  GekkoAOTNativeFlatVmem v1 = services;
  v1.version = 1u;
  return bind(&v1) == 1;
}

bool ModuleLibrary::BindNativeLockedCache(const GekkoAOTNativeLockedCache& services) const {
  if (!handle_) return false;
#ifdef _WIN32
  auto bind = reinterpret_cast<GekkoAOTBindNativeLockedCache>(
      GetProcAddress(reinterpret_cast<HMODULE>(handle_), "gekkoaot_bind_native_locked_cache"));
#else
  dlerror();
  auto bind = reinterpret_cast<GekkoAOTBindNativeLockedCache>(
      dlsym(handle_, "gekkoaot_bind_native_locked_cache"));
  (void)dlerror();
#endif
  return bind && bind(&services) == 1;
}

void ModuleLibrary::Close() {
  descriptor_ = nullptr;
  capabilities_ = 0u;
  rel_import_proofs_ = nullptr;
  if (!handle_) return;
#ifdef _WIN32
  FreeLibrary(reinterpret_cast<HMODULE>(handle_));
#else
  dlclose(handle_);
#endif
  handle_ = nullptr;
}

} // namespace GekkoAOT::Native
