// SPDX-License-Identifier: GPL-3.0-or-later
#include "native/runtime.h"
#include "native/sdk_autoresolver.h"

#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

struct Options {
  std::filesystem::path module;
  std::filesystem::path dol;
  std::filesystem::path boot_bin;
  std::filesystem::path fst_bin;
  std::filesystem::path bi2_bin;
  std::filesystem::path disc_image;
  std::filesystem::path native_gx_bridge;
  std::filesystem::path sdk_manifest;
  std::filesystem::path emit_sdk_manifest;
  std::filesystem::path stop_file;
  std::vector<std::filesystem::path> secondary_modules;
  std::string game_id;
  GekkoAOT::Native::GameCubeBootConfig::VideoMode video_mode =
      GekkoAOT::Native::GameCubeBootConfig::VideoMode::Auto;
  std::uint64_t dispatch_limit = 0; // 0 = unlimited; use --dispatch-limit for diagnostics/tests
  bool mem2 = false;
  bool probe_module = false;
  std::vector<std::pair<GekkoAOT::NativeOS::Kind, std::uint32_t>> os_hooks;
  std::vector<std::pair<GekkoAOT::Native::SimpleHleKind, std::uint32_t>> simple_hooks;
};

// Bounded read-only snapshot at a stop/failure, never in the dispatch hot path.
// Thread fields use the same retail SDK ABI as NativeOS; a wait source is not
// inferred merely from a queue address.
void PrintRuntimeSnapshot(const GekkoAOT::Native::HostRuntime& runtime) {
  const auto& cpu = runtime.Cpu();
  const auto& memory = runtime.Memory();
  const auto& cp = runtime.CommandProcessor();
  const auto& vi = runtime.VideoInterface();
  const auto word = [&](std::uint32_t address) {
    std::uint32_t value = 0;
    memory.Read32(address, &value);
    return value;
  };
  std::cout << "GEKKOAOT_FINAL_CPU_V96=1" << std::hex
            << " pc=" << cpu.pc << " lr=" << cpu.lr << " cr=" << cpu.cr
            << " ctr=" << cpu.ctr << " msr=" << cpu.msr
            << " dar=" << cpu.dar << " dsisr=" << cpu.dsisr;
  for (unsigned i = 0; i < 32; ++i)
    std::cout << " r" << std::dec << i << "=" << std::hex << cpu.gpr[i];
  std::cout << "\nGEKKOAOT_FINAL_CP_V96=1 base=" << cp.FifoBase()
            << " end=" << cp.FifoEnd() << " read=" << cp.FifoReadPointer()
            << " write=" << cp.FifoWritePointer() << " distance=" << cp.FifoReadWriteDistance()
            << " breakpoint=" << cp.FifoBreakpoint() << " control=" << cp.Control()
            << " status=" << cp.Status() << " irq=" << cp.InterruptPending()
            << " pi_write=" << runtime.ProcessorInterface().FifoWritePointer();
  std::uint64_t beam = 0;
  vi.Read(0xcc00202cu, 4, &beam);
  std::cout << "\nGEKKOAOT_FINAL_VI_V96=1 beam=" << beam
            << " halfline=" << vi.HalfLineCount() << " irq=" << vi.InterruptPending()
            << " top=" << vi.XfbAddressTop() << " bottom=" << vi.XfbAddressBottom()
            << std::dec << " width=" << vi.XfbWidthPixels()
            << " stride=" << vi.XfbStrideBytes() << " field_height=" << vi.XfbFieldHeight();
  const auto current = word(0x800000e4u);
  std::cout << "\nGEKKOAOT_FINAL_OS_V96=1" << std::hex
            << " current=" << current << " context=" << word(0x800000d4u)
            << " active_head=" << word(0x800000dcu) << " wake_source=unresolved\n";
  std::array<std::uint32_t, 16> seen{};
  auto thread = word(0x800000dcu);
  if (!thread) thread = current;
  for (unsigned n = 0; thread && n < seen.size(); ++n) {
    if (std::find(seen.begin(), seen.end(), thread) != seen.end()) break;
    seen[n] = thread;
    if (!memory.Resolve(thread, 0x30cu)) {
      std::cout << "GEKKOAOT_FINAL_THREAD_V96=1 address=" << thread << " mapped=0\n";
      break;
    }
    std::uint16_t state = 0;
    memory.Read16(thread + 0x2c8u, &state);
    const auto queue = word(thread + 0x2dcu);
    const auto mutex = word(thread + 0x2f0u);
    std::cout << "GEKKOAOT_FINAL_THREAD_V96=1 address=" << thread
              << " state=" << state << " suspend=" << word(thread + 0x2ccu)
              << " priority=" << word(thread + 0x2d0u) << " queue=" << queue
              << " mutex=" << mutex << " saved_pc=" << word(thread + 0x198u)
              << " saved_lr=" << word(thread + 0x84u);
    if (queue && memory.Resolve(queue, 8))
      std::cout << " queue_head=" << word(queue) << " queue_tail=" << word(queue + 4u);
    if (mutex && memory.Resolve(mutex, 16))
      std::cout << " mutex_owner=" << word(mutex + 8u) << " mutex_count=" << word(mutex + 12u);
    std::cout << "\n";
    thread = word(thread + 0x2fcu);
  }
  std::cout << std::dec;
}

