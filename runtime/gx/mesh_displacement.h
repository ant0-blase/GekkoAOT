// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "mesh_refinement.h"
#include "hd_texture.h"
namespace GekkoAOT::GX::Mesh {
struct HeightField {
  unsigned width=0,height=0,wrapS=0,wrapT=0;
  std::uint64_t identity=0;
  std::vector<std::uint8_t> pixels;
  float Sample(float u,float v)const{
    const float x=u*width-.5f,y=v*height-.5f;const int ix=int(std::floor(x)),iy=int(std::floor(y));
    const float fx=x-ix,fy=y-iy;float value=0;
    for(unsigned j=0;j<2;++j)for(unsigned i=0;i<2;++i)value+=pixels[std::size_t(HD::Wrap(iy+j,height,wrapT))*width+HD::Wrap(ix+i,width,wrapS)]/255.f*(i?fx:1-fx)*(j?fy:1-fy);
    return value;
  }
};
inline bool Displace(Result& result,std::span<const Vertex> input,const HeightField& height,float depth){
  if(!height.width||!height.height||height.pixels.size()!=std::size_t(height.width)*height.height||!std::isfinite(depth)||depth<=0||depth>.05f||result.vertices.size()!=result.samples.size())return false;
  bool moved=false;
  for(unsigned i=0;i<result.vertices.size();++i){
    const auto& sample=result.samples[i];if(sample.patch>=result.patches.size())return false;
    const auto& patch=result.patches[sample.patch];float length=0;unsigned edges=0;
    for(unsigned e=0;e<3;++e){const auto& a=input[patch.vertices[e]];const auto& b=input[patch.vertices[(e+1)%3]];
      const float du=a.attributes[8]-b.attributes[8],dv=a.attributes[9]-b.attributes[9],uv=std::hypot(du,dv);
      if(uv>1e-5f){length+=Length(a.position-b.position)/uv;++edges;}
    }
    if(!edges)continue;
    // Pin patch edges to the original surface. This deliberately trades
    // silhouette displacement for crack-free UV seams and hard boundaries.
    const float edge=std::min({sample.bary[0],sample.bary[1],sample.bary[2]});
    const float fade=std::clamp(edge*8.f,0.f,1.f);
    auto& v=result.vertices[i];float amount=(height.Sample(v.attributes[8],v.attributes[9])-.5f)*depth*(length/edges)*fade;
    // Bound actual movement by the local source triangle size.
    float extent=0;for(unsigned e=0;e<3;++e)extent=std::max(extent,Length(input[patch.vertices[e]].position-input[patch.vertices[(e+1)%3]].position));
    amount=std::clamp(amount,-extent*.02f,extent*.02f);
    if(!std::isfinite(amount)||!Finite(v.normal))return false;
    v.position=v.position+Normalize(v.normal)*amount;moved|=std::abs(amount)>extent*1e-6f;
  }
  return moved;
}
}
