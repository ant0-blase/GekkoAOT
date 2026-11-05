// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "hd_texture.h"
#include "espcn_weights.h"
#if defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP>=2)
#include <immintrin.h>
#define GEKKOAOT_TINY_SR_SSE2 1
#endif
namespace GekkoAOT::GX::TinySR {
inline std::uint64_t Fingerprint(unsigned scale){return (scale==4?Weights::X4Fingerprint:Weights::X2Fingerprint)^0x4348524f4d413035ull;}
#if defined(__GNUC__) && (defined(__x86_64__) || defined(__i386__))
// Dispatch only on CPUs that support AVX2+FMA; portable SSE2/scalar paths below
// remain available on Windows and other hosts. Eight output channels per FMA.
template<unsigned IC,unsigned OC,unsigned K,bool Relu>
__attribute__((target("avx2,fma"))) inline std::vector<float> ConvolveAVX2(std::span<const float> input,unsigned width,unsigned height,const float* weights,const float* bias){
  std::vector<float> output(std::size_t(width)*height*OC);
  for(unsigned y=0;y<height;++y)for(unsigned x=0;x<width;++x){
    __m256 accumulators[OC/8];for(unsigned o=0;o<OC/8;++o)accumulators[o]=_mm256_loadu_ps(bias+o*8);
    for(unsigned ky=0;ky<K;++ky){int sy=int(y)+int(ky)-int(K/2);if(sy<0||sy>=int(height))continue;
      for(unsigned kx=0;kx<K;++kx){int sx=int(x)+int(kx)-int(K/2);if(sx<0||sx>=int(width))continue;
        const auto* pixel=input.data()+(std::size_t(sy)*width+sx)*IC;const auto* kernel=weights+(ky*K+kx)*IC*OC;
        for(unsigned i=0;i<IC;++i){const auto value=_mm256_set1_ps(pixel[i]);for(unsigned o=0;o<OC/8;++o)accumulators[o]=_mm256_fmadd_ps(value,_mm256_loadu_ps(kernel+i*OC+o*8),accumulators[o]);}
      }
    }
    auto* pixel=output.data()+(std::size_t(y)*width+x)*OC;
    for(unsigned o=0;o<OC/8;++o){auto value=accumulators[o];if constexpr(Relu)value=_mm256_max_ps(value,_mm256_setzero_ps());_mm256_storeu_ps(pixel+o*8,value);}
  }return output;
}
#endif
// The complete inference runtime: three fixed convolutions, ReLU and subpixel
// rearrangement. No framework, graph interpreter, Python or external process.
// Tensor layout matches the imported pretrained HWIO weights and NHWC input.
template<unsigned IC,unsigned OC,unsigned K,bool Relu>
inline std::vector<float> Convolve(std::span<const float> input,unsigned width,unsigned height,
                                  const float* weights,const float* bias){
#if defined(__GNUC__) && (defined(__x86_64__) || defined(__i386__))
  if constexpr(OC%8==0){static const bool available=__builtin_cpu_supports("avx2")&&__builtin_cpu_supports("fma");if(available)return ConvolveAVX2<IC,OC,K,Relu>(input,width,height,weights,bias);}
#endif
  std::vector<float> output(std::size_t(width)*height*OC);
  for(unsigned y=0;y<height;++y)for(unsigned x=0;x<width;++x){
#ifdef GEKKOAOT_TINY_SR_SSE2
    __m128 accumulators[OC/4];
    for(unsigned o=0;o<OC/4;++o)accumulators[o]=_mm_loadu_ps(bias+o*4);
#else
    float accumulators[OC];std::copy_n(bias,OC,accumulators);
#endif
    for(unsigned ky=0;ky<K;++ky){const int sy=int(y)+int(ky)-int(K/2);if(sy<0||sy>=int(height))continue;
      for(unsigned kx=0;kx<K;++kx){const int sx=int(x)+int(kx)-int(K/2);if(sx<0||sx>=int(width))continue;
        const auto* pixel=input.data()+(std::size_t(sy)*width+sx)*IC;
        const auto* kernel=weights+(ky*K+kx)*IC*OC;
        for(unsigned i=0;i<IC;++i){
#ifdef GEKKOAOT_TINY_SR_SSE2
          const auto value=_mm_set1_ps(pixel[i]);
          for(unsigned o=0;o<OC/4;++o)accumulators[o]=_mm_add_ps(accumulators[o],_mm_mul_ps(value,_mm_loadu_ps(kernel+i*OC+o*4)));
#else
          for(unsigned o=0;o<OC;++o)accumulators[o]+=pixel[i]*kernel[i*OC+o];
#endif
        }
      }
    }
    auto* pixel=output.data()+(std::size_t(y)*width+x)*OC;
#ifdef GEKKOAOT_TINY_SR_SSE2
    for(unsigned o=0;o<OC/4;++o){auto value=accumulators[o];if constexpr(Relu)value=_mm_max_ps(value,_mm_setzero_ps());_mm_storeu_ps(pixel+o*4,value);}
#else
    for(unsigned o=0;o<OC;++o)pixel[o]=Relu?std::max(0.f,accumulators[o]):accumulators[o];
#endif
  }
  return output;
}
template<unsigned Scale>
inline std::vector<float> Infer(std::span<const float> input,unsigned w,unsigned h){
  const auto* f1=Scale==2?Weights::X2F1:Weights::X4F1;
  const auto* f2=Scale==2?Weights::X2F2:Weights::X4F2;
  const auto* f3=Scale==2?Weights::X2F3:Weights::X4F3;
  const auto* b1=Scale==2?Weights::X2B1:Weights::X4B1;
  const auto* b2=Scale==2?Weights::X2B2:Weights::X4B2;
  const auto* b3=Scale==2?Weights::X2B3:Weights::X4B3;
  auto first=Convolve<1,64,5,true>(input,w,h,f1,b1);
  auto second=Convolve<64,32,3,true>(first,w,h,f2,b2);
  std::vector<float>().swap(first);
  auto last=Convolve<32,Scale*Scale,3,false>(second,w,h,f3,b3);
  for(float& v:last)v=std::tanh(v);
  return last;
}
inline bool Eligible(std::span<const std::uint8_t> input,unsigned w,unsigned h,bool include2D=false){
  if(w<(include2D?4u:16u)||h<(include2D?4u:16u)||w>(include2D?1024u:512u)||h>(include2D?1024u:512u)||input.size()<std::size_t(w)*h*4)return false;
  // Original GX/TEV color data can be predominantly blue (sky, water, cars).
  // RGB values alone do not prove a vector/normal texture. The integration
  // handles actual normal resources separately and protects flat art first.
  if(include2D)return true;
  std::unordered_set<std::uint32_t> colors;unsigned vectors=0;
  for(unsigned i=0;i<w*h;++i){const auto* p=input.data()+i*4;if(!include2D && p[3]!=255)return false;
    if(colors.size()<=128)colors.insert(std::uint32_t(p[0])|(std::uint32_t(p[1])<<8)|(std::uint32_t(p[2])<<16));
    const float nx=p[0]/127.5f-1,ny=p[1]/127.5f-1,nz=p[2]/127.5f-1;
    const float n=nx*nx+ny*ny+nz*nz;
    vectors+=p[2]>180&&p[2]>p[0]+20&&p[2]>p[1]+20&&n>.8f&&n<1.2f;
  }
  return (include2D || colors.size()>128) && vectors<std::uint64_t(w)*h*8/10;
}
// Back-project the difference between the authored source and the box-
// degraded reconstruction. This constrains learned detail to the real source;
// it is a data-consistency solve, not an unsharp-mask filter.
inline void RestoreSource(std::span<const std::uint8_t> source,unsigned w,unsigned h,
                          std::span<std::uint8_t> output,unsigned scale,unsigned wrapS,unsigned wrapT){
  const unsigned ow=w*scale,oh=h*scale;
  const auto y=[](const std::uint8_t* p){return .299f*p[0]+.587f*p[1]+.114f*p[2];};
  std::vector<float> error(std::size_t(w)*h);
  for(unsigned iteration=0;iteration<6;++iteration){
    for(unsigned sy=0;sy<h;++sy)for(unsigned sx=0;sx<w;++sx){
      float mean=0;
      for(unsigned j=0;j<scale;++j)for(unsigned i=0;i<scale;++i)mean+=y(output.data()+(std::size_t(sy*scale+j)*ow+sx*scale+i)*4);
      error[std::size_t(sy)*w+sx]=y(source.data()+(std::size_t(sy)*w+sx)*4)-mean/(scale*scale);
    }
    for(unsigned py=0;py<oh;++py)for(unsigned px=0;px<ow;++px){
      const float sx=(px+.5f)/scale-.5f,sy=(py+.5f)/scale-.5f;
      const int ix=int(std::floor(sx)),iy=int(std::floor(sy));const float fx=sx-ix,fy=sy-iy;
      float delta=0;
      for(unsigned j=0;j<2;++j)for(unsigned i=0;i<2;++i)
        delta+=error[std::size_t(HD::Wrap(iy+j,h,wrapT))*w+HD::Wrap(ix+i,w,wrapS)]*(i?fx:1-fx)*(j?fy:1-fy);
      auto* p=output.data()+(std::size_t(py)*ow+px)*4;
      delta=std::clamp(delta*.9f,-float(std::min({p[0],p[1],p[2]})),255.f-std::max({p[0],p[1],p[2]}));
      for(unsigned c=0;c<3;++c)p[c]=std::uint8_t(std::lround(p[c]+delta));
    }
  }
}
inline std::optional<HD::Image> Enhance(std::span<const std::uint8_t> input,unsigned w,unsigned h,unsigned mips,
                                       unsigned scale,unsigned wrapS,unsigned wrapT,std::uint64_t budget,
                                       std::uint64_t* changed=nullptr,bool high=false,bool include2D=false){
  if((scale!=2&&scale!=4)||!mips||mips>std::bit_width(std::max(w,h))||wrapS>2||wrapT>2||
     HD::MipBytes(w,h,mips)>input.size()||!Eligible(input,w,h,include2D))return {};
  const unsigned extra=scale==4?2:1;const auto bytes=HD::MipBytes(w*scale,h*scale,mips+extra);
  if(bytes>budget)return {};
  // Four source texels of context cover the network's receptive radius.
  // Wrap/clamp/mirror is applied to original texels, then the halo is cropped.
  // This preserves tiling instead of using zero padding at a GX texture seam.
  const auto context=include2D?HD::OpaqueContext(input,w,h,wrapS,wrapT):std::vector<std::uint8_t>{};
  const auto modelSource=context.empty()?input:std::span<const std::uint8_t>(context);
  constexpr unsigned halo=4,tile=128;
  HD::Image out{w*scale,h*scale,mips+extra,std::vector<std::uint8_t>(bytes)};
  HD::Resample(input,w,h,{out.pixels.data(),std::size_t(out.width)*out.height*4},out.width,out.height,true,HD::Encoding::Linear,wrapS,wrapT);
  // Bound activation memory independently of texture size. Four texels of
  // overlap reproduce the complete receptive field, including tile borders.
  for(unsigned ty=0;ty<h;ty+=tile)for(unsigned tx=0;tx<w;tx+=tile){
    const unsigned tw=std::min(tile,w-tx),th=std::min(tile,h-ty),pw=tw+halo*2,ph=th+halo*2;
    std::vector<float> luma(std::size_t(pw)*ph);
    for(unsigned y=0;y<ph;++y)for(unsigned x=0;x<pw;++x){
      const auto at=(std::size_t(HD::Wrap(int(ty+y)-int(halo),int(h),wrapT))*w+HD::Wrap(int(tx+x)-int(halo),int(w),wrapS))*4;
      luma[std::size_t(y)*pw+x]=(.299f*modelSource[at]+.587f*modelSource[at+1]+.114f*modelSource[at+2])/255.f;
    }
    auto prediction=scale==2?Infer<2>(luma,pw,ph):Infer<4>(luma,pw,ph);
    if(high){
      // Two orientations reduce the compact model's directional bias. Invert
      // transpose including the pixel-shuffle phase before averaging.
      std::vector<float> transposed(luma.size());
      for(unsigned y=0;y<ph;++y)for(unsigned x=0;x<pw;++x)transposed[std::size_t(x)*ph+y]=luma[std::size_t(y)*pw+x];
      const auto alternate=scale==2?Infer<2>(transposed,ph,pw):Infer<4>(transposed,ph,pw);
      for(unsigned y=0;y<ph;++y)for(unsigned x=0;x<pw;++x)
        for(unsigned j=0;j<scale;++j)for(unsigned i=0;i<scale;++i){
          const auto at=(std::size_t(y)*pw+x)*scale*scale+j*scale+i;
          const auto inverse=(std::size_t(x)*ph+y)*scale*scale+i*scale+j;
          prediction[at]=(prediction[at]+alternate[inverse])*.5f;
        }
    }
    for(unsigned y=0;y<th*scale;++y)for(unsigned x=0;x<tw*scale;++x){
      const auto at=(std::size_t(ty*scale+y)*out.width+tx*scale+x)*4;
      const auto index=((std::size_t(y/scale+halo)*pw+x/scale+halo)*scale*scale)+(y%scale)*scale+x%scale;
      const float estimated=std::clamp(prediction[index],0.f,1.f);
      const float original=(.299f*out.pixels[at]+.587f*out.pixels[at+1]+.114f*out.pixels[at+2])/255.f;
      // Keep the learned luma residual in the common RGB gamut so chroma is
      // preserved even at saturated edges. Alpha remains untouched.
      const auto lo=std::min({out.pixels[at],out.pixels[at+1],out.pixels[at+2]});
      const auto hi=std::max({out.pixels[at],out.pixels[at+1],out.pixels[at+2]});
      const float difference=std::clamp(std::clamp(estimated-original,-.2f,.2f)*255,-float(lo),255.f-hi);
      bool different=false;
      for(unsigned c=0;c<3;++c){const auto value=std::uint8_t(std::lround(out.pixels[at+c]+difference));different|=std::abs(int(value)-int(out.pixels[at+c]))>1;out.pixels[at+c]=value;}
      if(different&&changed)++*changed;
    }
  }
  if(high)RestoreSource(input,w,h,{out.pixels.data(),std::size_t(out.width)*out.height*4},scale,wrapS,wrapT);
  std::size_t offset=std::size_t(out.width)*out.height*4;
  if(scale==4){const unsigned mw=w*2,mh=h*2;
    for(unsigned y=0;y<mh;++y)for(unsigned x=0;x<mw;++x)for(unsigned c=0;c<4;++c){unsigned sum=0;
      for(unsigned j=0;j<2;++j)for(unsigned i=0;i<2;++i)sum+=out.pixels[(std::size_t(y*2+j)*out.width+x*2+i)*4+c];
      out.pixels[offset+(std::size_t(y)*mw+x)*4+c]=std::uint8_t((sum+2)/4);}
    if(high){
      // This mip is commonly sampled in game: use the actual 2x model instead
      // of blurring the 4x result down. Authored lower mips stay byte exact.
      const auto twice=Enhance(input,w,h,mips,2,wrapS,wrapT,budget,nullptr,true,include2D);
      if(twice)std::copy_n(twice->pixels.begin(),std::size_t(mw)*mh*4,out.pixels.begin()+offset);
    }
    offset+=std::size_t(mw)*mh*4;
  }
  std::copy_n(input.begin(),std::size_t(HD::MipBytes(w,h,mips)),out.pixels.begin()+offset);
  return out;
}
}