void Usage(const char* argv0) {
  std::cerr
      << "usage: " << argv0 << " --module MODULE [--dol main.dol] [options]\n"
      << "  --game-id ID             validate six-character game ID\n"
      << "  --boot-bin FILE          initialize GameCube low-memory/CPU boot state\n"
      << "  --fst-bin FILE           load apploader FST metadata into high MEM1\n"
      << "  --bi2-bin FILE           load BI2 and publish 0x800000F4 boot pointer\n"
      << "  --disc-image FILE        attach GameCube media through native nod/DI\n"
      << "  --video-mode MODE        auto, ntsc or pal (default: auto)\n"
      << "  --native-gx-bridge FILE  dlopen Native GX/Aurora renderer plugin\n"
      << "  --sdk-manifest FILE      load build-time resolved native SDK/HLE hooks\n"
      << "  --emit-sdk-manifest FILE resolve SDK/HLE hooks, write manifest and exit\n"
      << "  --stop-file FILE          poll for a graceful stop request between native slices\n"
      << "  --secondary-module FILE  load native REL/overlay side module (repeatable)\n"
      << "  --dispatch-limit N       stop after N native dispatches (0 = unlimited, default)\n"
      << "  --mem2                   allocate 64 MiB MEM2\n"
      << "  --probe-module           validate ABI/module and exit\n"
      << "  --os-hook NAME=ADDRESS   register native OS HLE entrypoint\n"
      << "  --hle-hook NAME=ADDRESS  register time/libc SDK HLE entrypoint\n";
}

std::optional<std::uint64_t> ParseInteger(std::string_view text) {
  std::uint64_t value = 0;
  int base = 10;
  if (text.starts_with("0x") || text.starts_with("0X")) {
    text.remove_prefix(2);
    base = 16;
  }
  if (text.empty()) return std::nullopt;
  const auto [ptr, error] = std::from_chars(text.data(), text.data() + text.size(), value, base);
  if (error != std::errc{} || ptr != text.data() + text.size()) return std::nullopt;
  return value;
}

