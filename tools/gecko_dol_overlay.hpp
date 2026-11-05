#pragma once
// SPDX-License-Identifier: GPL-3.0-or-later
// Apply supported Gecko writes to DOL TEXT before AOT compilation. A RAM
// write to PPC instructions cannot change LLVM code that was compiled earlier.
#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <filesystem>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
#include <sstream>
#include <limits>

namespace GekkoAOT::Build::GeckoDol {
namespace fs = std::filesystem;
struct Write { std::uint32_t address{}; std::vector<std::uint8_t> bytes; };
struct Result { std::vector<std::uint8_t> dol; unsigned instructions_patched = 0; };
inline std::uint32_t Be32(const std::vector<std::uint8_t>& b, std::size_t off) {
  if (off > b.size() || b.size() - off < 4) throw std::runtime_error("truncated DOL header");
  return (std::uint32_t(b[off]) << 24) | (std::uint32_t(b[off+1]) << 16) |
         (std::uint32_t(b[off+2]) << 8) | b[off+3];
}
inline bool HexWord(std::string_view s, std::uint32_t& out) {
  if(s.size() != 8) return false;
  const auto [end, error] = std::from_chars(s.data(),s.data()+s.size(),out,16);
  return error == std::errc{} && end == s.data()+s.size();
}
inline bool Pair(const std::string& line, std::uint32_t& a, std::uint32_t& b) {
  std::istringstream in(line); std::string x,y,extra;
  return bool(in>>x>>y) && !(in>>extra) && HexWord(x,a) && HexWord(y,b);
}
inline void AppendBE(std::vector<std::uint8_t>& out, std::uint32_t value) {
  out.push_back(static_cast<std::uint8_t>(value>>24));
  out.push_back(static_cast<std::uint8_t>(value>>16));
  out.push_back(static_cast<std::uint8_t>(value>>8));
  out.push_back(static_cast<std::uint8_t>(value));
}
inline std::vector<Write> LoadWrites(const fs::path& path) {
  std::ifstream in(path);
  if(!in) throw std::runtime_error("cannot read cheats: " + path.string());
  std::vector<Write> writes;
  std::string line;
  std::size_t number=0;
  auto fail=[&](const char* message)->void {
    throw std::runtime_error(path.string()+":"+std::to_string(number)+": "+message);
  };
  while(std::getline(in,line)) {
    ++number;
    const auto first=line.find_first_not_of(" \t\r");
    if(first==std::string::npos) continue;
    line.erase(0,first);
    if(line[0]=='#'||line[0]==';'||line[0]=='$'||line[0]=='[') continue;
    const auto comment=line.find_first_of("#;");
    if(comment!=std::string::npos) line.resize(comment);
    std::uint32_t word=0,value=0;
    if(!Pair(line,word,value)) fail("expected Gecko 8-digit hex pair");
    const std::uint32_t type=word>>24;
    Write write;
    write.address=0x80000000u|(word&0x01ffffffu);
    if(type==0x00||type==0x02||type==0x04) {
      const unsigned n=type==0x00?1u:type==0x02?2u:4u;
      for(unsigned i=n;i>0;--i) write.bytes.push_back(static_cast<std::uint8_t>(value>>((i-1)*8)));
    } else if(type==0x06) {
      if(value==0||value>(1u<<20)) fail("bad 06 payload length");
      for(std::uint32_t n=0;n<(value+7u)/8u;++n) {
        if(!std::getline(in,line)) fail("truncated 06 payload");
        ++number; std::uint32_t a=0,b=0;
        if(!Pair(line,a,b)) fail("invalid 06 payload");
        AppendBE(write.bytes,a);AppendBE(write.bytes,b);
      }
      write.bytes.resize(value);
    } else fail("unsupported Gecko type");
    const std::uint32_t physical=write.address&0x01ffffffu;
    if(physical>=0x01800000u||write.bytes.size()>0x01800000u-physical)
      fail("write outside retail MEM1");
    writes.push_back(std::move(write));
  }
  if(in.bad()) throw std::runtime_error("failed reading cheat file");
  return writes;
}
inline Result PatchText(std::vector<std::uint8_t> dol, const std::vector<Write>& writes) {
  if(dol.size()<0x100) throw std::runtime_error("truncated DOL");
  Result result{std::move(dol),0};
  struct Section { std::uint32_t address,size,offset; };
  std::vector<Section> sections;
  for(std::size_t i=0;i<7;++i) {
    const auto offset=Be32(result.dol,i*4);
    const auto address=Be32(result.dol,0x48+i*4);
    const auto size=Be32(result.dol,0x90+i*4);
    if(!size)continue;
    if(std::uint64_t(offset)+size>result.dol.size()) throw std::runtime_error("invalid DOL TEXT file bounds");
    sections.push_back({address,size,offset});
  }
  for(const auto& w:writes) {
    for(const auto& section:sections) {
      if(std::uint64_t(w.address)<section.address ||
         std::uint64_t(w.address)>=std::uint64_t(section.address)+section.size) continue;
      const auto delta=std::uint64_t(w.address)-section.address;
      if(delta+w.bytes.size()>section.size)
        throw std::runtime_error("Gecko code write crosses DOL TEXT boundary at "+std::to_string(w.address));
      // Partial instruction patches cannot be represented reliably by an AOT
      // recompilation from a DOL; refuse them instead of changing the wrong op.
      if((w.address&3u) || (w.bytes.size()&3u))
        throw std::runtime_error("unaligned Gecko patch into PPC instructions");
      auto dst=result.dol.begin()+section.offset+static_cast<std::ptrdiff_t>(delta);
      std::copy(w.bytes.begin(),w.bytes.end(),dst);
      result.instructions_patched += static_cast<unsigned>(w.bytes.size()/4u);
      break;
    }
  }
  return result;
}
} // namespace GekkoAOT::Build::GeckoDol
