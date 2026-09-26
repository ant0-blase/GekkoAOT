#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace GekkoAOTSdk {

// Structural SDK resolver primitives.  These deliberately do not depend on
// Dolphin's SymbolDB so the matching policy can be unit-tested independently.
// A caller supplies the discovered function boundaries, code bytes and direct
// call edges from PPCAnalyst.

inline std::uint32_t StructuralReadBE32(const std::uint8_t* p) {
  return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) |
         (std::uint32_t(p[2]) << 8) | std::uint32_t(p[3]);
}

inline bool StructuralDirectCall(std::uint32_t pc, std::uint32_t word,
                                 std::uint32_t* target) {
  if ((word >> 26) != 18u || (word & 1u) == 0u)
    return false;
  std::uint32_t displacement = word & 0x03fffffcu;
  if (displacement & 0x02000000u)
    displacement |= 0xfc000000u;
  const bool absolute = (word & 2u) != 0u;
  if (target)
    *target = absolute ? displacement : pc + displacement;
  return true;
}

inline std::uint32_t StructuralInstructionSkeleton(std::uint32_t word) {
  const std::uint32_t op = word >> 26;
  if (op == 18u)
    return word & 0xfc000003u; // branch target is relocation/build dependent
  if (op == 16u)
    return word & 0xffff0003u; // preserve BO/BI, AA and LK; discard BD

  // Normalize displacement/immediate fields while retaining operation and
  // register flow. Small constants are useful semantic evidence, so keep them.
  if (op == 14u || op == 15u || op == 24u || op == 25u ||
      (op >= 32u && op <= 47u)) {
    const auto imm = static_cast<std::int16_t>(word & 0xffffu);
    const std::uint32_t ra = (word >> 16) & 31u;
    const bool sda = ra == 2u || ra == 13u;
    if (sda || imm > 0x100 || imm < -0x100)
      return word & 0xffff0000u;
  }
  return word;
}

struct StructuralShape {
  std::uint32_t size = 0;
  std::uint32_t instruction_count = 0;
  std::uint32_t direct_calls = 0;
  std::uint32_t branches = 0;
  std::uint32_t returns = 0;
  std::int32_t stack_frame = 0;
  std::array<std::uint16_t, 64> primary_ops{};
  std::vector<std::uint32_t> skeleton;
};

inline StructuralShape BuildStructuralShape(const std::uint8_t* code,
                                            std::uint32_t size) {
  StructuralShape out;
  if (!code || size < 4u || (size & 3u) != 0u || size > 0x10000u)
    return out;
  out.size = size;
  out.instruction_count = size / 4u;
  out.skeleton.reserve(out.instruction_count);
  for (std::uint32_t i = 0; i < out.instruction_count; ++i) {
    const std::uint32_t word = StructuralReadBE32(code + i * 4u);
    const std::uint32_t op = word >> 26;
    ++out.primary_ops[op];
    out.skeleton.push_back(StructuralInstructionSkeleton(word));
    if (op == 18u && (word & 1u))
      ++out.direct_calls;
    if (op == 16u || op == 18u || op == 19u)
      ++out.branches;
    if (word == 0x4e800020u)
      ++out.returns;

    // stwu r1,-N(r1)
    if (i < 8u && op == 37u && ((word >> 21) & 31u) == 1u &&
        ((word >> 16) & 31u) == 1u) {
      const auto displacement = static_cast<std::int16_t>(word & 0xffffu);
      if (displacement < 0)
        out.stack_frame = -displacement;
    }
  }
  return out;
}

inline double StructuralRatio(std::uint32_t a, std::uint32_t b) {
  if (a == 0u && b == 0u)
    return 1.0;
  if (a == 0u || b == 0u)
    return 0.0;
  return static_cast<double>(std::min(a, b)) /
         static_cast<double>(std::max(a, b));
}

inline double StructuralHistogramSimilarity(const StructuralShape& a,
                                             const StructuralShape& b) {
  std::uint32_t common = 0;
  std::uint32_t total = 0;
  for (std::size_t i = 0; i < a.primary_ops.size(); ++i) {
    common += std::min(a.primary_ops[i], b.primary_ops[i]);
    total += std::max(a.primary_ops[i], b.primary_ops[i]);
  }
  return total ? static_cast<double>(common) / total : 0.0;
}

