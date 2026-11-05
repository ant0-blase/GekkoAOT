// SPDX-License-Identifier: GPL-3.0-or-later
#include "../../runtime/gx/material_reconstruction.h"
#include <cstdio>
#include <cstdlib>
using namespace GekkoAOT::GX;
static void Check(bool condition,const char* text){if(!condition){std::fprintf(stderr,"FAIL: %s\n",text);std::exit(1);}}
int main(){
  constexpr unsigned w=96,h=96;
  std::vector<std::uint8_t> input(w*h*4);
  for(unsigned y=0;y<h;++y)for(unsigned x=0;x<w;++x){
    const unsigned i=(y*w+x)*4;
    // A noisy, opaque grid of tiles with repeated mortar in both axes.
    const bool seam=(x%16)<2 || (y%16)<2;
    const unsigned noise=(x*17+y*31+x*y*3)%41;
    input[i]=seam?25:120+noise;input[i+1]=seam?22:90+(x*7+y*11)%29;
    input[i+2]=seam?20:70+(x*13+y*5)%37;input[i+3]=255;
  }
  const auto original=input;
  auto tiled=Material::Reconstruct(input,w,h,7,1,1);
  Check(tiled.kind==Material::Kind::StructuredRelief,"repeated opaque tile structure has a height hypothesis");
  Check(tiled.confidence>.8f,"strong repeated seams have high confidence");
  Check(input==original,"source texture is immutable");
  Check(tiled.map.pixels.size()==HD::MipBytes(w,h,7),"all mip levels are produced");
  unsigned low=255,high=0,normalPixels=0;
  for(unsigned i=0;i<w*h;++i){low=std::min(low,unsigned(tiled.map.pixels[i*4+3]));high=std::max(high,unsigned(tiled.map.pixels[i*4+3]));
    normalPixels+=std::abs(int(tiled.map.pixels[i*4])-128)>2 || std::abs(int(tiled.map.pixels[i*4+1])-128)>2;
    Check(tiled.map.pixels[i*4+2]>=183,"estimated diffuse surfaces remain rough");}
  Check(high-low>200 && normalPixels>w*h/8,"height and surface normals change actual texels");
  input[3]=254;Check(Material::Reconstruct(input,w,h,1).kind==Material::Kind::Unknown,"translucency excludes reconstruction");
  input=original;for(unsigned i=0;i<w*h;++i){input[i*4]=input[i*4+1]=input[i*4+2]=std::uint8_t((i%4)*70);}
  Check(Material::Reconstruct(input,w,h,1).kind==Material::Kind::Unknown,"limited-palette artwork is excluded");
  input=original;std::uint32_t random=42;
  for(unsigned i=0;i<w*h*4;++i){random=random*1664525+1013904223;input[i]=(i%4==3)?255:random>>24;}
  auto irregular=Material::Reconstruct(input,w,h,1,2,2);
  Check(irregular.kind==Material::Kind::TexturedDiffuse,"noise does not invent structured POM");
  for(unsigned i=0;i<w*h;++i)Check(irregular.map.pixels[i*4+3]==128,"unproved height stays flat");
  // A 16-color console texture: refused as interface art, a surface in a scene.
  std::vector<std::uint8_t> palette(std::size_t(w)*h*4);
  for(unsigned y=0;y<h;++y)for(unsigned x=0;x<w;++x){
    const unsigned level=(((x*13+y*7)^(x*y))%16u)*16u;auto* p=palette.data()+(std::size_t(y)*w+x)*4;
    p[0]=std::uint8_t(level);p[1]=std::uint8_t(level*3/4);p[2]=std::uint8_t(level/2);p[3]=255;
  }
  Check(Material::Reconstruct(palette,w,h,1).kind==Material::Kind::Unknown,"small palettes stay out as interface art");
  Check(Material::Reconstruct(palette,w,h,1,0,0,true).kind!=Material::Kind::Unknown,"small palettes on scene surfaces get a material");
  std::vector<std::uint8_t> flat(std::size_t(w)*h*4);
  for(std::size_t i=0;i<flat.size();i+=4){const std::uint8_t v=(i/4)%2?90:100;flat[i]=flat[i+1]=flat[i+2]=v;flat[i+3]=255;}
  Check(Material::Reconstruct(flat,w,h,1,0,0,true).kind==Material::Kind::Unknown,"two-color tints stay flat even in a scene");
  Check(Material::Reconstruct({},w,h,1).kind==Material::Kind::Unknown,"short source is rejected");
  Check(Material::Reconstruct(original,w,h,99).kind==Material::Kind::Unknown,"invalid mip count is rejected");
  {
    std::vector<std::uint8_t> source(16*16*4,80);
    for(unsigned i=0;i<16*16;++i)source[i*4+3]=255;
    HD::Image sky{64,64,3,std::vector<std::uint8_t>(HD::MipBytes(64,64,3),200)};
    const auto alpha=sky.pixels[3];
    HD::PreserveBackgroundEdges(sky,source,16,16,0,0);
    for(unsigned y=0;y<64;++y){
      Check(sky.pixels[(y*64)*4]==80,"left sky boundary keeps source colour");
      Check(sky.pixels[(y*64+63)*4]==80,"right sky boundary keeps source colour");
    }
    Check(sky.pixels[(32*64+32)*4]==200,"sky reconstruction detail retained inside face");
    for(std::size_t i=3;i<sky.pixels.size();i+=4)Check(sky.pixels[i]==alpha,"edge correction preserves alpha");
    Check(sky.pixels[(64*64+32*32)*4]==200,"authored mip payload is unchanged");
  }
  std::printf("MATERIAL_RECONSTRUCTION_PASSED confidence=%.3f normal_pixels=%u height_range=%u\n",tiled.confidence,normalPixels,high-low);
}
