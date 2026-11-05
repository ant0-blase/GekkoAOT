// SPDX-License-Identifier: GPL-3.0-or-later
// Maintainer check, not part of the build or ctest: proves that the
// fingerprinted SDK reference table (runtime/sdk/sdk_groundtruth_v1.h) makes
// the same resolver decisions as the former skeleton-word table.
//
// The former table is not in the repository. Build this file twice:
//
//   old: c++ -std=c++20 -O2 -DGT_OLD -I<dir with the former structural_signature.h
//        and sdk_groundtruth_v1.h> check_groundtruth_fingerprint.cpp -o gt-old
//   new: c++ -std=c++20 -O2 -Iruntime/sdk check_groundtruth_fingerprint.cpp -o gt-new
//
//   ./gt-old candidates.txt old-decisions.txt old-scores.txt
//   ./gt-new candidates.txt new-decisions.txt new-scores.txt
//   cmp old-decisions.txt new-decisions.txt
//
// The old build writes every reference body as a candidate (candidates.txt);
// the new build reads them back, so both evaluate the same candidates. Each
// reference body is matched against all other bodies with the rules of
// ResolveGroundTruthCorpus: exact pass, fuzzy pass (best identity, margin,
// support, admission) and the score thresholds the passes use.
#include "sdk_groundtruth_v1.h"

#include <cstdio>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using namespace GekkoAOTSdk;

namespace {

struct Candidate {
  std::size_t index = 0;
  std::int32_t stack_frame = 0;
  std::vector<std::uint32_t> words;
};

StructuralShape CandidateShape(const Candidate& c) {
  std::vector<std::uint8_t> bytes;
  for (const auto w : c.words) {
    bytes.push_back(static_cast<std::uint8_t>(w >> 24));
    bytes.push_back(static_cast<std::uint8_t>(w >> 16));
    bytes.push_back(static_cast<std::uint8_t>(w >> 8));
    bytes.push_back(static_cast<std::uint8_t>(w));
  }
  auto shape = BuildStructuralShape(bytes.data(), static_cast<std::uint32_t>(bytes.size()));
  // A skeleton loses large stwu displacements; use the recorded frame.
  shape.stack_frame = c.stack_frame;
  return shape;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 4) {
    std::cerr << "usage: " << argv[0] << " candidates.txt decisions.txt scores.txt\n";
    return 2;
  }
  std::vector<Candidate> candidates;
#if defined(GT_OLD)
  {
    std::ofstream out(argv[1]);
    for (std::size_t i = 0; i < kGroundTruthExemplars.size(); ++i) {
      const auto& e = kGroundTruthExemplars[i];
      Candidate c{i, e.stack_frame, {}};
      for (const auto w : GroundTruthSkeleton(e)) c.words.push_back(w);
      out << i << ' ' << c.stack_frame << ' ' << c.words.size();
      for (const auto w : c.words) out << ' ' << w;
      out << '\n';
      candidates.push_back(std::move(c));
    }
  }
#else
  {
    std::ifstream in(argv[1]);
    std::string line;
    while (std::getline(in, line)) {
      std::istringstream row(line);
      Candidate c;
      std::size_t n = 0;
      row >> c.index >> c.stack_frame >> n;
      c.words.resize(n);
      for (auto& w : c.words) row >> w;
      candidates.push_back(std::move(c));
    }
    if (candidates.size() != kGroundTruthExemplars.size()) {
      std::cerr << "candidate count mismatch\n";
      return 1;
    }
  }
#endif
  const auto& shapes = GroundTruthShapes();
  std::ofstream decisions(argv[2]);
  std::ofstream scores(argv[3]);
  scores << std::setprecision(17);
  std::size_t admissions = 0, pairs = 0;
  for (const auto& c : candidates) {
    const auto shape = CandidateShape(c);
    // Exact pass.
    std::set<std::string_view> exact;
    for (std::size_t j = 0; j < shapes.size(); ++j)
      if (j != c.index && GroundTruthExact(shape, shapes[j])) exact.insert(kGroundTruthExemplars[j].name);
    decisions << c.index << " exact";
    for (const auto n : exact) decisions << ' ' << n;
    decisions << '\n';
    // Fuzzy pass and per-pair thresholds.
    struct Support { double best = 0.0; std::set<std::uint8_t> sources; };
    std::map<std::string_view, Support> identity;
    for (std::size_t j = 0; j < shapes.size(); ++j) {
      if (j == c.index) continue;
      const auto& e = kGroundTruthExemplars[j];
      const double score = GroundTruthSimilarity(shape, shapes[j]);
      ++pairs;
      scores << c.index << ' ' << j << ' ' << score << '\n';
      decisions << c.index << ' ' << j << " t"
                << (score >= 0.24) << (score >= 0.55) << (score >= 0.88)
                << (score >= 0.96) << (score >= 0.985) << '\n';
      if (!e.critical) continue;
      auto& a = identity[e.name];
      a.best = std::max(a.best, score);
      if (score >= 0.88) a.sources.insert(e.source_variant);
    }
    std::string_view best_name;
    double best = 0.0, second = 0.0;
    unsigned support = 0;
    for (const auto& [name, a] : identity) {
      if (a.best > best) {
        second = best;
        best = a.best;
        best_name = name;
        support = static_cast<unsigned>(a.sources.size());
      } else if (a.best > second) {
        second = a.best;
      }
    }
    const double margin = best - second;
    const bool eligible = shape.instruction_count >= 8u;
    const bool admitted = eligible && !best_name.empty() && best >= 0.96 && margin >= 0.045 &&
                          (support >= 2 || best >= 0.985);
    admissions += admitted;
    decisions << c.index << " fuzzy best=" << best_name << " support=" << support
              << " admitted=" << admitted << " t" << (best >= 0.88) << (best >= 0.96)
              << (best >= 0.985) << " margin_ok=" << (margin >= 0.045) << '\n';
  }
  std::cout << candidates.size() << " candidates, " << pairs << " pairs, " << admissions
            << " admissions\n";
  return 0;
}