inline double StructuralSequenceSimilarity(const StructuralShape& a,
                                            const StructuralShape& b) {
  if (a.skeleton.empty() || b.skeleton.empty())
    return 0.0;
  const auto& small = a.skeleton.size() <= b.skeleton.size() ? a.skeleton : b.skeleton;
  const auto& large = a.skeleton.size() <= b.skeleton.size() ? b.skeleton : a.skeleton;

  // Compare at proportional positions and tolerate a two-instruction window.
  // This is intentionally conservative; it is evidence, never sole admission.
  std::size_t matches = 0;
  for (std::size_t i = 0; i < small.size(); ++i) {
    const double scaled = small.size() == 1
        ? 0.0
        : static_cast<double>(i) * static_cast<double>(large.size() - 1) /
              static_cast<double>(small.size() - 1);
    const std::size_t center = static_cast<std::size_t>(scaled + 0.5);
    const std::size_t begin = center > 2 ? center - 2 : 0;
    const std::size_t end = std::min(large.size(), center + 3);
    for (std::size_t j = begin; j < end; ++j) {
      if (small[i] == large[j]) {
        ++matches;
        break;
      }
    }
  }
  return static_cast<double>(matches) / static_cast<double>(small.size());
}

inline double StructuralSimilarity(const StructuralShape& a,
                                   const StructuralShape& b) {
  if (!a.instruction_count || !b.instruction_count)
    return 0.0;
  const double size = StructuralRatio(a.instruction_count, b.instruction_count);
  if (size < 0.72)
    return 0.0;
  const double histogram = StructuralHistogramSimilarity(a, b);
  const double sequence = StructuralSequenceSimilarity(a, b);
  const double calls = StructuralRatio(a.direct_calls + 1u, b.direct_calls + 1u);
  const double branches = StructuralRatio(a.branches + 1u, b.branches + 1u);
  const double frame = (a.stack_frame == b.stack_frame) ? 1.0 :
      (a.stack_frame == 0 || b.stack_frame == 0 ? 0.6 : 0.25);
  return 0.18 * size + 0.24 * histogram + 0.36 * sequence +
         0.10 * calls + 0.07 * branches + 0.05 * frame;
}

struct StructuralFunction {
  std::uint32_t address = 0;
  std::uint32_t size = 0;
  std::uint32_t hash = 0;
  std::uint32_t flags = 0;
  const std::uint8_t* code = nullptr;
  std::vector<std::uint32_t> calls;
  StructuralShape shape;
};

struct StructuralResolution {
  std::string name;
  double confidence = 0.0;
  std::string source;
};

class StructuralResolver {
public:
  void AddFunction(std::uint32_t address, std::uint32_t size,
                   std::uint32_t hash, std::uint32_t flags,
                   const std::uint8_t* code,
                   std::vector<std::uint32_t> calls) {
    if (!address || !size || !code || (address & 3u) || (size & 3u))
      return;
    StructuralFunction fn;
    fn.address = address;
    fn.size = size;
    fn.hash = hash;
    fn.flags = flags;
    fn.code = code;
    fn.calls = std::move(calls);
    // Do not depend on PPCAnalyst/SymbolDB having populated callers/callees.
    // Stripped retail DOLs frequently have valid function boundaries but an
    // incomplete calls vector. Decode every direct PPC `bl` from the bytes and
    // merge it with the supplied graph so semantic matching works universally.
    for (std::uint32_t offset = 0; offset < size; offset += 4u) {
      std::uint32_t target = 0;
      if (StructuralDirectCall(address + offset, StructuralReadBE32(code + offset),
                               &target))
        fn.calls.push_back(target);
    }
    std::sort(fn.calls.begin(), fn.calls.end());
    fn.calls.erase(std::unique(fn.calls.begin(), fn.calls.end()), fn.calls.end());
    fn.shape = BuildStructuralShape(code, size);
    if (!fn.shape.instruction_count)
      return;
    functions_[address] = std::move(fn);
  }

  const StructuralFunction* Function(std::uint32_t address) const {
    const auto it = functions_.find(address);
    return it == functions_.end() ? nullptr : &it->second;
  }

  const StructuralResolution* Resolution(std::uint32_t address) const {
    const auto it = resolved_.find(address);
    return it == resolved_.end() ? nullptr : &it->second;
  }

