// SPDX-License-Identifier: GPL-3.0-or-later
// GekkoAOT native replacement for the historical Python metadata generator.
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;
struct Range { std::uint32_t start{}, end{}; };

static std::string ReadText(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) throw std::runtime_error("cannot open " + path.string());
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}
static std::vector<std::uint8_t> ReadBytes(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) throw std::runtime_error("cannot open " + path.string());
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}
static std::uint32_t Be32(const std::vector<std::uint8_t>& b, std::size_t o) {
  if (o + 4 > b.size()) throw std::runtime_error("truncated DOL header");
  return (std::uint32_t(b[o]) << 24) | (std::uint32_t(b[o+1]) << 16) |
         (std::uint32_t(b[o+2]) << 8) | std::uint32_t(b[o+3]);
}
static std::uint64_t Fnv1a(const std::uint8_t* data, std::size_t size) {
  std::uint64_t h = 0xcbf29ce484222325ull;
  for (std::size_t i=0;i<size;++i) h = (h ^ data[i]) * 0x100000001b3ull;
  return h;
}
static std::uint32_t Hex32(const std::string& text) {
  return static_cast<std::uint32_t>(std::stoull(text, nullptr, 16));
}

static std::vector<std::uint32_t> ParseHexArray(const std::string& text, const std::string& symbol) {
  const auto name = text.find(symbol);
  if (name == std::string::npos) return {};
  const auto open = text.find('{', name);
  if (open == std::string::npos) return {};
  const auto close = text.find("};", open + 1);
  if (close == std::string::npos) return {};

  std::vector<std::uint32_t> values;
  const std::string_view body{text.data() + open + 1, close - open - 1};
  const std::regex value(R"(0x([0-9A-Fa-f]+)u)");
  const std::string body_text(body);
  for (std::sregex_iterator i(body_text.begin(), body_text.end(), value), e; i != e; ++i)
    values.push_back(Hex32((*i)[1].str()));
  return values;
}

