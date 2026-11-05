// SPDX-License-Identifier: GPL-3.0-or-later
#include "../../runtime/gx/compact_sr.h"
#include <cstdio>
#include <cstdlib>
using namespace GekkoAOT::GX;
static void Check(bool v,const char* text){if(!v){std::fprintf(stderr,"FAIL: %s\n",text);std::exit(1);}}
int main(int argc,char**argv){Check(argc==2,"model path");auto model=CompactSR::Load(argv[1]);Check(bool(model),"pinned converted model loads");
 std::vector<float> input(48);for(unsigned i=0;i<48;++i)input[i]=float((i*17+3)%101)/100;
 auto result=CompactSR::Infer(*model,input,4,4);
 constexpr unsigned indices[]={0,1,15,16,47,99,255,767};
 // Independent NumPy HWIO convolution reference, zero padding, channel PReLU.
 constexpr float expected[]={-.168940797f,-.201813191f,.276396185f,-.184050024f,.332936853f,-.0457234345f,.043806687f,.127055898f};
 for(unsigned i=0;i<8;++i)Check(std::abs(result[indices[i]]-expected[i])<.0001f,"native 34-layer output matches independent inference");
 std::vector<std::uint8_t> source(16*16*4);
 for(unsigned i=0;i<256;++i){source[i*4]=50+i%170;source[i*4+1]=40+(i*7)%180;source[i*4+2]=60+(i*13)%150;source[i*4+3]=255;}
 const auto original=source;std::uint64_t changed=0;
 auto twice=CompactSR::Enhance(*model,source,16,16,1,2,1,1,16<<20,&changed);
 Check(twice && twice->width==32 && twice->height==32 && twice->mips==2,"RGB model output is reconstructed at 2x");Check(changed>200,"learned model changes actual spatial baseline pixels");
 Check(std::memcmp(twice->pixels.data()+32*32*4,source.data(),source.size())==0,"original authored mip retained byte-exact");
 auto four=CompactSR::Enhance(*model,source,16,16,1,4,1,1,16<<20);
 Check(four && four->width==64 && four->mips==3,"RGB model output and generated 2x mip at 4x");
 Check(std::memcmp(four->pixels.data()+64*64*4,twice->pixels.data(),32*32*4)==0,"2x result equals the filtered 4x level");
 Check(source==original,"source immutable");source[3]=254;Check(!CompactSR::Enhance(*model,source,16,16,1,4,1,1,16<<20),"alpha textures stay protected");
 std::vector<std::uint8_t> sky(1024*8*4);
 for(unsigned y=0;y<8;++y)for(unsigned x=0;x<1024;++x){auto* p=sky.data()+(y*1024+x)*4;p[0]=90+x%70;p[1]=90+(x+y*3)%70;p[2]=240;p[3]=255;}
 Check(TinySR::Eligible(sky,1024,8,true),"blue original sky and elongated 1024-wide texture admitted");
 Check(!HD::LimitedPalette(sky,1024,8),"photo sky is not mistaken for pixel art");
 const auto stub=[](const CompactSR::Model&,std::span<const float>,unsigned w,unsigned h){return std::vector<float>(std::size_t(w)*h*48);};
 auto large=CompactSR::Enhance(*model,sky,1024,8,1,4,1,1,16<<20,nullptr,true,stub);
 Check(large && large->width==4096 && large->height==32,"larger and narrow source uses tiled neural backend");
 std::vector<std::uint8_t> tiny(8*8*4);
 for(unsigned i=0;i<64;++i){tiny[i*4]=i*3;tiny[i*4+1]=(i*17)%255;tiny[i*4+2]=(i*37)%255;tiny[i*4+3]=255;}
 Check(!HD::LimitedPalette(tiny,8,8),"small photographic texture is not excluded just by color count");
 Check(TinySR::Eligible(tiny,8,8,true),"small original color texture admitted");
 std::fill(tiny.begin(),tiny.end(),255);
 Check(HD::LimitedPalette(tiny,8,8),"flat glyph and pixel-art protection retained");
 Check(!CompactSR::Load("missing-model.gsr"),"missing model is reported");
 std::printf("NATIVE_COMPACT_SR_PASSED changed_pixels=%llu dependencies=none\n",(unsigned long long)changed);
}