  bool Seed(std::uint32_t address, std::string name, double confidence,
            std::string source) {
    if (!Function(address) || name.empty())
      return false;
    confidence = std::clamp(confidence, 0.0, 1.0);
    const auto found = resolved_.find(address);
    if (found != resolved_.end()) {
      if (found->second.name == name) {
        if (confidence > found->second.confidence) {
          found->second.confidence = confidence;
          found->second.source = std::move(source);
        }
        return false;
      }
      // Never resolve an address to two SDK identities from similarly strong
      // evidence. The conflict remains guest code instead of guessing.
      if (std::abs(found->second.confidence - confidence) < 0.08) {
        ambiguous_.insert(address);
        return false;
      }
      if (found->second.confidence > confidence)
        return false;
    }
    resolved_[address] = {std::move(name), confidence, std::move(source)};
    ambiguous_.erase(address);
    return true;
  }

  // Strong semantic proofs (for example the +/-1 scheduler counter update)
  // are allowed to repair a weaker/colliding DSY seed. This is deliberately
  // separate from Seed(): ordinary matchers still cannot overwrite identities.
  bool Override(std::uint32_t address, std::string name, double confidence,
                std::string source) {
    if (!Function(address) || name.empty())
      return false;
    confidence = std::clamp(confidence, 0.0, 1.0);
    const auto found = resolved_.find(address);
    const bool changed = found == resolved_.end() || found->second.name != name ||
                         confidence > found->second.confidence ||
                         found->second.source != source;
    resolved_[address] = {std::move(name), confidence, std::move(source)};
    ambiguous_.erase(address);
    return changed;
  }

  bool IsAmbiguous(std::uint32_t address) const {
    return ambiguous_.contains(address);
  }

  void MarkAmbiguous(std::uint32_t address) {
    if (!Function(address))
      return;
    resolved_.erase(address);
    ambiguous_.insert(address);
  }

  std::vector<std::uint32_t> Find(std::string_view name,
                                  double minimum = 0.0) const {
    std::vector<std::uint32_t> out;
    for (const auto& [address, resolution] : resolved_) {
      if (!ambiguous_.contains(address) && resolution.name == name &&
          resolution.confidence >= minimum)
        out.push_back(address);
    }
    return out;
  }

  bool Calls(std::uint32_t address, std::string_view identity,
             double minimum = 0.90) const {
    const auto* fn = Function(address);
    if (!fn)
      return false;
    for (const auto target : fn->calls) {
      const auto* resolved = Resolution(target);
      if (resolved && !IsAmbiguous(target) && resolved->name == identity &&
          resolved->confidence >= minimum)
        return true;
    }
    return false;
  }

  unsigned CountCalls(std::uint32_t address, std::string_view identity,
                      double minimum = 0.90) const {
    const auto* fn = Function(address);
    if (!fn)
      return 0;
    unsigned count = 0;
    for (const auto target : fn->calls) {
      const auto* resolved = Resolution(target);
      if (resolved && !IsAmbiguous(target) && resolved->name == identity &&
          resolved->confidence >= minimum)
        ++count;
    }
    return count;
  }

  // Propagate identities to duplicate/library variants by normalized PPC
  // structure. Raw address, raw branch displacement and large relocatable
  // immediates do not participate in this score.
  std::size_t ResolveStructuralAliases(double threshold = 0.965,
                                       double margin = 0.035) {
    std::size_t added = 0;
    std::vector<std::pair<std::uint32_t, StructuralResolution>> pending;
    for (const auto& [address, fn] : functions_) {
      if (resolved_.contains(address) || ambiguous_.contains(address))
        continue;
      double best = 0.0;
      double second = 0.0;
      std::string best_name;
      for (const auto& [seed_address, seed] : resolved_) {
        if (seed.confidence < 0.98 || ambiguous_.contains(seed_address))
          continue;
        // Some SDK leaf functions are intentionally byte-identical despite
        // different semantics/names (for example OSInitThreadQueue and
        // OSCreateAlarm both clear two words). Never propagate those identities
        // from body shape alone; require call-graph/context evidence instead.
        if (seed.name == "OSInitThreadQueue" || seed.name == "OSCreateAlarm")
          continue;
        const auto* exemplar = Function(seed_address);
        if (!exemplar)
          continue;
        const double score = StructuralSimilarity(fn.shape, exemplar->shape);
        if (score > best) {
          second = best;
          best = score;
          best_name = seed.name;
        } else if (score > second && seed.name != best_name) {
          second = score;
        }
      }
      if (!best_name.empty() && best >= threshold && best - second >= margin)
        pending.push_back({address, {best_name, std::min(0.97, best),
                                    "normalized-ppc-shape"}});
    }
    for (auto& [address, resolution] : pending)
      added += Seed(address, std::move(resolution.name), resolution.confidence,
                    std::move(resolution.source)) ? 1u : 0u;
    return added;
  }

