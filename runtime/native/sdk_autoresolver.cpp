// SPDX-License-Identifier: GPL-3.0-or-later
#include "native/sdk_autoresolver.h"

#include "native/runtime.h"
#include "sdk/os_primitives.h"
#include "sdk/sdk_groundtruth_v1.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

namespace GekkoAOT::Native {
namespace {

using Resolver = GekkoAOTSdk::StructuralResolver;
using OsKind = GekkoAOT::NativeOS::Kind;

bool EnvEnabled(const char* name, bool fallback = true) {
  const char* value = std::getenv(name);
  if (!value || !*value) return fallback;
  const std::string_view text(value);
  return text != "0" && text != "false" && text != "off" && text != "no";
}

std::string_view EnvText(const char* name, std::string_view fallback) {
  const char* value = std::getenv(name);
  return value && *value ? std::string_view(value) : fallback;
}

constexpr std::array<OsKind, 46> kAllOsKinds = {
    OsKind::InitThreadQueue, OsKind::GetCurrentThread, OsKind::IsThreadTerminated,
    OsKind::DisableScheduler, OsKind::EnableScheduler, OsKind::SelectThread,
    OsKind::Reschedule, OsKind::YieldThread,
    OsKind::CreateThread, OsKind::ExitThread, OsKind::CancelThread,
    OsKind::JoinThread, OsKind::DetachThread, OsKind::ResumeThread,
    OsKind::SuspendThread, OsKind::SleepThread, OsKind::WakeupThread,
    OsKind::SetThreadPriority, OsKind::GetThreadPriority, OsKind::SaveContext, OsKind::LoadContext,
    OsKind::ClearContext, OsKind::InitContext, OsKind::GetCurrentContext,
    OsKind::SetCurrentContext, OsKind::InitMutex, OsKind::LockMutex,
    OsKind::UnlockMutex, OsKind::TryLockMutex, OsKind::InitCond,
    OsKind::WaitCond, OsKind::SignalCond, OsKind::InitMessageQueue,
    OsKind::SendMessage, OsKind::ReceiveMessage, OsKind::JamMessage,
    OsKind::InitAlarm, OsKind::CreateAlarm, OsKind::SetAlarm,
    OsKind::SetAbsAlarm, OsKind::SetPeriodicAlarm, OsKind::CancelAlarm,
    OsKind::CheckAlarmQueue, OsKind::DisableInterrupts, OsKind::EnableInterrupts,
    OsKind::RestoreInterrupts,
};

std::optional<OsKind> OsKindForName(std::string_view name) {
  if (name == "__OSInitThreadQueue") return OsKind::InitThreadQueue;
  if (name == "__OSGetCurrentThread") return OsKind::GetCurrentThread;
  for (const auto kind : kAllOsKinds)
    if (name == GekkoAOT::NativeOS::Name(kind)) return kind;
  return std::nullopt;
}

std::optional<SimpleHleKind> SimpleKindForName(std::string_view name) {
  if (name == "OSGetTime") return SimpleHleKind::OsGetTime;
  if (name == "OSGetTick") return SimpleHleKind::OsGetTick;
  if (name == "memcpy" || name == "__memcpy") return SimpleHleKind::Memcpy;
  if (name == "memmove") return SimpleHleKind::Memmove;
  if (name == "memset" || name == "__memset") return SimpleHleKind::Memset;
  if (name == "memcmp") return SimpleHleKind::Memcmp;
  if (name == "bzero") return SimpleHleKind::Bzero;
  return std::nullopt;
}

const char* SimpleKindName(SimpleHleKind kind) {
  switch (kind) {
  case SimpleHleKind::OsGetTime: return "OSGetTime";
  case SimpleHleKind::OsGetTick: return "OSGetTick";
  case SimpleHleKind::Memcpy: return "memcpy";
  case SimpleHleKind::Memmove: return "memmove";
  case SimpleHleKind::Memset: return "memset";
  case SimpleHleKind::Memcmp: return "memcmp";
  case SimpleHleKind::Bzero: return "bzero";
  }
  return "?";
}

constexpr std::uint32_t kStaticDirectOsBase = 0xfffd0000u;
constexpr std::uint32_t kStaticDirectSimpleBase = 0xfffc0000u;

std::uint32_t StaticDirectToken(std::string_view name) {
  if (const auto kind = OsKindForName(name))
    return kStaticDirectOsBase | static_cast<std::uint8_t>(*kind);
  if (const auto kind = SimpleKindForName(name))
    return kStaticDirectSimpleBase | static_cast<std::uint8_t>(*kind);
  return 0u;
}

// v76: some NativeOS entrypoints are not ordinary leaf replacements. They can
// snapshot the currently running thread, switch to another OSContext, wake a
// higher-priority thread, or re-enable an interrupt that immediately captures
// the interrupted architectural state. Their direct-token ABI therefore needs
// the caller's preserved PPC context, not merely the registers used by the SDK
// function body itself.
bool IsContextCaptureBoundary(OsKind kind) {
  switch (kind) {
  case OsKind::EnableScheduler:
  case OsKind::SelectThread:
  case OsKind::Reschedule:
  case OsKind::YieldThread:
  case OsKind::ExitThread:
  case OsKind::CancelThread:
  case OsKind::JoinThread:
  case OsKind::DetachThread:
  case OsKind::ResumeThread:
  case OsKind::SuspendThread:
  case OsKind::SleepThread:
  case OsKind::WakeupThread:
  case OsKind::SetThreadPriority:
  case OsKind::SaveContext:
  case OsKind::LoadContext:
  case OsKind::SetCurrentContext:
  case OsKind::LockMutex:
  case OsKind::UnlockMutex:
  case OsKind::TryLockMutex:
  case OsKind::WaitCond:
  case OsKind::SignalCond:
  case OsKind::SendMessage:
  case OsKind::ReceiveMessage:
  case OsKind::JamMessage:
  case OsKind::EnableInterrupts:
  case OsKind::RestoreInterrupts:
    return true;
  default:
    return false;
  }
}

bool IsOptionalForSchedulerClosure(OsKind kind) {
  // The native scheduler only needs the primitives that own run/wait queue
  // transitions to be a closed set.  Several SDK entrypoints are safe to leave
  // as guest AOT wrappers: they either only mutate ABI-visible objects or end
  // up in the core Sleep/Wakeup/Resume primitives below.  Requiring every
  // optional SDK API made stripped retail DOLs impossible to admit when, for
  // example, OSJoinThread or OSJamMessage was never linked at all.
  //
  // v71 makes closure admission LINKAGE-aware. High-level SDK wrappers are not
  // ownership roots: mutex/cond/message/yield ultimately transition threads via
  // Sleep/Wakeup/Resume/Suspend and can therefore be absent from a particular
  // link without disabling NativeOS. When they are linked and resolved they are
  // still installed natively. Guest-created threads are reconciled through the
  // ABI-visible OSThread/run queues. Alarm callbacks remain a separate closure.
  switch (kind) {
  case OsKind::GetCurrentThread:
  case OsKind::SelectThread:
  case OsKind::Reschedule:
  case OsKind::YieldThread:
  case OsKind::SetThreadPriority:
  case OsKind::GetThreadPriority:
  case OsKind::IsThreadTerminated:
  case OsKind::GetCurrentContext:
  case OsKind::CreateThread:
  case OsKind::ExitThread:
  case OsKind::CancelThread:
  case OsKind::JoinThread:
  case OsKind::DetachThread:
  case OsKind::InitContext:
  case OsKind::InitMutex:
  case OsKind::LockMutex:
  case OsKind::UnlockMutex:
  case OsKind::TryLockMutex:
  case OsKind::InitCond:
  case OsKind::WaitCond:
  case OsKind::SignalCond:
  case OsKind::InitMessageQueue:
  case OsKind::SendMessage:
  case OsKind::ReceiveMessage:
  case OsKind::JamMessage:
  case OsKind::InitAlarm:
  case OsKind::SetAbsAlarm:
  case OsKind::SetPeriodicAlarm:
  case OsKind::CheckAlarmQueue:
    return true;
  default:
    return false;
  }
}

bool IsAlarmGuestByDefault(OsKind kind) {
  // v68: the OSContext family is native again.  Its implementation is now tied
  // to the same NativeOS scheduler ownership and follows the SDK PPC field/queue
  // semantics used by retail titles.  Alarm callbacks remain guest-AOT by
  // default because their callback/reentry model is an independent closure.
  switch (kind) {
  case OsKind::InitAlarm:
  case OsKind::CreateAlarm:
  case OsKind::SetAlarm:
  case OsKind::SetAbsAlarm:
  case OsKind::SetPeriodicAlarm:
  case OsKind::CancelAlarm:
  case OsKind::CheckAlarmQueue:
    return true;
  default:
    return false;
  }
}

bool IsInterruptHelper(OsKind kind) {
  return kind == OsKind::DisableInterrupts || kind == OsKind::EnableInterrupts ||
         kind == OsKind::RestoreInterrupts;
}

struct Range {
  std::uint32_t start = 0;
  std::uint32_t end = 0;
};

std::vector<Range> CodeRanges(const GekkoAOTModuleDesc* module) {
  std::vector<Range> ranges;
  if (!module) return ranges;
  ranges.reserve(module->num_code_ranges);
  for (std::uint32_t i = 0; i < module->num_code_ranges; ++i) {
    const auto& in = module->code_ranges[i];
    if (!in.start || in.end <= in.start || (in.start & 3u) || (in.end & 3u)) continue;
    ranges.push_back({in.start, in.end});
  }
  std::sort(ranges.begin(), ranges.end(), [](const Range& a, const Range& b) {
    return std::tie(a.start, a.end) < std::tie(b.start, b.end);
  });
  std::vector<Range> merged;
  for (const auto& range : ranges) {
    if (!merged.empty() && range.start <= merged.back().end) {
      merged.back().end = std::max(merged.back().end, range.end);
    } else {
      merged.push_back(range);
    }
  }
  return merged;
}

std::vector<Range> CodeRanges(const std::vector<GekkoAOTRange>& input) {
  std::vector<Range> ranges;
  ranges.reserve(input.size());
  for (const auto& in : input) {
    if (!in.start || in.end <= in.start || (in.start & 3u) || (in.end & 3u)) continue;
    ranges.push_back({in.start, in.end});
  }
  std::sort(ranges.begin(), ranges.end(), [](const Range& a, const Range& b) {
    return std::tie(a.start, a.end) < std::tie(b.start, b.end);
  });
  std::vector<Range> merged;
  for (const auto& range : ranges) {
    if (!merged.empty() && range.start <= merged.back().end)
      merged.back().end = std::max(merged.back().end, range.end);
    else
      merged.push_back(range);
  }
  return merged;
}

const Range* ContainingRange(const std::vector<Range>& ranges, std::uint32_t address) {
  for (const auto& range : ranges)
    if (address >= range.start && address < range.end) return &range;
  return nullptr;
}

struct Prefix3 {
  std::uint32_t a = 0, b = 0, c = 0;
  bool operator<(const Prefix3& other) const {
    return std::tie(a, b, c) < std::tie(other.a, other.b, other.c);
  }
};

Prefix3 CandidatePrefix3(const std::uint8_t* code) {
  return {
      GekkoAOTSdk::StructuralInstructionSkeleton(GekkoAOTSdk::StructuralReadBE32(code)),
      GekkoAOTSdk::StructuralInstructionSkeleton(GekkoAOTSdk::StructuralReadBE32(code + 4)),
      GekkoAOTSdk::StructuralInstructionSkeleton(GekkoAOTSdk::StructuralReadBE32(code + 8)),
  };
}

std::uint64_t Prefix2(std::uint32_t a, std::uint32_t b) {
  return (std::uint64_t{a} << 32) | b;
}

void RepairSemanticOsIdentities(Resolver& resolver) {
  const auto& shapes = GekkoAOTSdk::GroundTruthShapes();

  // Collision-prone scheduler wrappers: require the +/-1 counter operation,
  // proven interrupt helpers and an exact reference body.
  for (const auto& [address, function] : resolver.Functions()) {
    const int delta = GekkoAOTSdk::DecodeOsSchedulerDelta(function.code, function.size);
    if (!delta || !resolver.Calls(address, "OSDisableInterrupts", 0.98) ||
        !resolver.Calls(address, "OSRestoreInterrupts", 0.98))
      continue;
    const char* identity = delta > 0 ? "OSDisableScheduler" : "OSEnableScheduler";
    bool exact = false;
    for (std::size_t i = 0; i < GekkoAOTSdk::kGroundTruthExemplars.size(); ++i) {
      const auto& exemplar = GekkoAOTSdk::kGroundTruthExemplars[i];
      if (exemplar.name == identity &&
          GekkoAOTSdk::GroundTruthExact(function.shape, shapes[i])) {
        exact = true;
        break;
      }
    }
    if (exact) resolver.Override(address, identity, 0.999, "scheduler-delta+interrupt-proof");
  }

  // Tiny leaf primitives have stable store semantics even when their raw DSY
  // hashes collide between SDK revisions.
  for (const auto& [address, function] : resolver.Functions()) {
    using Primitive = GekkoAOTSdk::OsPrimitive;
    const auto primitive = GekkoAOTSdk::DecodeOsPrimitive(function.code, function.size);
    const char* identity = nullptr;
    switch (primitive) {
    case Primitive::Mutex: identity = "OSInitMutex"; break;
    case Primitive::MessageQueue: identity = "OSInitMessageQueue"; break;
    case Primitive::Priority: identity = "OSGetThreadPriority"; break;
    default: break;
    }
    if (identity) resolver.Override(address, identity, 0.985, "instruction-semantics");
  }

  // The queue initializer is byte-collision-prone. Its use from OSInitMutex is
  // the semantic discriminator.
  for (const auto owner : resolver.Find("OSInitMutex", 0.98)) {
    const auto* function = resolver.Function(owner);
    if (!function) continue;
    for (const auto target : function->calls) {
      const auto* callee = resolver.Function(target);
      if (!callee) continue;
      if (GekkoAOTSdk::DecodeOsPrimitive(callee->code, callee->size) ==
          GekkoAOTSdk::OsPrimitive::Queue)
        resolver.Override(target, "OSInitThreadQueue", 0.99, "callee-of-OSInitMutex");
    }
  }

  for (const auto& [address, function] : resolver.Functions()) {
    if (GekkoAOTSdk::DecodeOsLoadContextShape(function.code, function.size))
      resolver.Override(address, "OSLoadContext", 0.99, "context-restore-semantics");
  }
  for (const auto clear : resolver.Find("OSClearContext", 0.98)) {
    for (const auto& [address, function] : resolver.Functions()) {
      if (GekkoAOTSdk::DecodeOsInitContext(function.code, function.size,
                                            function.address, clear))
        resolver.Override(address, "OSInitContext", 0.99,
                          "instruction-semantics+call");
    }
  }

  // OSInitCond and OSSignalCond can have the same normalized body. Their direct
  // callee separates them without relying on link order or absolute addresses.
  for (const auto& [address, function] : resolver.Functions()) {
    std::uint32_t target = 0;
    if (!GekkoAOTSdk::DecodeOsThinCallWrapper(function.code, function.size,
                                               function.address, &target))
      continue;
    const auto* callee = resolver.Resolution(target);
    if (!callee || resolver.IsAmbiguous(target) || callee->confidence < 0.98) continue;
    if (callee->name == "OSInitThreadQueue")
      resolver.Override(address, "OSInitCond", 0.999, "thin-wrapper-callee-proof");
    else if (callee->name == "OSWakeupThread")
      resolver.Override(address, "OSSignalCond", 0.999, "thin-wrapper-callee-proof");
  }

  static const std::vector<GekkoAOTSdk::CallGraphRule> kRules = {
      {"OSInitCond", 8, 96, {"OSInitThreadQueue"}, {}, 1, 2, 0.965},
      {"OSWaitCond", 64, 512, {"OSSleepThread", "OSLockMutex", "OSWakeupThread"},
       {"OSDisableInterrupts", "OSRestoreInterrupts"}, 3, 10, 0.975},
      {"OSSignalCond", 8, 160, {"OSWakeupThread"}, {}, 1, 3, 0.955},
      {"OSJoinThread", 80, 640, {"OSDisableInterrupts", "OSSleepThread", "OSRestoreInterrupts"},
       {}, 3, 10, 0.97},
      {"OSDetachThread", 48, 384, {"OSDisableInterrupts", "OSWakeupThread", "OSRestoreInterrupts"},
       {}, 3, 8, 0.97},
      {"OSCreateThread", 128, 1200, {"OSInitContext", "OSDisableInterrupts", "OSRestoreInterrupts"},
       {}, 3, 10, 0.98},
      {"OSExitThread", 96, 900, {"OSClearContext", "OSWakeupThread", "OSDisableInterrupts"},
       {"OSRestoreInterrupts"}, 3, 12, 0.975},
      {"OSTryLockMutex", 48, 480, {"OSDisableInterrupts", "OSGetCurrentThread", "OSRestoreInterrupts"},
       {}, 3, 8, 0.97},
      {"OSInitMessageQueue", 32, 192, {"OSInitThreadQueue"}, {}, 2, 4, 0.97},
      {"OSSetAbsAlarm", 48, 320, {"OSDisableInterrupts", "OSRestoreInterrupts"}, {}, 2, 6, 0.955},
  };
  for (unsigned pass = 0; pass < 8; ++pass) {
    std::size_t added = 0;
    for (const auto& rule : kRules) {
      std::vector<std::uint32_t> matches;
      for (const auto& [address, function] : resolver.Functions()) {
        if (resolver.Resolution(address) && !resolver.IsAmbiguous(address)) continue;
        if (GekkoAOTSdk::MatchCallGraphRule(resolver, function, rule))
          matches.push_back(address);
      }
      if (matches.empty()) continue;
      const auto* first = resolver.Function(matches.front());
      bool one_body = first != nullptr;
      for (const auto address : matches) {
        const auto* candidate = resolver.Function(address);
        one_body = one_body && candidate && first &&
                   candidate->shape.skeleton == first->shape.skeleton;
      }
      if (!one_body) continue;
      for (const auto address : matches)
        if (resolver.Override(address, rule.name, rule.confidence, "closure-callgraph"))
          ++added;
    }
    if (!added) break;
  }
}

} // namespace

NativeSdkAutoStats AutoRegisterNativeSdk(HostRuntime& runtime,
                                           const std::filesystem::path& manifest_output) {
  std::vector<std::pair<std::uint32_t, std::string>> manifest_hooks;
  NativeSdkAutoStats stats;
  if (!EnvEnabled("GEKKOAOT_NATIVE_SDK_AUTO", true)) {
    std::cerr << "GEKKOAOT_NATIVE_SDK_AUTO_V1=0 reason=disabled\n";
    return stats;
  }

  const auto* module = runtime.Module();
  const auto ranges = module && module->code_ranges && module->num_code_ranges != 0
                          ? CodeRanges(module)
                          : CodeRanges(runtime.LoadedDolCodeRanges());
  const std::uint32_t entry_point = module ? module->entry_point : runtime.LoadedDolEntryPoint();
  if (ranges.empty()) {
    std::cerr << "GEKKOAOT_NATIVE_SDK_AUTO_V1=0 reason=no-code-ranges\n";
    return stats;
  }

  std::map<Prefix3, std::vector<std::size_t>> prefix3;
  std::map<std::uint64_t, std::vector<std::size_t>> prefix2;
  for (std::size_t i = 0; i < GekkoAOTSdk::kGroundTruthExemplars.size(); ++i) {
    const auto& exemplar = GekkoAOTSdk::kGroundTruthExemplars[i];
    const auto skeleton = GekkoAOTSdk::GroundTruthSkeleton(exemplar);
    if (skeleton.size() >= 3)
      prefix3[{skeleton[0], skeleton[1], skeleton[2]}].push_back(i);
    else if (skeleton.size() == 2)
      prefix2[Prefix2(skeleton[0], skeleton[1])].push_back(i);
  }

  // Scan direct BL targets first. Used SDK functions are overwhelmingly direct
  // call targets, and keeping this set also makes two-instruction leaf matching
  // conservative enough to avoid matching instruction pairs inside game code.
  std::set<std::uint32_t> direct_targets;
  for (const auto& range : ranges) {
    for (std::uint32_t pc = range.start; pc + 4u <= range.end; pc += 4u) {
      const auto* code = runtime.Memory().Resolve(pc, 4u);
      if (!code) continue;
      ++stats.scanned_words;
      std::uint32_t target = 0;
      if (GekkoAOTSdk::StructuralDirectCall(
              pc, GekkoAOTSdk::StructuralReadBE32(code), &target) &&
          ContainingRange(ranges, target))
        direct_targets.insert(target);
    }
  }
  stats.direct_call_targets = direct_targets.size();
  if (entry_point) direct_targets.insert(entry_point);
  for (const auto& range : ranges) direct_targets.insert(range.start);

  struct ExactMatch { std::string_view name; std::uint32_t size; };
  Resolver resolver;
  std::set<std::uint32_t> discovered;

  // v71: exact corpus bodies are only admitted at proven function entries.
  // v70 scanned every aligned instruction and could therefore assign an SDK
  // identity to a byte sequence in the middle of another function (DNDD01
  // famously produced a bogus OSCreateAlarm inside UnsetRun). Direct BL targets,
  // the DOL entry point and executable range starts are conservative function
  // entry evidence and are sufficient for SDK entrypoint lowering.
  for (const auto pc : direct_targets) {
    const auto* range = ContainingRange(ranges, pc);
    if (!range || pc + 12u > range->end) continue;
    const auto* prefix_code = runtime.Memory().Resolve(pc, 12u);
    if (!prefix_code) continue;
    const auto it = prefix3.find(CandidatePrefix3(prefix_code));
    if (it == prefix3.end()) continue;

    std::vector<ExactMatch> matches;
    for (const auto index : it->second) {
      const auto& exemplar = GekkoAOTSdk::kGroundTruthExemplars[index];
      const auto size64 = std::uint64_t{exemplar.skeleton_count} * 4u;
      if (size64 < 12u || size64 > 0x10000u || pc + size64 > range->end) continue;
      const auto size = static_cast<std::uint32_t>(size64);
      const auto* code = runtime.Memory().Resolve(pc, size);
      if (!code) continue;
      const auto shape = GekkoAOTSdk::BuildStructuralShape(code, size);
      if (GekkoAOTSdk::GroundTruthExact(shape, GekkoAOTSdk::GroundTruthShapes()[index]))
        matches.push_back({exemplar.name, size});
    }
    if (matches.empty()) continue;

    std::set<std::string_view> names;
    std::uint32_t selected_size = 0xffffffffu;
    for (const auto& match : matches) {
      names.insert(match.name);
      selected_size = std::min(selected_size, match.size);
    }
    if (selected_size == 0xffffffffu || discovered.contains(pc)) continue;
    const auto* code = runtime.Memory().Resolve(pc, selected_size);
    if (!code) continue;
    resolver.AddFunction(pc, selected_size, 0, 0, code, {});
    discovered.insert(pc);
    ++stats.exact_functions;
    if (names.size() == 1)
      resolver.Seed(pc, std::string(*names.begin()), 0.999,
                    "groundtruth-normalized-exact-entry");
    else {
      resolver.MarkAmbiguous(pc);
      ++stats.ambiguous_functions;
    }
  }

  // Some SDK entrypoints are intentionally referenced indirectly (thread exit
  // trampolines are a common example) and therefore never appear as a `bl`
  // target. Recover only exact corpus bodies that are CONTIGUOUS with an
  // already-proven SDK function. This preserves SDK library clusters without
  // returning to v70's unsafe "match at any aligned instruction" policy.
  struct AdjacentExactCandidate {
    std::uint32_t address = 0;
    std::uint32_t size = 0;
    std::string name;
  };
  std::vector<AdjacentExactCandidate> adjacent_exact;
  std::size_t adjacency_recovered = 0;
  for (const auto& range : ranges) {
    for (std::uint32_t pc = range.start; pc + 12u <= range.end; pc += 4u) {
      if (discovered.contains(pc)) continue;
      const auto* prefix_code = runtime.Memory().Resolve(pc, 12u);
      if (!prefix_code) continue;
      const auto it = prefix3.find(CandidatePrefix3(prefix_code));
      if (it == prefix3.end()) continue;
      std::set<std::string_view> names;
      std::uint32_t selected_size = 0xffffffffu;
      for (const auto index : it->second) {
        const auto& exemplar = GekkoAOTSdk::kGroundTruthExemplars[index];
        const auto size64 = std::uint64_t{exemplar.skeleton_count} * 4u;
        if (size64 < 12u || size64 > 0x10000u || pc + size64 > range.end) continue;
        const auto size = static_cast<std::uint32_t>(size64);
        const auto* code = runtime.Memory().Resolve(pc, size);
        if (!code) continue;
        const auto shape = GekkoAOTSdk::BuildStructuralShape(code, size);
        if (!GekkoAOTSdk::GroundTruthExact(
                shape, GekkoAOTSdk::GroundTruthShapes()[index]))
          continue;
        names.insert(exemplar.name);
        selected_size = std::min(selected_size, size);
      }
      if (names.size() == 1u && selected_size != 0xffffffffu)
        adjacent_exact.push_back({pc, selected_size, std::string(*names.begin())});
    }
  }

  const auto padding_only = [&](std::uint32_t begin, std::uint32_t end) {
    if (end < begin || end - begin > 16u || ((end - begin) & 3u)) return false;
    for (std::uint32_t pc = begin; pc < end; pc += 4u) {
      const auto* word = runtime.Memory().Resolve(pc, 4u);
      if (!word) return false;
      const auto raw = GekkoAOTSdk::StructuralReadBE32(word);
      if (raw != 0u && raw != 0x60000000u) return false; // zero / nop padding
    }
    return true;
  };
  for (unsigned pass = 0; pass < 16; ++pass) {
    bool changed = false;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> proven;
    proven.reserve(resolver.Functions().size());
    for (const auto& [address, fn] : resolver.Functions())
      if (resolver.Resolution(address) && !resolver.IsAmbiguous(address))
        proven.emplace_back(address, address + fn.size);

    for (const auto& candidate : adjacent_exact) {
      if (discovered.contains(candidate.address)) continue;
      bool nested = false;
      bool adjacent = false;
      const auto candidate_end = candidate.address + candidate.size;
      for (const auto& [start_pc, end_pc] : proven) {
        if (candidate.address > start_pc && candidate.address < end_pc) {
          nested = true;
          break;
        }
        if (candidate.address == end_pc || candidate_end == start_pc ||
            (candidate.address > end_pc && padding_only(end_pc, candidate.address)) ||
            (candidate_end < start_pc && padding_only(candidate_end, start_pc)))
          adjacent = true;
      }
      if (nested || !adjacent) continue;
      const auto* code = runtime.Memory().Resolve(candidate.address, candidate.size);
      if (!code) continue;
      resolver.AddFunction(candidate.address, candidate.size, 0, 0, code, {});
      resolver.Seed(candidate.address, candidate.name, 0.998,
                    "groundtruth-exact-sdk-adjacency");
      discovered.insert(candidate.address);
      ++stats.exact_functions;
      ++adjacency_recovered;
      changed = true;
    }
    if (!changed) break;
  }

  // Tiny two-instruction SDK leaves are only considered at a proven direct
  // call target (or DOL entry/range start), preventing accidental mid-function
  // matches while still recovering OSGetTick-style helpers.
  for (const auto pc : direct_targets) {
    if (discovered.contains(pc)) continue;
    const auto* range = ContainingRange(ranges, pc);
    if (!range || pc + 8u > range->end) continue;
    const auto* code = runtime.Memory().Resolve(pc, 8u);
    if (!code) continue;
    const auto a = GekkoAOTSdk::StructuralInstructionSkeleton(GekkoAOTSdk::StructuralReadBE32(code));
    const auto b = GekkoAOTSdk::StructuralInstructionSkeleton(GekkoAOTSdk::StructuralReadBE32(code + 4));
    const auto it = prefix2.find(Prefix2(a, b));
    if (it == prefix2.end()) continue;
    std::set<std::string_view> names;
    for (const auto index : it->second) {
      const auto shape = GekkoAOTSdk::BuildStructuralShape(code, 8u);
      if (GekkoAOTSdk::GroundTruthExact(shape, GekkoAOTSdk::GroundTruthShapes()[index]))
        names.insert(GekkoAOTSdk::kGroundTruthExemplars[index].name);
    }
    if (names.empty()) continue;
    resolver.AddFunction(pc, 8u, 0, 0, code, {});
    discovered.insert(pc);
    ++stats.exact_functions;
    if (names.size() == 1)
      resolver.Seed(pc, std::string(*names.begin()), 0.999, "groundtruth-tiny-exact-calltarget");
    else {
      resolver.MarkAmbiguous(pc);
      ++stats.ambiguous_functions;
    }
  }

  // Re-run the corpus graph resolver over the materialized exact functions and
  // then apply the same semantic collision repairs used by the old native SDK
  // path. No absolute game addresses are involved.
  const auto corpus = GekkoAOTSdk::ResolveGroundTruthCorpus(resolver);
  RepairSemanticOsIdentities(resolver);

  std::map<OsKind, std::vector<std::uint32_t>> os_candidates;
  std::vector<std::pair<SimpleHleKind, std::uint32_t>> simple_candidates;
  for (const auto& [address, resolution] : resolver.Resolutions()) {
    if (resolver.IsAmbiguous(address) || resolution.confidence < 0.95) continue;
    ++stats.resolved_functions;
    if (const auto kind = OsKindForName(resolution.name)) {
      os_candidates[*kind].push_back(address);
      ++stats.os_candidates;
    }
    if (const auto kind = SimpleKindForName(resolution.name))
      simple_candidates.emplace_back(*kind, address);
    if (OsKindForName(resolution.name) || SimpleKindForName(resolution.name)) {
      std::cerr << "GEKKOAOT_SDK_RESOLVE name=" << resolution.name
                << " pc=0x" << std::hex << address << std::dec
                << " confidence=" << resolution.confidence
                << " source=" << resolution.source << "\n";
    }
  }

  // v74: OSGetThreadPriority is an ABI leaf, not merely an instruction-shape
  // identity. Its SDK contract is exactly `return thread->priority`, where
  // OSThread::priority lives at ABI offset 0x2d4. A normalized two-instruction
  // shape (`lwz r3, imm(r3); blr`) is far too weak: GHLE69 contains many such
  // accessors and v71 incorrectly selected CBGetBytesAvailableForRead at
  // 0x800d4384 (which loads offset 0x110) as OSGetThreadPriority.
  //
  // First require the exact OSThread ABI leaf `lwz r3, 0x2d4(r3); blr`.
  // A unique exact ABI leaf is already strong evidence (and lets stripped DOLs
  // keep the native getter even when OSSetThreadPriority itself is unreferenced).
  // If multiple exact ABI leaves ever survive, require a unique one in the
  // canonical forward OSThread cluster after a proven OSSetThreadPriority.
  // GetThreadPriority is optional for scheduler closure, so ambiguity fails
  // closed without disabling NativeOS.
  if (auto getter = os_candidates.find(OsKind::GetThreadPriority);
      getter != os_candidates.end()) {
    constexpr std::uint32_t kGetThreadPriorityLwz = 0x806302d4u;
    constexpr std::uint32_t kBlr = 0x4e800020u;
    constexpr std::uint32_t kMaxThreadClusterDistance = 0x180u;

    const auto before = getter->second.size();
    std::vector<std::uint32_t> abi_exact;
    for (const auto get_pc : getter->second) {
      const auto* code = runtime.Memory().Resolve(get_pc, 8u);
      if (!code) continue;
      const auto first = GekkoAOTSdk::StructuralReadBE32(code);
      const auto second = GekkoAOTSdk::StructuralReadBE32(code + 4u);
      if (first == kGetThreadPriorityLwz && second == kBlr)
        abi_exact.push_back(get_pc);
    }

    std::optional<std::uint32_t> selected;
    const char* proof = "none";
    if (abi_exact.size() == 1u) {
      selected = abi_exact.front();
      proof = "unique-abi";
    } else if (abi_exact.size() > 1u) {
      std::vector<std::uint32_t> clustered;
      if (const auto setter = os_candidates.find(OsKind::SetThreadPriority);
          setter != os_candidates.end()) {
        for (const auto get_pc : abi_exact) {
          for (const auto set_pc : setter->second) {
            if (get_pc <= set_pc) continue;
            const auto distance = get_pc - set_pc;
            if (distance >= 8u && distance <= kMaxThreadClusterDistance) {
              clustered.push_back(get_pc);
              break;
            }
          }
        }
      }
      if (clustered.size() == 1u) {
        selected = clustered.front();
        proof = "abi+osthread-cluster";
      }
    }

    if (selected)
      getter->second.assign(1u, *selected);
    else
      getter->second.clear();

    std::cerr << "GEKKOAOT_SDK_LEAF_PROOF_V74=1 name=OSGetThreadPriority"
              << " candidates=" << before
              << " abi_exact=" << abi_exact.size()
              << " abi=lwz-r3-thread-priority-0x2d4+blr"
              << " cluster=OSSetThreadPriority-forward<=0x180"
              << " proof=" << proof << " selected=";
    if (selected)
      std::cerr << "0x" << std::hex << *selected << std::dec;
    else
      std::cerr << "guest-aot";
    std::cerr << "\n";
  }

  for (const auto& [kind, address] : simple_candidates) {
    runtime.RegisterSimpleHle(kind, address);
    manifest_hooks.emplace_back(address, SimpleKindName(kind));
    ++stats.simple_hooks;
  }

  const bool native_os_enabled = EnvEnabled("GEKKOAOT_NATIVE_OS", true);
  std::vector<OsKind> missing;
  if (native_os_enabled) {
    for (const auto kind : kAllOsKinds) {
      if (IsOptionalForSchedulerClosure(kind)) continue;
      const auto it = os_candidates.find(kind);
      if (it == os_candidates.end() || it->second.empty()) missing.push_back(kind);
    }
  }
  stats.missing_scheduler = missing.size();

  // The three interrupt helpers are mutation-simple and safe independently of
  // the cooperative scheduler closure. Install them whenever they are proven.
  if (native_os_enabled) {
    for (const auto kind : {OsKind::DisableInterrupts, OsKind::EnableInterrupts,
                            OsKind::RestoreInterrupts}) {
      const auto found = os_candidates.find(kind);
      if (found == os_candidates.end()) continue;
      for (const auto address : found->second) {
        runtime.RegisterOsHook(kind, address);
        manifest_hooks.emplace_back(address, GekkoAOT::NativeOS::Name(kind));
        ++stats.os_hooks;
        std::cerr << "[native-os] HOOK " << GekkoAOT::NativeOS::Name(kind)
                  << " -> 0x" << std::hex << address << std::dec
                  << " source=standalone-structural\n";
      }
    }
  }

  if (native_os_enabled && missing.empty()) {
    const auto policy = EnvText("GEKKOAOT_NATIVE_OS_ADMISSION", "hot");
    const bool full = policy == "full";
    const bool hot = full || policy == "hot" || policy == "scheduler-core+interrupts";
    for (const auto& [kind, addresses] : os_candidates) {
      if (IsInterruptHelper(kind)) continue; // already installed above
      if (!full && IsAlarmGuestByDefault(kind)) continue;
      if (!hot && IsInterruptHelper(kind)) continue;
      for (const auto address : addresses) {
        runtime.RegisterOsHook(kind, address);
        manifest_hooks.emplace_back(address, GekkoAOT::NativeOS::Name(kind));
        ++stats.os_hooks;
        std::cerr << "[native-os] HOOK " << GekkoAOT::NativeOS::Name(kind)
                  << " -> 0x" << std::hex << address << std::dec
                  << " source=standalone-structural\n";
      }
    }
    std::cerr << "GEKKOAOT_NATIVE_OS_CLOSURE_AUTO_V8=1 policy=" << policy
              << " hooks=" << stats.os_hooks << " candidates=" << stats.os_candidates
              << "\n";
    std::cerr << "GEKKOAOT_NATIVE_OS_CONTEXT_COMPAT_V70=1 direct-sdk-context=native-sdk-ppc"
              << " scheduler-context=native-sdk-ppc queue-authority=guest+host-mirror"
              << " admission=" << (full ? "full" : "hot") << "\n";
  } else if (native_os_enabled) {
    std::cerr << "GEKKOAOT_NATIVE_OS_CLOSURE_AUTO_V8=0 action=guest-aot missing=";
    for (std::size_t i = 0; i < missing.size(); ++i) {
      if (i) std::cerr << ',';
      std::cerr << GekkoAOT::NativeOS::Name(missing[i]);
    }
    std::cerr << " safe-interrupt-hooks=" << stats.os_hooks << "\n";
  }

  if (!manifest_output.empty()) {
    std::sort(manifest_hooks.begin(), manifest_hooks.end());
    manifest_hooks.erase(std::unique(manifest_hooks.begin(), manifest_hooks.end()),
                         manifest_hooks.end());
    std::filesystem::create_directories(manifest_output.parent_path());
    std::ofstream out(manifest_output, std::ios::trunc);
    if (!out) {
      std::cerr << "GEKKOAOT_NATIVE_SDK_MANIFEST_V2=0 reason=open-failed path=\""
                << manifest_output.string() << "\"\n";
    } else {
      out << "# GEKKOAOT_NATIVE_SDK_MANIFEST_V2\n";
      out << "# address name direct-token abi-class\n";
      std::size_t context_capture = 0;
      for (const auto& [address, name] : manifest_hooks) {
        const auto token = StaticDirectToken(name);
        out << "0x" << std::hex << std::setw(8) << std::setfill('0') << address
            << std::dec << ' ' << name;
        if (token != 0u)
          out << " 0x" << std::hex << std::setw(8) << std::setfill('0') << token << std::dec;
        if (const auto kind = OsKindForName(name); kind && IsContextCaptureBoundary(*kind)) {
          out << " context-capture";
          ++context_capture;
        }
        out << '\n';
      }
      std::cerr << "GEKKOAOT_NATIVE_OS_CONTEXT_ABI_V76=1 class=context-capture hooks="
                << context_capture
                << " state=nonvolatile-int+control+gqr+nonvolatile-fp\n";
      std::cerr << "GEKKOAOT_NATIVE_SDK_MANIFEST_V2=1 mode=write hooks="
                << manifest_hooks.size() << " path=\"" << manifest_output.string()
                << "\"\n";
    }
  }

  std::cerr << "GEKKOAOT_SDK_ENTRY_PROOF_V71=1 policy=direct-bl+sdk-adjacency"
            << " adjacency-recovered=" << adjacency_recovered
            << " arbitrary-aligned-hooks=0"
            << " closure=linked-core\n";
  std::cerr << "GEKKOAOT_NATIVE_OS_LINKAGE_V71=1 core=atomic"
            << " high-level=install-when-linked-and-proven"
            << " absent-sdk-entrypoints=non-blocking\n";

  std::cerr << "GEKKOAOT_NATIVE_SDK_AUTO_V1=1 scanned_words=" << stats.scanned_words
            << " call_targets=" << stats.direct_call_targets
            << " exact=" << stats.exact_functions
            << " ambiguous=" << stats.ambiguous_functions
            << " resolved=" << stats.resolved_functions
            << " os_candidates=" << stats.os_candidates
            << " os_hooks=" << stats.os_hooks
            << " simple_hooks=" << stats.simple_hooks
            << " corpus_exact=" << corpus.exact
            << " corpus_fuzzy=" << corpus.fuzzy
            << " corpus_graph=" << corpus.graph << "\n";
  return stats;
}

bool RegisterNativeSdkManifest(HostRuntime& runtime,
                               const std::filesystem::path& manifest_path) {
  std::ifstream in(manifest_path);
  if (!in) {
    std::cerr << "GEKKOAOT_NATIVE_SDK_MANIFEST_V2=0 reason=open-failed path=\""
              << manifest_path.string() << "\"\n";
    return false;
  }

  std::size_t hooks = 0;
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#') continue;
    std::istringstream row(line);
    std::string address_text, name;
    if (!(row >> address_text >> name)) return false;
    char* end = nullptr;
    const unsigned long parsed = std::strtoul(address_text.c_str(), &end, 0);
    if (!end || *end != '\0' || parsed == 0 || parsed > 0xfffffffful) return false;
    const auto address = static_cast<std::uint32_t>(parsed);
    if (const auto kind = OsKindForName(name)) {
      runtime.RegisterOsHook(*kind, address);
    } else if (const auto kind = SimpleKindForName(name)) {
      runtime.RegisterSimpleHle(*kind, address);
    } else {
      std::cerr << "GEKKOAOT_NATIVE_SDK_MANIFEST_V2=0 reason=unknown-hook name="
                << name << "\n";
      return false;
    }
    ++hooks;
  }
  std::cerr << "GEKKOAOT_NATIVE_SDK_MANIFEST_V2=1 mode=load hooks=" << hooks
            << " path=\"" << manifest_path.string() << "\"\n";
  return true;
}

} // namespace GekkoAOT::Native
