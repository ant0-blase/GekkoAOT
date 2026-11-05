// SPDX-License-Identifier: GPL-3.0-or-later
#include "../../tools/gecko_dol_overlay.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace gd = GekkoAOT::Build::GeckoDol;
namespace fs = std::filesystem;
static void require(bool ok, const char* description) {
  if (!ok) throw std::runtime_error(description);
}
static void writeBE(std::vector<std::uint8_t>& b, std::size_t pos, std::uint32_t value) {
  b[pos] = value >> 24; b[pos+1] = value >> 16; b[pos+2] = value >> 8; b[pos+3] = value;
}
int main() {
  try {
    std::vector<std::uint8_t> dol(0x140, 0);
    // Text section: game VA 802B30E0 .. 802B311F, file offset 0x100.
    writeBE(dol,0x00,0x100); writeBE(dol,0x48,0x802B30E0);
    writeBE(dol,0x90,0x40);
    writeBE(dol,0x110,0x38000000);
    const std::vector<gd::Write> hacks{{0x802B30F0,{0x38,0x00,0x00,0x01}}};
    const auto patched = gd::PatchText(dol,hacks);
    require(patched.instructions_patched == 1, "did not identify code patch");
    require(gd::Be32(patched.dol,0x110) == 0x38000001u, "did not modify PPC instruction");
    require(gd::Be32(dol,0x110) == 0x38000000u, "modified original DOL");
    const auto data = gd::PatchText(dol,{{0x80001000,{0xde,0xad,0xbe,0xef}}});
    require(data.instructions_patched == 0 && data.dol == dol, "patched non-code write");
    bool rejected = false;
    try { (void)gd::PatchText(dol,{{0x802B30FF,{0x00,0x00,0x00,0x01}}}); }
    catch(const std::runtime_error&) { rejected=true; }
    require(rejected, "accepted crossing TEXT boundary/misaligned PPC instruction");
    const auto temp=fs::temp_directory_path()/"gekkoaot-gecko-test-codes.txt";
    {
      std::ofstream file(temp,std::ios::trunc);
      file<<"$HPCOS\n042B30F0 38000001\n02000100 0000ABCD\n";
    }
    const auto writes=gd::LoadWrites(temp);
    fs::remove(temp);
    require(writes.size()==2 && writes[0].address==0x802B30F0u &&
            writes[0].bytes==hacks[0].bytes, "failed to parse HPCOS cheat");
    const auto parsed = gd::PatchText(dol,writes);
    require(parsed.instructions_patched==1, "lost PPC patch during parsing");
    std::cout << "PASS Gecko AOT DOL overlay and data-write separation\n";
    return 0;
  } catch(const std::exception& e) {
    std::cerr << "FAIL: " << e.what() << '\n';
    return 1;
  }
}