  const std::map<std::uint32_t, StructuralFunction>& Functions() const {
    return functions_;
  }
  const std::map<std::uint32_t, StructuralResolution>& Resolutions() const {
    return resolved_;
  }

private:
  std::map<std::uint32_t, StructuralFunction> functions_;
  std::map<std::uint32_t, StructuralResolution> resolved_;
  std::set<std::uint32_t> ambiguous_;
};

struct CardSyncAdapterProof {
  std::uint32_t callback = 0;
  std::uint32_t wait = 0;
  double confidence = 0.0;
};

inline std::optional<std::uint32_t> MaterializedAddressForRegister(
    const std::uint8_t* code, std::size_t before, unsigned reg) {
  if (!code || reg > 31u || before == 0)
    return std::nullopt;
  const std::size_t begin = before > 12 ? before - 12 : 0;
  for (std::size_t low_i = before; low_i-- > begin;) {
    const std::uint32_t low = StructuralReadBE32(code + low_i * 4u);
    const std::uint32_t op = low >> 26;
    const unsigned rd = (low >> 21) & 31u;
    const unsigned ra = (low >> 16) & 31u;
    if (rd != reg || ra != reg || (op != 14u && op != 24u))
      continue;
    for (std::size_t high_i = low_i; high_i-- > begin;) {
      const std::uint32_t high = StructuralReadBE32(code + high_i * 4u);
      if ((high >> 26) != 15u || ((high >> 21) & 31u) != reg ||
          ((high >> 16) & 31u) != 0u)
        continue;
      const std::uint32_t hi = (high & 0xffffu) << 16;
      const std::uint32_t lo = low & 0xffffu;
      const std::uint32_t value = op == 14u
          ? hi + static_cast<std::uint32_t>(
                     static_cast<std::int32_t>(static_cast<std::int16_t>(lo)))
          : hi | lo;
      return value;
    }
  }
  return std::nullopt;
}

// Recognize SDK synchronous adapters by dataflow and call topology, not by
// placement in the object file. This accepts compiler-layout variation in
// CARDRead/CARDWrite while still requiring callback materialization, a direct
// call to the already-proven Async API, an error check, a later sync/wait call
// and a normal blr return.
inline std::optional<CardSyncAdapterProof> DecodeCardSyncAdapter(
    const std::uint8_t* code, std::size_t size, std::uint32_t pc,
    std::uint32_t async_target, unsigned synchronous_argc) {
  if (!code || (size & 3u) || size < 24u || size > 0x180u ||
      (pc & 3u) || (async_target & 3u) || synchronous_argc < 1u ||
      synchronous_argc > 5u)
    return std::nullopt;
  const std::size_t words = size / 4u;
  const unsigned callback_reg = 3u + synchronous_argc;
  std::optional<std::size_t> async_index;
  std::vector<std::pair<std::size_t, std::uint32_t>> calls;
  bool has_return = false;
  for (std::size_t i = 0; i < words; ++i) {
    const std::uint32_t word = StructuralReadBE32(code + i * 4u);
    if (word == 0x4e800020u)
      has_return = true;
    std::uint32_t target = 0;
    if (!StructuralDirectCall(pc + static_cast<std::uint32_t>(i * 4u), word, &target))
      continue;
    calls.push_back({i, target});
    if (target == async_target) {
      if (async_index)
        return std::nullopt;
      async_index = i;
    }
  }
  if (!async_index || !has_return || calls.size() < 2u || calls.size() > 5u)
    return std::nullopt;

  const auto callback = MaterializedAddressForRegister(
      code, *async_index, callback_reg);
  if (!callback || (*callback & 3u) != 0u || *callback < 0x80000000u ||
      *callback >= 0x94000000u)
    return std::nullopt;

  std::uint32_t wait = 0;
  std::size_t wait_index = 0;
  for (const auto& [index, target] : calls) {
    if (index > *async_index && target != async_target) {
      wait = target;
      wait_index = index;
      break;
    }
  }
  if (!wait || (wait & 3u) || wait_index <= *async_index)
    return std::nullopt;

  bool checked_result = false;
  for (std::size_t i = *async_index + 1;
       i < std::min(words, *async_index + 8u); ++i) {
    const std::uint32_t word = StructuralReadBE32(code + i * 4u);
    // cmpwi crN,r3,0 / cmplwi crN,r3,0
    const auto op = word >> 26;
    if ((op == 10u || op == 11u) && ((word >> 16) & 31u) == 3u &&
        (word & 0xffffu) == 0u) {
      checked_result = true;
      break;
    }
  }
  if (!checked_result)
    return std::nullopt;

  // Require the wait call to occur before a final return and reject wrappers
  // that branch into another unrelated call chain after the wait.
  bool return_after_wait = false;
  for (std::size_t i = wait_index + 1; i < words; ++i) {
    if (StructuralReadBE32(code + i * 4u) == 0x4e800020u) {
      return_after_wait = true;
      break;
    }
  }
  if (!return_after_wait)
    return std::nullopt;

  double confidence = 0.94;
  if (calls.size() == 2u)
    confidence += 0.025;
  if (size <= 0x80u)
    confidence += 0.015;
  if (StructuralReadBE32(code) == 0x7c0802a6u)
    confidence += 0.01;
  return CardSyncAdapterProof{*callback, wait, std::min(0.99, confidence)};
}