std::optional<GekkoAOT::NativeOS::Kind> ParseOsKind(std::string_view name) {
#define GEKKOAOT_OS_KIND(x) if (name == GekkoAOT::NativeOS::Name(GekkoAOT::NativeOS::Kind::x)) return GekkoAOT::NativeOS::Kind::x
  GEKKOAOT_OS_KIND(InitThreadQueue); GEKKOAOT_OS_KIND(GetCurrentThread);
  GEKKOAOT_OS_KIND(IsThreadTerminated); GEKKOAOT_OS_KIND(DisableScheduler);
  GEKKOAOT_OS_KIND(EnableScheduler); GEKKOAOT_OS_KIND(YieldThread);
  GEKKOAOT_OS_KIND(CreateThread); GEKKOAOT_OS_KIND(ExitThread);
  GEKKOAOT_OS_KIND(CancelThread); GEKKOAOT_OS_KIND(JoinThread);
  GEKKOAOT_OS_KIND(DetachThread); GEKKOAOT_OS_KIND(ResumeThread);
  GEKKOAOT_OS_KIND(SuspendThread); GEKKOAOT_OS_KIND(SleepThread);
  GEKKOAOT_OS_KIND(WakeupThread); GEKKOAOT_OS_KIND(GetThreadPriority);
  GEKKOAOT_OS_KIND(SaveContext); GEKKOAOT_OS_KIND(LoadContext);
  GEKKOAOT_OS_KIND(ClearContext); GEKKOAOT_OS_KIND(InitContext);
  GEKKOAOT_OS_KIND(GetCurrentContext); GEKKOAOT_OS_KIND(SetCurrentContext);
  GEKKOAOT_OS_KIND(InitMutex); GEKKOAOT_OS_KIND(LockMutex);
  GEKKOAOT_OS_KIND(UnlockMutex); GEKKOAOT_OS_KIND(TryLockMutex);
  GEKKOAOT_OS_KIND(InitCond); GEKKOAOT_OS_KIND(WaitCond); GEKKOAOT_OS_KIND(SignalCond);
  GEKKOAOT_OS_KIND(InitMessageQueue); GEKKOAOT_OS_KIND(SendMessage);
  GEKKOAOT_OS_KIND(ReceiveMessage); GEKKOAOT_OS_KIND(JamMessage);
  GEKKOAOT_OS_KIND(InitAlarm); GEKKOAOT_OS_KIND(CreateAlarm); GEKKOAOT_OS_KIND(SetAlarm);
  GEKKOAOT_OS_KIND(SetAbsAlarm); GEKKOAOT_OS_KIND(SetPeriodicAlarm);
  GEKKOAOT_OS_KIND(CancelAlarm); GEKKOAOT_OS_KIND(CheckAlarmQueue);
  GEKKOAOT_OS_KIND(DisableInterrupts); GEKKOAOT_OS_KIND(EnableInterrupts);
  GEKKOAOT_OS_KIND(RestoreInterrupts);
#undef GEKKOAOT_OS_KIND
  return std::nullopt;
}

template<class Kind>
bool ParseHook(std::string_view value, std::optional<Kind> (*kind_parser)(std::string_view),
               std::vector<std::pair<Kind, std::uint32_t>>& out) {
  const auto eq = value.find('=');
  if (eq == std::string_view::npos) return false;
  auto kind = kind_parser(value.substr(0, eq));
  auto address = ParseInteger(value.substr(eq + 1));
  if (!kind || !address || *address > 0xffffffffull || *address == 0) return false;
  out.emplace_back(*kind, static_cast<std::uint32_t>(*address));
  return true;
}

std::optional<GekkoAOT::Native::SimpleHleKind> ParseSimpleKind(std::string_view name) {
  using Kind = GekkoAOT::Native::SimpleHleKind;
  if (name == "OSGetTime") return Kind::OsGetTime;
  if (name == "OSGetTick") return Kind::OsGetTick;
  if (name == "memcpy" || name == "__memcpy") return Kind::Memcpy;
  if (name == "memmove") return Kind::Memmove;
  if (name == "memset" || name == "__memset") return Kind::Memset;
  if (name == "memcmp") return Kind::Memcmp;
  if (name == "bzero") return Kind::Bzero;
  return std::nullopt;
}

std::optional<GekkoAOT::Native::GameCubeBootConfig::VideoMode>
ParseVideoMode(std::string_view name) {
  using Mode = GekkoAOT::Native::GameCubeBootConfig::VideoMode;
  if (name == "auto") return Mode::Auto;
  if (name == "ntsc") return Mode::Ntsc;
  if (name == "pal") return Mode::Pal;
  return std::nullopt;
}

std::string EscapeForLog(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (const char ch : text) {
    switch (ch) {
    case '\\': out += "\\\\"; break;
    case '"': out += "\\\""; break;
    case '\n': out += "\\n"; break;
    case '\r': out += "\\r"; break;
    case '\t': out += "\\t"; break;
    default: out.push_back(ch); break;
    }
  }
  return out;
}


