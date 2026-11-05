// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "tiny_sr.h"
#include <fstream>
#include <cstring>
#include <memory>
#include <string>
namespace GekkoAOT::GX::CompactSR {
struct Layer {unsigned input=0,output=0;std::vector<float> weights,bias,slope;};
struct Model {std::array<Layer,34> layers;};
inline std::shared_ptr<const Model> Load(const std::string& path){
  std::ifstream f(path,std::ios::binary|std::ios::ate);
  if(!f||f.tellg()!=4853660)return {};
  f.seekg(0);std::vector<std::uint8_t>b(4853660);f.read(reinterpret_cast<char*>(b.data()),b.size());if(!f)return {};
  std::uint64_t digest=14695981039346656037ull;for(auto byte:b)digest=(digest^byte)*1099511628211ull;
  if(digest!=0x7f61b3cbf1087597ull || std::memcmp(b.data(),"GSRVGG01",8)!=0)return {};
  std::size_t at=8;const auto u32=[&](){std::uint32_t n=0;for(unsigned i=0;i<4;++i)n|=std::uint32_t(b.at(at++))<<(i*8);return n;};
  if(u32()!=34)return {};
  auto model=std::make_shared<Model>();
  for(unsigned i=0;i<34;++i){auto& l=model->layers[i];l.input=u32();l.output=u32();if(l.input!=(i==0?3u:64u)||l.output!=(i==33?48u:64u))return {};
    for(auto pair:{std::pair{&l.weights,l.input*l.output*9},std::pair{&l.bias,l.output},std::pair{&l.slope,l.output}}){pair.first->resize(pair.second);for(auto&v:*pair.first){v=std::bit_cast<float>(u32());if(!std::isfinite(v))return {};}}
  }
  if(at!=b.size())return {};return model;
}
inline std::vector<float> Infer(const Model& model,std::span<const float> input,unsigned w,unsigned h){
  std::vector<float> current(input.begin(),input.end());
  for(unsigned layer=0;layer<model.layers.size();++layer){const auto& l=model.layers[layer];
    auto next=layer==0?TinySR::Convolve<3,64,3,false>(current,w,h,l.weights.data(),l.bias.data()):
      layer==33?TinySR::Convolve<64,48,3,false>(current,w,h,l.weights.data(),l.bias.data()):
      TinySR::Convolve<64,64,3,false>(current,w,h,l.weights.data(),l.bias.data());
    if(layer<33)for(std::size_t i=0;i<next.size();++i)if(next[i]<0)next[i]*=l.slope[i%l.output];
    current=std::move(next);
  }
  return current;
}
using InferenceBackend=std::vector<float>(*)(const Model&,std::span<const float>,unsigned,unsigned);
inline std::optional<HD::Image> Enhance(const Model& model,std::span<const std::uint8_t> input,unsigned w,unsigned h,
 unsigned mips,unsigned scale,unsigned wrapS,unsigned wrapT,std::uint64_t budget,std::uint64_t* changed=nullptr,bool include2D=false,InferenceBackend backend=nullptr){
  if((scale!=2&&scale!=4)||!mips||mips>std::bit_width(std::max(w,h))||wrapS>2||wrapT>2||
     HD::MipBytes(w,h,mips)>input.size()||!TinySR::Eligible(input,w,h,include2D))return {};
  unsigned extra=scale==4?2:1;auto bytes=HD::MipBytes(w*scale,h*scale,mips+extra);if(bytes>budget)return {};
  HD::Image out{w*scale,h*scale,mips+extra,std::vector<std::uint8_t>(bytes)};
  HD::Resample(input,w,h,{out.pixels.data(),std::size_t(out.width)*out.height*4},out.width,out.height,true,HD::Encoding::Linear,wrapS,wrapT);
  // 34 convolutions: include the entire receptive field at every tile edge.
  // Workspaces remain bounded; the original GX wrap mode supplies context.
  const auto context=include2D?HD::OpaqueContext(input,w,h,wrapS,wrapT):std::vector<std::uint8_t>{};
  const auto modelSource=context.empty()?input:std::span<const std::uint8_t>(context);
  const unsigned tile=backend?256u:128u;constexpr unsigned halo=34;
  for(unsigned ty=0;ty<h;ty+=tile)for(unsigned tx=0;tx<w;tx+=tile){
    const unsigned tw=std::min(tile,w-tx),th=std::min(tile,h-ty),pw=tw+halo*2,ph=th+halo*2;
    std::vector<float> source(std::size_t(pw)*ph*3);
    for(unsigned y=0;y<ph;++y)for(unsigned x=0;x<pw;++x){
      auto at=(std::size_t(HD::Wrap(int(ty+y)-int(halo),h,wrapT))*w+HD::Wrap(int(tx+x)-int(halo),w,wrapS))*4;
      for(unsigned c=0;c<3;++c)source[(std::size_t(y)*pw+x)*3+c]=modelSource[at+c]/255.f;
    }
    auto prediction=backend?backend(model,source,pw,ph):Infer(model,source,pw,ph);
    if(prediction.size()!=std::size_t(pw)*ph*48)return {};
    auto sample=[&](unsigned x,unsigned y,unsigned c){
      auto base=std::size_t(y/4+halo)*pw+x/4+halo;
      return prediction[base*48+c*16+(y%4)*4+x%4]+source[base*3+c];
    };
    const unsigned block=4/scale;
    for(unsigned y=0;y<th*scale;++y)for(unsigned x=0;x<tw*scale;++x){
      auto at=(std::size_t(ty*scale+y)*out.width+tx*scale+x)*4;
      bool different=false;
      for(unsigned c=0;c<3;++c){unsigned value=0;for(unsigned j=0;j<block;++j)for(unsigned i=0;i<block;++i)value+=unsigned(std::lround(std::clamp(sample(x*block+i,y*block+j,c),0.f,1.f)*255));
        const auto v=std::uint8_t((value+(block*block)/2)/(block*block));different|=std::abs(int(v)-int(out.pixels[at+c]))>1;out.pixels[at+c]=v;}
      if(changed&&different)++*changed;
    }
  }
  std::size_t offset=std::size_t(out.width)*out.height*4;
  if(scale==4){const unsigned mw=w*2,mh=h*2;
    for(unsigned y=0;y<mh;++y)for(unsigned x=0;x<mw;++x)for(unsigned c=0;c<4;++c){unsigned sum=0;
      for(unsigned j=0;j<2;++j)for(unsigned i=0;i<2;++i)sum+=out.pixels[(std::size_t(y*2+j)*out.width+x*2+i)*4+c];
      out.pixels[offset+(std::size_t(y)*mw+x)*4+c]=std::uint8_t((sum+2)/4);}
    offset+=std::size_t(mw)*mh*4;
  }
  std::copy_n(input.begin(),std::size_t(HD::MipBytes(w,h,mips)),out.pixels.begin()+offset);
  return out;
}

}