// OSDisableScheduler/OSEnableScheduler are small wrappers around interrupt
// control and a global reschedule counter. Their key semantic distinction is
// the +/-1 update, which is stable even when addresses/register allocation move.
inline int DecodeOsSchedulerDelta(const std::uint8_t* code, std::size_t size) {
  if (!code || (size & 3u) || size < 24u || size > 0x100u)
    return 0;
  int found = 0;
  for (std::size_t i = 0; i < size / 4u; ++i) {
    const std::uint32_t word = StructuralReadBE32(code + i * 4u);
    if ((word >> 26) != 14u)
      continue;
    const auto imm = static_cast<std::int16_t>(word & 0xffffu);
    if (imm != 1 && imm != -1)
      continue;
    if (found && found != imm)
      return 0;
    found = imm;
  }
  return found;
}

// OSLoadContext is handwritten PPC and terminates in rfi after restoring a
// large register set from OSContext. This semantic fingerprint avoids relying
// on its position immediately after OSSaveContext.
inline bool DecodeOsLoadContextShape(const std::uint8_t* code,
                                     std::size_t size) {
  if (!code || (size & 3u) || size < 0x90u || size > 0x180u)
    return false;
  bool rfi = false;
  bool lmw_from_context = false;
  unsigned context_loads = 0;
  for (std::size_t i = 0; i < size / 4u; ++i) {
    const std::uint32_t word = StructuralReadBE32(code + i * 4u);
    const std::uint32_t op = word >> 26;
    const unsigned ra = (word >> 16) & 31u;
    if (word == 0x4c000064u)
      rfi = true;
    if (op == 46u && ra == 3u)
      lmw_from_context = true;
    if (ra == 3u && ((op >= 32u && op <= 35u) ||
                     (op >= 40u && op <= 43u)))
      ++context_loads;
  }
  return rfi && lmw_from_context && context_loads >= 8u;
}

struct CallGraphRule {
  const char* name = nullptr;
  std::uint32_t min_size = 0;
  std::uint32_t max_size = 0xffffffffu;
  std::vector<const char*> required;
  std::vector<const char*> any_of;
  unsigned min_direct_calls = 0;
  unsigned max_direct_calls = 0xffffffffu;
  double confidence = 0.94;
};

inline bool MatchCallGraphRule(const StructuralResolver& resolver,
                               const StructuralFunction& fn,
                               const CallGraphRule& rule) {
  if (fn.size < rule.min_size || fn.size > rule.max_size ||
      fn.shape.direct_calls < rule.min_direct_calls ||
      fn.shape.direct_calls > rule.max_direct_calls)
    return false;
  for (const char* required : rule.required) {
    if (!resolver.Calls(fn.address, required))
      return false;
  }
  if (!rule.any_of.empty()) {
    bool matched = false;
    for (const char* optional : rule.any_of)
      matched |= resolver.Calls(fn.address, optional);
    if (!matched)
      return false;
  }
  return true;
}

} // namespace GekkoAOTSdk
