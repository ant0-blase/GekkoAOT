// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "hd_texture.h"
#include <cstdlib>
#include <numeric>

namespace GekkoAOT::GX::Material {
enum class Kind : unsigned { Unknown, TexturedDiffuse, StructuredRelief };
struct Surface {
  HD::Image map;
  Kind kind=Kind::Unknown;
  float confidence=0;
};
struct Repetition {unsigned period=0,phase=0;float confidence=0;};
// A height hypothesis requires repeated, aligned texture structure in both
// directions. Brightness alone never determines relief or material type.
inline Repetition FindRepetition(const std::vector<float>& profile){
  Repetition result;
  if(profile.size()<32)return result;
  const float mean=std::accumulate(profile.begin(),profile.end(),0.f)/profile.size();
  const float peak=*std::max_element(profile.begin(),profile.end());
  if(mean<.005f || peak<mean*1.8f)return result;
  float best=.7f;
  for(unsigned period=8;period<=std::min<std::size_t>(64,profile.size()/3);++period){
    float cross=0,a=0,b=0;
    for(unsigned i=0;i+period<profile.size();++i){
      const float x=profile[i]-mean,y=profile[i+period]-mean;
      cross+=x*y;a+=x*x;b+=y*y;
    }
    const float correlation=cross/std::sqrt(std::max(a*b,1e-12f));
    if(correlation<=best+.01f)continue;
    best=correlation;result.period=period;
  }
  if(!result.period)return result;
  float maximum=0;
  for(unsigned phase=0;phase<result.period;++phase){
    float sum=0;unsigned count=0;
    for(unsigned at=phase;at<profile.size();at+=result.period){sum+=profile[at];++count;}
    sum/=std::max(count,1u);
    if(sum>maximum){maximum=sum;result.phase=phase;}
  }
  result.confidence=std::clamp((best-.5f)*2,0.f,1.f);
  return result;
}
inline Surface Reconstruct(std::span<const std::uint8_t> pixels,unsigned w,unsigned h,unsigned mips,unsigned wrapS=0,unsigned wrapT=0,bool world=false){
  Surface surface;
  if(w<16 || h<16 || w>2048 || h>2048 || !mips || mips>std::bit_width(std::max(w,h)) ||
     pixels.size()<std::size_t(w)*h*4)return surface;
  std::unordered_set<std::uint32_t> colors;
  std::vector<float> luma(std::size_t(w)*h),chroma(luma.size()),xProfile(w),yProfile(h);
  for(std::size_t at=0;at<luma.size();++at){
    const auto* p=pixels.data()+at*4;
    if(p[3]!=255)return surface; // alpha tests, sprites and foliage stay intact
    if(colors.size()<129)colors.insert(std::uint32_t(p[0])|(std::uint32_t(p[1])<<8)|(std::uint32_t(p[2])<<16));
    luma[at]=(54.f*p[0]+183.f*p[1]+19.f*p[2])/(256.f*255);
    chroma[at]=(float(p[0])-p[2])/(255.f*2);
  }
  // Flat, cel and interface art has no invented relief. Scene surfaces with a
  // small palette are ordinary console textures: only the few-color ones (flat
  // tints, stripes) stay out.
  if(colors.size()<(world?8u:129u))return surface;
  const auto pixel=[&](int x,int y){return std::size_t(HD::Wrap(y,int(h),wrapT))*w+HD::Wrap(x,int(w),wrapS);};
  if(colors.size()<=128){
    // Quantized gradients are staircases: read as relief they would print the
    // palette's bands on the surface, so the structure is taken after a low-pass.
    const auto low_pass=[&](std::vector<float>& channel){
      std::vector<float> blurred(channel.size());
      for(unsigned y=0;y<h;++y)for(unsigned x=0;x<w;++x){
        float sum=0;
        for(int j=-1;j<=1;++j)for(int i=-1;i<=1;++i)
          sum+=channel[pixel(int(x)+i,int(y)+j)]*float((2-std::abs(i))*(2-std::abs(j)));
        blurred[std::size_t(y)*w+x]=sum/16;
      }
      channel=std::move(blurred);
    };
    low_pass(luma);low_pass(chroma);
  }
  float energy=0;
  for(unsigned y=0;y<h;++y)for(unsigned x=0;x<w;++x){
    const auto left=pixel(int(x)-1,y),right=pixel(int(x)+1,y),top=pixel(x,int(y)-1),bottom=pixel(x,int(y)+1);
    const float dx=std::hypot(luma[right]-luma[left],chroma[right]-chroma[left]);
    const float dy=std::hypot(luma[bottom]-luma[top],chroma[bottom]-chroma[top]);
    xProfile[x]+=dx/h;yProfile[y]+=dy/w;energy+=dx+dy;
  }
  energy/=std::size_t(w)*h;
  if(energy<.012f)return surface;
  const auto horizontal=FindRepetition(xProfile),vertical=FindRepetition(yProfile);
  surface.kind=horizontal.period && vertical.period &&
    std::min(horizontal.confidence,vertical.confidence)>.8f?Kind::StructuredRelief:Kind::TexturedDiffuse;
  surface.confidence=surface.kind==Kind::StructuredRelief?
    std::min(horizontal.confidence,vertical.confidence):std::clamp(energy*3,.15f,.55f);
  std::vector<float> height(luma.size(),.5f);
  const auto seam=[](unsigned at,const Repetition& r){
    const unsigned phase=(at+r.period-r.phase)%r.period;
    const float distance=float(std::min(phase,r.period-phase));
    const float t=std::clamp(distance/std::max(1.5f,r.period*.10f),0.f,1.f);
    return t*t*(3-2*t);
  };
  if(surface.kind==Kind::StructuredRelief)
    for(unsigned y=0;y<h;++y)for(unsigned x=0;x<w;++x)
      height[std::size_t(y)*w+x]=std::min(seam(x,horizontal),seam(y,vertical));
  surface.map={w,h,mips,std::vector<std::uint8_t>(HD::MipBytes(w,h,mips))};
  // Roughness from the image: no authored data exists. Worn metal and other
  // low-saturation, structured surfaces get smoother, and within a surface the
  // brighter marks are the polished ones.
  float meanLuma=0,meanChroma=0;
  for(std::size_t i=0;i<luma.size();++i){meanLuma+=luma[i];meanChroma+=std::abs(chroma[i]);}
  meanLuma/=luma.size();meanChroma/=luma.size();
  float spread=0;for(const float v:luma)spread+=(v-meanLuma)*(v-meanLuma);
  spread=std::sqrt(spread/luma.size());
  const float neutral=1-std::clamp(meanChroma/.08f,0.f,1.f);
  const float baseRoughness=.88f-.33f*neutral*std::clamp(energy/.06f,0.f,1.f);
  for(unsigned y=0;y<h;++y)for(unsigned x=0;x<w;++x){
    const auto at=std::size_t(y)*w+x;
    const auto left=pixel(int(x)-1,y),right=pixel(int(x)+1,y),top=pixel(x,int(y)-1),bottom=pixel(x,int(y)+1);
    float dx,dy;
    if(surface.kind==Kind::StructuredRelief){
      dx=(height[right]-height[left])*.6f;dy=(height[bottom]-height[top])*.6f;
    }else{
      // Bounded, explicitly estimated micro-normal variation from joint color
      // structure. It produces no height map, POM or metallic response.
      dx=(luma[right]-luma[left]+.5f*(chroma[right]-chroma[left]))*2.5f;
      dy=(luma[bottom]-luma[top]+.5f*(chroma[bottom]-chroma[top]))*2.5f;
    }
    const float inverse=1/std::sqrt(1+dx*dx+dy*dy);
    auto* p=surface.map.pixels.data()+at*4;
    p[0]=std::uint8_t(std::lround((.5f-.5f*dx*inverse)*255));
    p[1]=std::uint8_t(std::lround((.5f-.5f*dy*inverse)*255));
    const float polish=std::clamp((luma[at]-meanLuma)/std::max(spread*1.5f,.01f),0.f,1.f);
    p[2]=std::uint8_t(std::lround(std::clamp(baseRoughness-.22f*polish,.3f,.95f)*255));
    p[3]=std::uint8_t(std::lround(height[at]*255));
  }
  std::size_t previous=0,offset=std::size_t(w)*h*4;unsigned pw=w,ph=h;
  for(unsigned level=1;level<mips;++level){
    const unsigned nw=std::max(1u,pw/2),nh=std::max(1u,ph/2);
    for(unsigned y=0;y<nh;++y)for(unsigned x=0;x<nw;++x)for(unsigned c=0;c<4;++c){
      unsigned sum=0;
      for(unsigned j=0;j<2;++j)for(unsigned i=0;i<2;++i)
        sum+=surface.map.pixels[previous+(std::size_t(std::min(y*2+j,ph-1))*pw+std::min(x*2+i,pw-1))*4+c];
      surface.map.pixels[offset+(std::size_t(y)*nw+x)*4+c]=std::uint8_t((sum+2)/4);
    }
    previous=offset;offset+=std::size_t(nw)*nh*4;pw=nw;ph=nh;
  }
  return surface;
}
} // namespace GekkoAOT::GX::Material