int main(int argc, char** argv) {
  if (argc != 5) {
    std::cerr << "usage: gekkoaot-module-meta generated.h generated_smc.txt main.dol output.inc\n";
    return 2;
  }
  try {
    const std::string header = ReadText(argv[1]);
    const auto dol = ReadBytes(argv[3]);
    std::set<std::pair<std::uint32_t,std::uint32_t>> code_set;

    const std::regex guard(R"(address\s*>=\s*(0x[0-9A-Fa-f]+)u\s*&&\s*address\s*<\s*(0x[0-9A-Fa-f]+)u)");
    for (std::sregex_iterator i(header.begin(), header.end(), guard), e; i != e; ++i)
      code_set.emplace(Hex32((*i)[1].str()), Hex32((*i)[2].str()));

    const std::regex compact(R"(u32\s+offset\s*=\s*address\s*-\s*(0x[0-9A-Fa-f]+)u\s*;\s*if\s*\(\s*offset\s*<\s*(0x[0-9A-Fa-f]+)u)");
    for (std::sregex_iterator i(header.begin(), header.end(), compact), e; i != e; ++i) {
      const auto base = Hex32((*i)[1].str());
      code_set.emplace(base, base + Hex32((*i)[2].str()));
    }

    // DolRecomp's indexed dispatcher no longer emits one address guard per
    // code range. Instead it emits paired run-start/run-end tables. Parse
    // those tables as the authoritative code coverage when the legacy
    // linear/compact patterns are absent.
    if (code_set.empty()) {
      const auto starts = ParseHexArray(header, "dolrecomp_run_start");
      const auto ends = ParseHexArray(header, "dolrecomp_run_end");
      if (!starts.empty() || !ends.empty()) {
        if (starts.size() != ends.size())
          throw std::runtime_error("mismatched DolRecomp indexed run tables");
        for (std::size_t i = 0; i < starts.size(); ++i) {
          if (starts[i] >= ends[i])
            throw std::runtime_error("invalid DolRecomp indexed run range");
          code_set.emplace(starts[i], ends[i]);
        }
      }
    }
    if (code_set.empty()) throw std::runtime_error("no DolRecomp code coverage ranges found");

    std::vector<Range> code;
    for (auto [a,b] : code_set) code.push_back({a,b});

    std::vector<std::uint32_t> funcs;
    const std::regex fn(R"(void\s+func_([0-9A-Fa-f]{8})\s*\(\s*CPUState\s*\*\s*ctx\s*\)\s*;)");
    for (std::sregex_iterator i(header.begin(), header.end(), fn), e; i != e; ++i)
      funcs.push_back(Hex32((*i)[1].str()));
    std::sort(funcs.begin(), funcs.end());
    funcs.erase(std::unique(funcs.begin(), funcs.end()), funcs.end());
    if (funcs.empty()) throw std::runtime_error("no func_XXXXXXXX declarations found in generated.h");

    std::vector<Range> chunks;
    for (std::size_t i=0;i<funcs.size();++i) {
      const auto a = funcs[i];
      auto it = std::find_if(code.begin(), code.end(), [&](const Range& r){ return a >= r.start && a < r.end; });
      if (it == code.end()) throw std::runtime_error("generated function outside code coverage");
      auto b = it->end;
      if (i+1 < funcs.size() && funcs[i+1] >= it->start && funcs[i+1] < it->end) b = funcs[i+1];
      chunks.push_back({a,b});
    }

    std::vector<Range> smc;
    if (fs::exists(argv[2])) {
      std::istringstream lines(ReadText(argv[2]));
      std::string line;
      const std::regex smc_re(R"(^\s*(0x[0-9A-Fa-f]+)-(0x[0-9A-Fa-f]+))");
      while (std::getline(lines,line)) {
        std::smatch m;
        if (std::regex_search(line,m,smc_re)) smc.push_back({Hex32(m[1].str()), Hex32(m[2].str())+4u});
      }
    }
    std::sort(smc.begin(), smc.end(), [](auto a, auto b){ return a.start < b.start; });

    struct DolSection { std::uint32_t addr{}, size{}, off{}; };
    std::vector<DolSection> sections;
    for (unsigned i=0;i<18;++i) {
      const auto off=Be32(dol,0x00+i*4), addr=Be32(dol,0x48+i*4), size=Be32(dol,0x90+i*4);
      if (off && addr && size) sections.push_back({addr,size,off});
    }
    auto hash_range = [&](Range r) {
      for (const auto& s : sections) {
        const std::uint64_t end = std::uint64_t(s.addr)+s.size;
        if (r.start >= s.addr && r.end <= end) {
          const std::size_t off = s.off + (r.start-s.addr), size = r.end-r.start;
          if (off+size > dol.size()) throw std::runtime_error("DOL section is truncated");
          return Fnv1a(dol.data()+off,size);
        }
      }
      throw std::runtime_error("chunk is not contained in one DOL section");
    };

    std::ofstream out(argv[4], std::ios::binary|std::ios::trunc);
    if (!out) throw std::runtime_error("cannot create output metadata include");
    out << "// Generated by gekkoaot-module-meta v" << GEKKOAOT_VERSION << " - do not edit.\n";
    out << "static const GekkoAOTRange s_code_ranges[] = {\n";
    for (auto r:code) out << "  {0x"<<std::hex<<std::uppercase<<std::setw(8)<<std::setfill('0')<<r.start<<"u, 0x"<<std::setw(8)<<r.end<<"u},\n";
    out << "};\n#define GEKKOAOT_CODE_RANGE_COUNT " << std::dec << code.size() << "u\n";
    out << "static const GekkoAOTRange s_smc_ranges[] = {\n";
    if (smc.empty()) out << "  {0u,0u},\n";
    else for (auto r:smc) out << "  {0x"<<std::hex<<std::uppercase<<std::setw(8)<<std::setfill('0')<<r.start<<"u, 0x"<<std::setw(8)<<r.end<<"u},\n";
    out << "};\n#define GEKKOAOT_SMC_RANGE_COUNT " << std::dec << smc.size() << "u\n";
    out << "static const GekkoAOTRange s_chunk_ranges[] = {\n";
    for (auto r:chunks) out << "  {0x"<<std::hex<<std::uppercase<<std::setw(8)<<std::setfill('0')<<r.start<<"u, 0x"<<std::setw(8)<<r.end<<"u},\n";
    out << "};\n#define GEKKOAOT_CHUNK_RANGE_COUNT " << std::dec << chunks.size() << "u\n";
    out << "static const uint64_t s_chunk_hashes[] = {\n";
    for (auto r:chunks) out << "  0x"<<std::hex<<std::uppercase<<std::setw(16)<<std::setfill('0')<<hash_range(r)<<"ull,\n";
    out << "};\n";
    std::cout << "GEKKOAOT_MODULE_META=1 code_ranges="<<code.size()<<" smc_ranges="<<smc.size()<<" chunks="<<chunks.size()<<"\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "gekkoaot-module-meta: " << e.what() << '\n';
    return 1;
  }
}
