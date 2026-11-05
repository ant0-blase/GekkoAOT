// SPDX-License-Identifier: GPL-3.0-or-later
#include "../../runtime/sdk/gx_pixel_service.h"
#include "sdk_fixture_words.h"
#include "../../runtime/gx/native_command_stream.h"
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>
#define CHECK(x) do{if(!(x)){std::fprintf(stderr,"%d: %s\n",__LINE__,#x);std::exit(1);}}while(false)
using namespace GekkoAOTSdk;
static unsigned Mask(unsigned mb,unsigned me){unsigned m=0;for(unsigned bit=mb;;bit=(bit+1)&31){m|=1u<<(31-bit);if(bit==me)return m;}}
// Independent small PPC oracle for the exact call-free bodies. It observes
// every guest cache store and FIFO byte as well as all 32 final GPRs.
static unsigned Reference(GxPixelFamily family,std::array<unsigned,32>& r,unsigned cached){
  constexpr unsigned context=0x80010000,sda=0x80300000;
  unsigned cache=cached;std::vector<unsigned char> fifo;
  const auto load=[&](unsigned address){if(address==sda-0x7468)return context;CHECK(address==context+0x1d0 || address==context+0x1d8);return cache;};
  for(auto w:GxPixelFixture(family)){
    const unsigned op=w>>26,rt=w>>21&31,ra=w>>16&31,rb=w>>11&31;
    const auto ea=[&](){return (ra?r[ra]:0)+unsigned(std::int16_t(w));};
    if(w==0x4e800020)break;
    if(op==14 || op==15){r[rt]=(ra?r[ra]:0)+(op==15?unsigned(std::int16_t(w))<<16:unsigned(std::int16_t(w)));continue;}
    if(op==24 || op==25){r[ra]=r[rt]|((w&65535)<<(op==25?16:0));continue;}
    if(op==20 || op==21){const auto mask=Mask(w>>6&31,w>>1&31),rotated=std::rotl(r[rt],int(rb));r[ra]=(op==20?r[ra]&~mask:0)|(rotated&mask);continue;}
    if(op==31){CHECK((w>>1&1023)==444);r[ra]=r[rt]|r[rb];continue;}
    if(op==32 || op==33){const auto address=ea();r[rt]=load(address);if(op==33)r[ra]=address;continue;}
    const auto address=ea();
    if(address==0xcc008000){CHECK(op==36 || op==38);if(op==36)for(int s=24;s>=0;s-=8)fifo.push_back(r[rt]>>s);else fifo.push_back(r[rt]);continue;}
    if(op==44){CHECK(address==context+2 && r[rt]==0);continue;}
    CHECK(op==36 && (address==context+0x1d0 || address==context+0x1d8));cache=r[rt];
  }
  CHECK(fifo.size()==5 && fifo[0]==0x61);
  return unsigned(fifo[1])<<24|unsigned(fifo[2])<<16|unsigned(fifo[3])<<8|fifo[4];
}
int main(){
  std::mt19937 random(0x475850);
  for(unsigned kind=0;kind<4;++kind){
    const auto family=GxPixelFamily(kind);std::vector<std::uint8_t> code;
    for(auto w:GxPixelFixture(family))for(int s=24;s>=0;s-=8)code.push_back(w>>s);
    CHECK(ProveGxPixelLeaf(code,family)==std::int16_t(-0x7468));
    // Every non-relocation byte participates in the complete-body proof.
    for(unsigned i=0;i<code.size();++i){auto changed=code;changed[i]^=1;
      const auto word=GxPixelFixture(family)[i/4];const bool relocation=(word&0xfc1f0000u)==0x800d0000u && i%4>=2;
      if(!relocation)CHECK(!ProveGxPixelLeaf(changed,family));}
    auto relocated=code;
    for(unsigned i=0;i<code.size()/4;++i)if((GxPixelFixture(family)[i]&0xfc1f0000u)==0x800d0000u){relocated[i*4+2]=0x12;relocated[i*4+3]=0x34;}
    CHECK(ProveGxPixelLeaf(relocated,family)==0x1234);
    for(unsigned trial=0;trial<10000;++trial){
      std::array<unsigned,32> original;for(auto& r:original)r=random();original[13]=0x80300000;
      auto expected=original,actual=original;const auto cached=random();
      const auto word=Reference(family,expected,cached);
      CHECK(ExecuteGxPixelLeaf(family,actual.data(),0x80010000,cached)==word);
      CHECK(actual==expected);
    }
  }
  using namespace GekkoAOT::GX;
  // All gather offsets and fragmentations preserve exact mask/token order.
  for(unsigned before=0;before<32;++before)for(unsigned chunk=1;chunk<=64;++chunk){
    NativeCommandStream stream;CHECK(stream.StagePixel(0xfe,255,before));
    std::vector<unsigned char> bytes(before,0);bytes.insert(bytes.end(),{0x61,0xfe,0,0,255,0,0});
    unsigned ops=0,raw=0;
    for(unsigned off=0;off<bytes.size();off+=chunk)CHECK(stream.ConsumeOperations(bytes.data()+off,std::min<std::size_t>(chunk,bytes.size()-off),
      [&](const auto*,auto n){raw+=n;return true;},[]{return true;},[&](const NativeOperation& operation){
        const auto& state=std::get<NativePixelState>(operation);CHECK(state.reg==0xfe && state.value==255);++ops;return true;}));
    CHECK(ops==1 && raw==before+2 && stream.NativeCalls()==1);
  }
  NativeCommandStream mutated;CHECK(mutated.StagePixel(0x48,0x1234,0));
  const std::array<unsigned char,5> changed{0x61,0x48,0,0x12,0x35};unsigned raw=0;
  CHECK(mutated.ConsumeOperations(changed.data(),changed.size(),[&](const auto*,auto n){raw+=n;return true;},[]{return true;},[](const auto&){CHECK(false);return false;}));
  CHECK(raw==5 && mutated.FallbackCalls()==1);
  std::puts("GX pixel SDK: full-body proof, relocated SDA, 40000 full-GPR PPC equivalence cases, fragments and mutation passed");
}