std::optional<Options> ParseOptions(int argc, char** argv) {
  Options options;
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];
    const auto next = [&]() -> std::optional<std::string_view> {
      if (i + 1 >= argc) return std::nullopt;
      return std::string_view(argv[++i]);
    };
    if (arg == "--module") { auto v = next(); if (!v) return {}; options.module = *v; }
    else if (arg == "--dol") { auto v = next(); if (!v) return {}; options.dol = *v; }
    else if (arg == "--boot-bin") { auto v = next(); if (!v) return {}; options.boot_bin = *v; }
    else if (arg == "--fst-bin") { auto v = next(); if (!v) return {}; options.fst_bin = *v; }
    else if (arg == "--bi2-bin") { auto v = next(); if (!v) return {}; options.bi2_bin = *v; }
    else if (arg == "--disc-image") { auto v = next(); if (!v) return {}; options.disc_image = *v; }
    else if (arg == "--game-id") { auto v = next(); if (!v) return {}; options.game_id = *v; }
    else if (arg == "--native-gx-bridge") { auto v = next(); if (!v) return {}; options.native_gx_bridge = *v; }
    else if (arg == "--sdk-manifest") { auto v = next(); if (!v) return {}; options.sdk_manifest = *v; }
    else if (arg == "--emit-sdk-manifest") { auto v = next(); if (!v) return {}; options.emit_sdk_manifest = *v; }
    else if (arg == "--stop-file") { auto v = next(); if (!v) return {}; options.stop_file = *v; }
    else if (arg == "--secondary-module") { auto v = next(); if (!v) return {}; options.secondary_modules.emplace_back(*v); }
    else if (arg == "--video-mode") {
      auto v = next(); if (!v) return {}; auto mode = ParseVideoMode(*v); if (!mode) return {};
      options.video_mode = *mode;
    }
    else if (arg == "--dispatch-limit") {
      auto v = next(); if (!v) return {}; auto n = ParseInteger(*v); if (!n) return {};
      options.dispatch_limit = *n;
    }
    else if (arg == "--mem2") options.mem2 = true;
    else if (arg == "--probe-module") options.probe_module = true;
    else if (arg == "--os-hook") {
      auto v = next(); if (!v || !ParseHook(*v, &ParseOsKind, options.os_hooks)) return {};
    }
    else if (arg == "--hle-hook") {
      auto v = next(); if (!v || !ParseHook(*v, &ParseSimpleKind, options.simple_hooks)) return {};
    }
    else if (arg == "-h" || arg == "--help") { Usage(argv[0]); std::exit(0); }
    else return {};
  }
  if (options.native_gx_bridge.empty()) {
    if (const char* bridge = std::getenv("GEKKOAOT_NATIVE_GX_BRIDGE"); bridge && *bridge)
      options.native_gx_bridge = bridge;
  }
  if (options.module.empty() && options.emit_sdk_manifest.empty()) return {};
  if (!options.probe_module && options.dol.empty()) return {};
  return options;
}

} // namespace

