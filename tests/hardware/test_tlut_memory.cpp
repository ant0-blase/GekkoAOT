// SPDX-License-Identifier: GPL-3.0-or-later
#include "gx/host/tlut_memory.h"
#include <cstdlib>
#include <iostream>
#include <vector>
#define CHECK(x) do { if (!(x)) { std::cerr << __LINE__ << ": " #x "\n"; std::exit(1); } } while(false)
int main() {
  GekkoAOT::GX::TlutMemory memory;
  // Non-SDK region index, high byte alpha and low byte intensity (IA8).
  std::vector<std::uint8_t> palette(512);
  for(unsigned i=0;i<256;++i) { palette[2*i]=i; palette[2*i+1]=255-i; }
  CHECK(memory.Load(0x180u | (16u<<10), palette));
  auto ci8=memory.Palette(0x180u,256);
  CHECK(ci8.size()==512 && ci8[510]==255 && ci8[511]==0);
  auto ci4=memory.Palette(0x180u,16);
  CHECK(ci4.size()==32 && ci4[30]==15 && ci4[31]==240);
  // Main-memory reuse must not change already uploaded TMEM contents.
  palette.assign(512,0);
  CHECK(ci8[510]==255);
  CHECK(memory.Load(0x180u | (1u<<10), std::span(palette).first(32)));
  CHECK(ci8[30]==0 && ci8[510]==255); // partial upload retains the remainder
  CHECK(memory.Revision()==2);
  CHECK(memory.Load(0,{}));
  CHECK(memory.Revision()==2);
  CHECK(!memory.Load(1u<<10,std::span(palette).first(31)));
  // Count is 11 bits: 1024 lines encode a full CI14X2 palette, not zero.
  std::vector<std::uint8_t> large(32768,0x5a);
  CHECK(memory.Load(0x200u | (1024u<<10),large));
  CHECK(memory.Palette(0x200u,16384).back()==0x5a);
  CHECK(memory.Load(1023u | (1024u<<10),large));
  CHECK(memory.Palette(1023u,16384).back()==0x5a);
  CHECK(memory.Palette(1023u,UINT32_MAX).empty());
  CHECK(GekkoAOT::GX::TlutMemory::SourceAddress(0x64a311a9u)==0x00623520u);
  std::cout << "TLUT CI4/CI8/CI14X2 lengths, IA8 bytes, partial uploads and ownership passed\n";
}