int main(int argc, char** argv) {
  const auto options = ParseOptions(argc, argv);
  if (!options) { Usage(argv[0]); return 2; }

  try {
    GekkoAOT::Native::HostRuntime runtime(options->mem2);
    const char* expected = options->game_id.empty() ? nullptr : options->game_id.c_str();
    if (!options->module.empty()) {
      const auto module_status = runtime.LoadModule(options->module, expected);
      if (module_status != GekkoAOT::Native::ModuleStatus::Ok) {
        std::cerr << "GEKKOAOT_NATIVE_MODULE_ERROR status="
                  << GekkoAOT::Native::ModuleStatusName(module_status)
                  << " detail=\"" << runtime.ModuleLibraryHandle().LastError() << "\"\n";
        return 3;
      }

      const auto* module = runtime.Module();
      std::cout << "GEKKOAOT_STANDALONE_HOST_V1=1 game_id=" << module->game_id
                << " entry=0x" << std::hex << module->entry_point << std::dec
                << " cpu_abi=" << module->cpu_abi_version
                << " module_abi=" << module->abi_version << "\n";
    } else {
      std::cout << "GEKKOAOT_SDK_SCAN_HOST_V10=1 module=none mode=dol-static-analysis\n";
    }

    for (const auto& secondary_path : options->secondary_modules) {
      const auto secondary_status = runtime.LoadSecondaryModule(secondary_path, expected);
      if (secondary_status != GekkoAOT::Native::ModuleStatus::Ok) {
        std::cerr << "GEKKOAOT_NATIVE_SECONDARY_MODULE_ERROR status="
                  << GekkoAOT::Native::ModuleStatusName(secondary_status)
                  << " path=\"" << secondary_path.string() << "\" detail=\""
                  << runtime.SecondaryModuleLastError() << "\"\n";
        return 8;
      }
    }
    if (!options->secondary_modules.empty())
      std::cout << "GEKKOAOT_NATIVE_SECONDARY_MODULES=" << runtime.SecondaryModuleCount() << "\n";
    if (options->probe_module) return 0;

    if (!options->boot_bin.empty()) {
      GekkoAOT::Native::GameCubeBootConfig boot_config;
      boot_config.video_mode = options->video_mode;
      const auto boot = runtime.InitializeGameCube(options->boot_bin, boot_config);
      std::cout << "GEKKOAOT_NATIVE_BOOT game_id=" << boot.game_id
                << " video=" << (boot.ntsc ? "ntsc" : "pal")
                << " rtc_ticks=" << boot.rtc_timebase_ticks << "\n";
    }
    if (!options->fst_bin.empty()) {
      const auto fst = runtime.LoadGameCubeFst(options->fst_bin);
      std::cout << "GEKKOAOT_NATIVE_FST address=0x" << std::hex << fst.address
                << " size=0x" << fst.size << std::dec << "\n";
    }
    if (!options->bi2_bin.empty()) {
      const auto bi2 = runtime.LoadGameCubeBi2(options->bi2_bin);
      std::cout << "GEKKOAOT_NATIVE_BI2 address=0x" << std::hex << bi2.address
                << " size=0x" << bi2.size << std::dec << "\n";
    }

    const auto dol = runtime.LoadDolImage(options->dol);
    std::cout << "GEKKOAOT_NATIVE_DOL_LOADED entry=0x" << std::hex << dol.entry_point
              << std::dec << " bytes=" << dol.loaded_bytes << "\n";

    // The normal path consumes the immutable build-time SDK/HLE plan. This is
    // the same plan supplied to DolRecomp for static interception pruning, so
    // compiler and runtime agree exactly on which guest entrypoints are native
    // services. The structural resolver remains the universal bootstrap path
    // used once during compilation and as a compatibility fallback.
    if (!options->emit_sdk_manifest.empty()) {
      GekkoAOT::Native::AutoRegisterNativeSdk(runtime, options->emit_sdk_manifest);
      return 0;
    }
    if (!options->sdk_manifest.empty()) {
      if (!GekkoAOT::Native::RegisterNativeSdkManifest(runtime, options->sdk_manifest)) {
        std::cerr << "GEKKOAOT_NATIVE_SDK_MANIFEST_ERROR path=\""
                  << options->sdk_manifest.string() << "\"\n";
        return 9;
      }
    } else {
      GekkoAOT::Native::AutoRegisterNativeSdk(runtime);
    }
    for (const auto& [kind, address] : options->os_hooks) runtime.RegisterOsHook(kind, address);
    for (const auto& [kind, address] : options->simple_hooks) runtime.RegisterSimpleHle(kind, address);

    if (!options->disc_image.empty()) {
      if (!runtime.AttachDiscImage(options->disc_image)) {
        std::cerr << "GEKKOAOT_NATIVE_DI_MEDIA_ERROR path=\""
                  << options->disc_image.string() << "\"\n";
        return 7;
      }
      std::cout << "GEKKOAOT_NATIVE_DI_MEDIA=1 path=\""
                << options->disc_image.string() << "\" backend="
                << runtime.DiscMediaBackend() << " format="
                << runtime.DiscMediaFormat() << "\n";
    }

    if (!options->native_gx_bridge.empty()) {
      if (!runtime.InitializeNativeGX(options->native_gx_bridge)) {
        std::cerr << "GEKKOAOT_NATIVE_GX_ERROR path=\"" << options->native_gx_bridge.string()
                  << "\" detail=\"" << runtime.NativeGXLastError() << "\"\n";
        return 6;
      }
      std::cout << "GEKKOAOT_NATIVE_GX_STANDALONE=1 bridge=\""
                << options->native_gx_bridge.string()
                << "\" fifo=0xCC008000 memory=host-native\n";
    }

    GekkoAOT::Native::RunResult result{};
    bool pgo_stop_observed = false;
    if (options->stop_file.empty()) {
      result = runtime.Run(options->dispatch_limit);
    } else {
      // PGO needs a normal process exit so compiler-rt writes its counters.
      // Run the AOT runtime in coarse slices only while a stop file is armed;
      // the normal Play path remains one uninterrupted native Run().
      // Explicit diagnostic mode samples guest progress at dispatch
      // boundaries without adding instrumentation to the AOT hot path.
      const bool trace_pc = [] {
        const char* value = std::getenv("GEKKOAOT_NATIVE_PC_TRACE");
        return value && value[0] == '1' && value[1] == '\0';
      }();
      const std::uint64_t stop_poll_dispatches = trace_pc ? 25000u : 250000u;
      std::uint64_t remaining = options->dispatch_limit;
      std::uint64_t total_dispatches = 0;
      std::uint64_t total_cycles = 0;
      while (true) {
        std::error_code stop_ec;
        if (std::filesystem::exists(options->stop_file, stop_ec)) {
          // Preserve an acknowledgement for the parent controller.  Merely
          // testing that the request file disappeared after native-run exits is
          // ambiguous because the file is absent before training starts too.
          // Renaming it proves that this process actually observed the GUI Stop.
          auto consumed = options->stop_file;
          consumed += ".consumed";
          std::error_code ack_ec;
          std::filesystem::remove(consumed, ack_ec);
          ack_ec.clear();
          std::filesystem::rename(options->stop_file, consumed, ack_ec);
          if (ack_ec) {
            // Keep shutdown functional even if an unusual filesystem refuses
            // the rename. The parent will then refuse crash salvage rather
            // than misclassifying an unrelated abort as a user Stop.
            std::filesystem::remove(options->stop_file, stop_ec);
          }
          pgo_stop_observed = true;
          result.status = GekkoAOT::Native::RunStatus::HostRequestedExit;
          break;
        }
        std::uint64_t slice = stop_poll_dispatches;
        if (options->dispatch_limit != 0) {
          if (remaining == 0) {
            result.status = GekkoAOT::Native::RunStatus::DispatchLimitReached;
            break;
          }
          slice = std::min(slice, remaining);
        }
        result = runtime.Run(slice);
        total_dispatches += result.dispatches;
        total_cycles += result.cycles;
        if (trace_pc) {
          std::uint32_t current_thread = 0;
          std::uint32_t active_head = 0;
          runtime.Memory().Read32(0x800000e4u, &current_thread);
          runtime.Memory().Read32(0x800000dcu, &active_head);
          std::cout << "GEKKOAOT_NATIVE_PC_TRACE_V105=1 dispatches=" << std::dec
                    << total_dispatches << " pc=" << std::hex << runtime.Cpu().pc
                    << " lr=" << runtime.Cpu().lr << " current=" << current_thread
                    << " active_head=" << active_head
                    << " cp_distance=" << runtime.CommandProcessor().FifoReadWriteDistance()
                    << " di_reads=" << std::dec << result.di_reads << '\n';
        }
        if (options->dispatch_limit != 0) remaining -= std::min(remaining, result.dispatches);
        if (result.status != GekkoAOT::Native::RunStatus::DispatchLimitReached)
          break;
      }
      result.dispatches = total_dispatches;
      result.cycles = total_cycles;
      std::cout << "GEKKOAOT_GRACEFUL_STOP_V20=1 file=\""
                << options->stop_file.string() << "\" dispatches="
                << result.dispatches << " observed=" << (pgo_stop_observed ? 1 : 0)
                << "\n" << std::flush;
    }

    // Stop the asynchronous renderer before emitting the final result and
    // before HostRuntime starts tearing down input/disc/memory in its destructor.
    if (!pgo_stop_observed) runtime.ShutdownNativeGX();

    std::cout << "GEKKOAOT_NATIVE_RUN_RESULT status=\""
              << GekkoAOT::Native::RunStatusName(result.status) << "\" pc=0x"
              << std::hex << result.program_counter << " fault=0x" << result.fault_address
              << " fault_value=0x" << result.fault_value
              << " lr=0x" << result.link_register
              << " msr=0x" << result.msr
              << " srr0=0x" << result.srr0
              << " srr1=0x" << result.srr1
              << " exception=0x" << result.exception
              << " pi_cause=0x" << result.pi_cause
              << " pi_mask=0x" << result.pi_mask
              << " tb=0x" << result.timebase
              << " di_reads=" << std::dec << result.di_reads
              << " di_bytes=" << result.di_read_bytes
              << " di_failures=" << result.di_read_failures
              << " di_opcode=0x" << std::hex << result.di_last_opcode
              << " di_cmd0=0x" << result.di_command0
              << " di_status=0x" << result.di_status
              << " di_ctl=0x" << result.di_control
              << " di_error=0x" << result.di_last_error
              << " dec=0x" << result.decrementer
              << " dec_pending=" << std::dec << (result.decrementer_pending ? 1 : 0)
              << std::hex << " mmio_last=0x" << result.last_mmio_read
              << std::dec << " mmio_reads=" << result.mmio_reads
              << " mmio_same=" << result.same_mmio_read_streak
              << std::hex << " dsp_ctl=0x" << result.dsp_control
              << " dsp_mail=0x" << result.dsp_mailbox
              << std::dec << " dsp_mail_pending=" << (result.dsp_mail_pending ? 1 : 0)
              << " dsp_bootstrap_loaded=" << (result.dsp_bootstrap_loaded ? 1 : 0)
              << " dsp_bootstrap_running=" << (result.dsp_bootstrap_running ? 1 : 0)
              << " dsp_bootstrap_completed=" << (result.dsp_bootstrap_completed ? 1 : 0)
              << " dsp_task_stage=" << static_cast<unsigned>(result.dsp_task_loader_stage)
              << " dsp_task_pending=" << (result.dsp_task_init_pending ? 1 : 0)
              << " dsp_task_inits=" << result.dsp_task_init_count
              << std::hex << " dsp_cpu_mail=0x" << result.dsp_cpu_mail
              << std::dec << " dsp_cpu_mails=" << result.dsp_cpu_mails
              << " dsp_native=" << (result.dsp_native_core ? 1 : 0)
              << " dsp_rom=" << (result.dsp_rom_loaded ? 1 : 0)
              << std::hex << " dsp_pc=0x" << result.dsp_native_pc
              << " dsp_op=0x" << result.dsp_native_last_opcode
              << std::dec << " dsp_insns=" << result.dsp_native_instructions
              << " dsp_audio_frames=" << result.dsp_audio_frames
              << std::hex << " sp=0x" << result.stack_pointer
              << " halt_w08=0x" << result.halt_stack_word_08
              << " halt_w0c=0x" << result.halt_stack_word_0c
              << " halt_w10=0x" << result.halt_stack_word_10
              << " halt_w94=0x" << result.halt_stack_word_94
              << std::dec << " dispatches=" << result.dispatches
              << " secondary_dispatches=" << result.secondary_dispatches
              << " rel_refreshes=" << result.rel_mapping_refreshes
              << " rel_addr_xlate=" << result.rel_address_translations
              << " rel_addr_miss=" << result.rel_address_misses
              << std::hex << " rel_addr_last=0x" << result.rel_last_canonical
              << "->0x" << result.rel_last_runtime
              << std::dec << " hle_calls=" << result.hle_calls << " cycles=" << result.cycles;
    if (!result.halt_text_08.empty())
      std::cout << " halt_text08=\"" << EscapeForLog(result.halt_text_08) << "\"";
    if (!result.halt_text_10.empty())
      std::cout << " halt_text10=\"" << EscapeForLog(result.halt_text_10) << "\"";
    std::cout << "\n";
    if (result.status != GekkoAOT::Native::RunStatus::Completed)
      PrintRuntimeSnapshot(runtime);
    std::cout << std::flush;

    // PGO Stop is a process-lifetime boundary. Flush compiler-rt while the
    // instrumented module and its counters are still unquestionably alive,
    // then exit without tearing Aurora/Dawn down in-process. The OS owns the
    // remaining renderer resources and this avoids async WebGPU teardown
    // callbacks hitting stale allocations after the profile has been captured.
    if (pgo_stop_observed) {
      const int profile_rc = runtime.ModuleLibraryHandle().FlushProfile();
      std::cout << "GEKKOAOT_PGO_EXPLICIT_FLUSH_V20=1 rc=" << profile_rc
                << " policy=flush-before-renderer-teardown\n" << std::flush;
      if (profile_rc != 0) {
        std::cerr << "[gekkoaot] error: explicit PGO profile flush failed (rc="
                  << profile_rc << ")\n" << std::flush;
        return 10;
      }
      std::cout << "GEKKOAOT_PGO_FAST_EXIT_V20=1 renderer-teardown=skipped reason=acknowledged-stop\n"
                << std::flush;
      std::_Exit(0);
    }

    return (result.status == GekkoAOT::Native::RunStatus::Completed ||
            result.status == GekkoAOT::Native::RunStatus::HostRequestedExit) ? 0 : 4;
  } catch (const std::exception& e) {
    std::cerr << "GEKKOAOT_NATIVE_HOST_ERROR \"" << e.what() << "\"\n";
    return 5;
  }
}
