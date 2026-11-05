// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <optional>
#include <span>
#include <unordered_set>
#include <vector>

namespace GekkoAOT::GX::HD {
enum class Encoding { Linear, SRGB, Normal };
struct Image {
  unsigned width=0,height=0,mips=0;
  std::vector<std::uint8_t> pixels;
};
inline std::uint64_t MipBytes(unsigned w,unsigned h,unsigned mips) {
  std::uint64_t n=0;
  for(unsigned i=0;i<mips;++i){n+=std::uint64_t(w)*h*4;w=std::max(1u,w/2);h=std::max(1u,h/2);}
  return n;
}
inline float Decode(float x,Encoding encoding) {
  return encoding==Encoding::SRGB ? (x<=.04045f?x/12.92f:std::pow((x+.055f)/1.055f,2.4f)) : x;
}
inline float Encode(float x,Encoding encoding) {
  x=std::clamp(x,0.f,1.f);
  return encoding==Encoding::SRGB ? (x<=.0031308f?x*12.92f:1.055f*std::pow(x,1.f/2.4f)-.055f) : x;
}
// Catmull-Rom in fast mode; Lanczos-2 in high quality mode. Alpha is filtered
// with premultiplied RGB, so invisible palette colors cannot bleed at edges.
inline float Weight(float x,bool high) {
  x=std::abs(x);
  if(x>=2.f)return 0;
  if(high){if(x<1e-6f)return 1;constexpr float pi=3.14159265358979323846f;
    return std::sin(pi*x)*std::sin(pi*x*.5f)/(pi*pi*x*x*.5f);}
  return x<1 ? ((1.5f*x-2.5f)*x)*x+1 : ((-.5f*x+2.5f)*x-4)*x+2;
}
inline int Wrap(int x,int size,unsigned mode) {
  if(mode==0)return std::clamp(x,0,size-1);
  const int period=mode==2?size*2:size;
  x=(x%period+period)%period;
  return x<size?x:period-1-x;
}
inline void Resample(std::span<const std::uint8_t> source,unsigned w,unsigned h,
                     std::span<std::uint8_t> target,unsigned tw,unsigned th,
                     bool high,Encoding encoding,unsigned wrapS,unsigned wrapT) {
  struct Taps {std::array<int,4> index;std::array<float,4> weight;};
  const auto taps=[high](unsigned source,unsigned target,unsigned wrap){
    std::vector<Taps> result(target);
    for(unsigned x=0;x<target;++x){const float at=(float(x)+.5f)*source/target-.5f;const int base=int(std::floor(at));
      for(int i=-1;i<=2;++i){result[x].index[i+1]=Wrap(base+i,source,wrap);result[x].weight[i+1]=Weight(at-(base+i),high);}}
    return result;
  };
  // Kernels depend only on one axis. Avoid recomputing trigonometric weights
  // sixteen times per output pixel, while retaining the same 4x4 filter,
  // premultiplication, bounds and floating-point accumulation order.
  const auto horizontal=taps(w,tw,wrapS),vertical=taps(h,th,wrapT);
  for(unsigned y=0;y<th;++y)for(unsigned x=0;x<tw;++x){
    std::array<float,4> sum{};float weight=0;
    std::array<float,3> lo{1,1,1},hi{};
    for(int j=-1;j<=2;++j)for(int i=-1;i<=2;++i){
      const float k=horizontal[x].weight[i+1]*vertical[y].weight[j+1];
      const auto at=(std::size_t(vertical[y].index[j+1])*w+horizontal[x].index[i+1])*4;
      const float a=source[at+3]/255.f;
      for(unsigned c=0;c<3;++c){
        const float v=Decode(source[at+c]/255.f,encoding);
        if(a>0 || encoding==Encoding::Normal){lo[c]=std::min(lo[c],v);hi[c]=std::max(hi[c],v);}
        sum[c]+=v*(encoding==Encoding::Normal?1.f:a)*k;
      }
      sum[3]+=a*k;weight+=k;
    }
    const auto out=(std::size_t(y)*tw+x)*4;
    const float alpha=std::clamp(sum[3]/weight,0.f,1.f);
    std::array<float,3> rgb{};
    for(unsigned c=0;c<3;++c){
      const float divisor=encoding==Encoding::Normal?weight:sum[3];
      rgb[c]=divisor>1e-7f?std::clamp(sum[c]/divisor,lo[c],hi[c]):0.f;
    }
    if(encoding==Encoding::Normal){
      float length=0;for(float v:rgb)length+=(v*2-1)*(v*2-1);
      length=std::sqrt(std::max(length,1e-12f));
      for(float& v:rgb)v=((v*2-1)/length+1)*.5f;
    }
    for(unsigned c=0;c<3;++c)target[out+c]=std::uint8_t(std::lround(Encode(rgb[c],encoding)*255));
    target[out+3]=std::uint8_t(std::lround(alpha*255));
  }
}
// Independently reconstructed sky faces must retain their authored boundary
// colours. Fade only the reconstruction residual near an edge; detail inside
// the face and alpha/UV/wrap semantics remain unchanged.
inline void PreserveBackgroundEdges(Image& output,std::span<const std::uint8_t> original,
                                    unsigned w,unsigned h,unsigned wrapS,unsigned wrapT) {
  if(!w || !h || original.size()<std::size_t(w)*h*4 ||
     output.pixels.size()<MipBytes(output.width,output.height,output.mips))return;
  unsigned mw=output.width,mh=output.height;std::size_t offset=0;
  for(unsigned mip=0;mip<output.mips && (mw>w || mh>h);++mip){
    const auto size=std::size_t(mw)*mh*4;
    std::vector<std::uint8_t> reference(size);
    Resample(original,w,h,reference,mw,mh,true,Encoding::Linear,wrapS,wrapT);
    const float width=2.f*std::max(float(mw)/w,float(mh)/h);
    for(unsigned y=0;y<mh;++y)for(unsigned x=0;x<mw;++x){
      const unsigned edge=std::min({x,y,mw-1-x,mh-1-y});if(edge>=width)continue;
      const float t=std::clamp(edge/width,0.f,1.f),weight=t*t*(3-2*t);
      const auto pixel=(std::size_t(y)*mw+x)*4;
      for(unsigned c=0;c<3;++c){auto& value=output.pixels[offset+pixel+c];
        value=std::uint8_t(std::lround(reference[pixel+c]*(1-weight)+value*weight));}
    }
    offset+=size;mw=std::max(1u,mw/2);mh=std::max(1u,mh/2);
  }
}
// Build a coherent mip chain from reconstructed RGB. Carry authored mip
// changes relative to a source-box pyramid, preserving alpha byte exact. Mips
// deliberately unlike an ordinary pyramid remain entirely authored.
inline void ReconstructMipAppearance(Image& output,std::span<const std::uint8_t> original,unsigned w,unsigned h,unsigned mips,unsigned scale){
  if(!w||!h||!mips||(scale!=2&&scale!=4)||original.size()<MipBytes(w,h,mips)||output.pixels.size()<MipBytes(output.width,output.height,output.mips))return;
  const unsigned extra=scale==4?2:1;
  auto down=[](std::span<const std::uint8_t> in,unsigned iw,unsigned ih){
    unsigned nw=std::max(1u,iw/2),nh=std::max(1u,ih/2);std::vector<std::uint8_t> out(std::size_t(nw)*nh*4);
    for(unsigned y=0;y<nh;++y)for(unsigned x=0;x<nw;++x)for(unsigned c=0;c<4;++c){unsigned sum=0;
      for(unsigned j=0;j<2;++j)for(unsigned i=0;i<2;++i)sum+=in[(std::size_t(std::min(y*2+j,ih-1))*iw+std::min(x*2+i,iw-1))*4+c];
      out[(std::size_t(y)*nw+x)*4+c]=std::uint8_t((sum+2)/4);}
    return out;
  };
  std::size_t prefix=0;for(unsigned i=0;i+1<extra;++i)prefix+=std::size_t(std::max(1u,output.width>>i))*std::max(1u,output.height>>i)*4;
  const unsigned gw=output.width>>(extra-1),gh=output.height>>(extra-1);
  auto generated=down({output.pixels.data()+prefix,std::size_t(gw)*gh*4},gw,gh);
  std::vector<std::uint8_t> baseline(original.begin(),original.begin()+std::size_t(w)*h*4);
  std::size_t dst=0;for(unsigned i=0;i<extra;++i)dst+=std::size_t(std::max(1u,output.width>>i))*std::max(1u,output.height>>i)*4;
  std::size_t src=0;
  for(unsigned level=0;level<mips;++level){
    const std::size_t n=std::size_t(w)*h;double edit=0;
    for(std::size_t i=0;i<n;++i)for(unsigned c=0;c<3;++c)edit+=std::abs(int(original[src+i*4+c])-int(baseline[i*4+c]));
    const bool authoredSpecial=edit/(n*3)>12.;
    for(std::size_t i=0;i<n;++i){
      if(!authoredSpecial)for(unsigned c=0;c<3;++c){const int delta=int(original[src+i*4+c])-int(baseline[i*4+c]);output.pixels[dst+i*4+c]=std::uint8_t(std::clamp(int(generated[i*4+c])+delta,0,255));}
      output.pixels[dst+i*4+3]=original[src+i*4+3];
    }
    dst+=n*4;src+=n*4;
    if(level+1<mips){generated=down(generated,w,h);baseline=down(baseline,w,h);w=std::max(1u,w/2);h=std::max(1u,h/2);}
  }
}
// Pixel-art and font atlases retain their exact colors and alpha values.
// Edge-rule reconstruction selects matching neighbours rather than inventing
// photo texture inside glyphs, icons and flat animated artwork.
inline bool LimitedPalette(std::span<const std::uint8_t> source,unsigned w,unsigned h){
  if(!w||!h||source.size()<std::size_t(w)*h*4)return false;
  std::unordered_set<std::uint32_t> colors;std::size_t adjacent=0,equal=0;
  const auto color=[](const auto* p){return std::uint32_t(p[0])|(std::uint32_t(p[1])<<8)|(std::uint32_t(p[2])<<16);};
  for(unsigned y=0;y<h;++y)for(unsigned x=0;x<w;++x){
    const auto* p=source.data()+(std::size_t(y)*w+x)*4;if(!p[3])continue;
    colors.insert(color(p));if(colors.size()>128)return false;
    for(auto delta:{x?1u:0u,y?w:0u})if(delta){const auto* q=p-delta*4;if(q[3]){++adjacent;equal+=color(q)==color(p);}}
  }
  // A tiny photographic image necessarily has <=128 colors. Protect authored
  // small palettes or flat connected regions, not every small bitmap.
  return colors.size()<=16 || (adjacent && equal*4>=adjacent);
}
inline std::vector<std::uint8_t> Edge2x(std::span<const std::uint8_t> source,unsigned w,unsigned h,unsigned wrapS,unsigned wrapT){
  std::vector<std::uint8_t> output(std::size_t(w)*h*16);
  const auto at=[&](int x,int y){return source.data()+(std::size_t(Wrap(y,h,wrapT))*w+Wrap(x,w,wrapS))*4;};
  const auto equal=[](const std::uint8_t* a,const std::uint8_t* b){return (a[3]==0&&b[3]==0)||std::equal(a,a+4,b);};
  for(unsigned y=0;y<h;++y)for(unsigned x=0;x<w;++x){
    const auto *e=at(x,y),*b=at(x,int(y)-1),*d=at(int(x)-1,y),*f=at(int(x)+1,y),*g=at(x,int(y)+1);
    const std::uint8_t* choices[]={e,e,e,e};
    if(!equal(b,g)&&!equal(d,f)){choices[0]=equal(d,b)?d:e;choices[1]=equal(b,f)?f:e;choices[2]=equal(d,g)?d:e;choices[3]=equal(g,f)?f:e;}
    for(unsigned j=0;j<2;++j)for(unsigned i=0;i<2;++i)std::copy_n(choices[j*2+i],4,output.data()+(std::size_t(y*2+j)*w*2+x*2+i)*4);
  }return output;
}
inline std::optional<Image> EnhanceGraphic(std::span<const std::uint8_t> source,unsigned w,unsigned h,unsigned mips,unsigned scale,unsigned wrapS,unsigned wrapT,std::uint64_t budget){
  if(!w||!h||!mips||mips>std::bit_width(std::max(w,h))||(scale!=2&&scale!=4)||wrapS>2||wrapT>2||MipBytes(w,h,mips)>source.size()||MipBytes(w*scale,h*scale,mips+(scale==4?2:1))>budget)return {};
  auto twice=Edge2x(source,w,h,wrapS,wrapT);Image out{w*scale,h*scale,mips+(scale==4?2u:1u),{}};
  if(scale==4)out.pixels=Edge2x(twice,w*2,h*2,wrapS,wrapT);
  out.pixels.insert(out.pixels.end(),twice.begin(),twice.end());out.pixels.insert(out.pixels.end(),source.begin(),source.begin()+MipBytes(w,h,mips));return out;
}
// Coverage masks (I4/I8 fonts or constant-color transparent glyph atlases).
// Contours are recovered at the 50% coverage crossings, with local edge
// normals; new texels integrate that contour instead of copying source blocks.
struct CoverageStyle {unsigned kind=0;std::array<std::uint8_t,3> color{};};
inline CoverageStyle CoverageEncoding(std::span<const std::uint8_t> source,unsigned w,unsigned h){
  if(!w||!h||source.size()<std::size_t(w)*h*4)return {};
  unsigned low=255,high=0,empty=0,binary=0;bool intensity=true,constant=true,haveColor=false;CoverageStyle style;
  for(std::size_t i=0;i<std::size_t(w)*h;++i){const auto* p=source.data()+i*4;low=std::min(low,unsigned(p[3]));high=std::max(high,unsigned(p[3]));empty+=p[3]<8;binary+=p[3]<=16||p[3]>=239;
    for(unsigned c=0;c<3;++c)intensity&=std::abs(int(p[c])-int(p[3]))<=2;
    if(p[3]>8){if(!haveColor){std::copy_n(p,3,style.color.begin());haveColor=true;}else for(unsigned c=0;c<3;++c)constant&=std::abs(int(p[c])-int(style.color[c]))<=2;}
  }
  if(low>16||high<128||empty<std::uint64_t(w)*h/5||binary*100<std::uint64_t(w)*h*65||(!intensity&&!constant))return {};
  style.kind=intensity?1u:2u;return style;
}
inline std::optional<Image> EnhanceCoverage(std::span<const std::uint8_t> source,unsigned w,unsigned h,unsigned mips,unsigned scale,unsigned wrapS,unsigned wrapT,std::uint64_t budget){
  const auto style=CoverageEncoding(source,w,h);if(!style.kind)return {};
  auto result=EnhanceGraphic(source,w,h,mips,scale,wrapS,wrapT,budget);if(!result)return {};
  // Marching squares gives finite, connected contour segments. A tangent
  // fitted independently at each crossing can overlap its neighbour with the
  // opposite sign, punching holes in thin glyph strokes. Take the sign from
  // the source bilinear field and distance only from these actual segments.
  struct Segment {float ax,ay,bx,by;};
  std::vector<std::vector<Segment>> cells(std::size_t(w)*h);
  const auto alpha=[&](int x,int y){return source[(std::size_t(Wrap(y,h,wrapT))*w+Wrap(x,w,wrapS))*4+3]/255.f;};
  for(unsigned y=0;y<h;++y)for(unsigned x=0;x<w;++x){
    const float a[4]={alpha(x,y),alpha(x+1,y),alpha(x+1,y+1),alpha(x,y+1)};
    const float vx[4]={float(x)+.5f,float(x)+1.5f,float(x)+1.5f,float(x)+.5f};
    const float vy[4]={float(y)+.5f,float(y)+.5f,float(y)+1.5f,float(y)+1.5f};
    std::array<std::array<float,2>,4> crossing{};unsigned n=0;
    for(unsigned k=0;k<4;++k){const auto j=(k+1)%4;if((a[k]<.5f)==(a[j]<.5f))continue;
      const float t=(.5f-a[k])/(a[j]-a[k]);crossing[n++]={vx[k]+t*(vx[j]-vx[k]),vy[k]+t*(vy[j]-vy[k])};}
    auto& segments=cells[std::size_t(y)*w+x];
    const auto add=[&](unsigned a,unsigned b){segments.push_back({crossing[a][0],crossing[a][1],crossing[b][0],crossing[b][1]});};
    if(n==2)add(0,1);
    else if(n==4){
      // The centre decides connectivity of the ambiguous diagonal cell.
      if(((a[0]+a[1]+a[2]+a[3])*.25f>=.5f)==(a[0]>=.5f)){add(0,1);add(2,3);}
      else {add(0,3);add(1,2);}
    }
  }
  for(unsigned y=0;y<h*scale;++y)for(unsigned x=0;x<w*scale;++x){
    const float px=(x+.5f)/scale,py=(y+.5f)/scale;
    const int ix=int(std::floor(px-.5f)),iy=int(std::floor(py-.5f));
    const float tx=px-.5f-ix,ty=py-.5f-iy;
    const float field=(1-ty)*((1-tx)*alpha(ix,iy)+tx*alpha(ix+1,iy))+ty*((1-tx)*alpha(ix,iy+1)+tx*alpha(ix+1,iy+1));
    float nearest=4.f;bool found=false;
    for(int oy=-1;oy<=1;++oy)for(int ox=-1;ox<=1;++ox){
      const int cx=ix+ox,cy=iy+oy;
      for(const auto& e:cells[std::size_t(Wrap(cy,h,wrapT))*w+Wrap(cx,w,wrapS)]){
        const float ax=e.ax+cx-Wrap(cx,w,wrapS),ay=e.ay+cy-Wrap(cy,h,wrapT);
        const float dx=e.bx-e.ax,dy=e.by-e.ay;
        const float t=std::clamp(((px-ax)*dx+(py-ay)*dy)/std::max(1e-12f,dx*dx+dy*dy),0.f,1.f);
        const float rx=px-ax-t*dx,ry=py-ay-t*dy;
        nearest=std::min(nearest,rx*rx+ry*ry);found=true;
      }
    }
    // No contour in this neighbourhood: retain the original coverage field,
    // including faint strokes which never cross 50 percent opacity.
    const float contour=found?std::clamp(.5f+std::copysign(std::sqrt(nearest),field-.5f)*scale,0.f,1.f):field;
    // Antialiased source glyphs encode stroke AREA, not a binary silhouette.
    // Their thin stems may never reach 50% alpha. Preserve this coverage and
    // bound contour correction by its uncertainty; otherwise font stems turn
    // into stencil-like fragments even with topologically correct contours.
    const float uncertainty=field*(1.f-field)*.35f;
    const float value=std::clamp(field+std::clamp(contour-field,-uncertainty,uncertainty),0.f,1.f);
    const auto coverage=std::uint8_t(std::lround(value*255));
    auto* out=result->pixels.data()+(std::size_t(y)*w*scale+x)*4;out[3]=coverage;
    for(unsigned c=0;c<3;++c)out[c]=style.kind==1?coverage:style.color[c];
  }
  if(scale==4){const std::size_t offset=std::size_t(w*scale)*h*scale*4;
    for(unsigned y=0;y<h*2;++y)for(unsigned x=0;x<w*2;++x)for(unsigned c=0;c<4;++c){unsigned total=0;
      for(unsigned j=0;j<2;++j)for(unsigned i=0;i<2;++i)total+=result->pixels[(std::size_t(y*2+j)*w*4+x*2+i)*4+c];
      result->pixels[offset+(std::size_t(y)*w*2+x)*4+c]=std::uint8_t((total+2)/4);}
  }
  return result;
}
// Extrapolate RGB into fully transparent texels for neural context, without
// changing visible RGBA or alpha. This avoids dark colour fringes at cutouts.
inline std::vector<std::uint8_t> OpaqueContext(std::span<const std::uint8_t> source,unsigned w,unsigned h,unsigned wrapS,unsigned wrapT){
  std::vector<std::uint8_t> rgb(source.begin(),source.begin()+std::size_t(w)*h*4);std::vector<unsigned char> known(w*h);
  for(unsigned i=0;i<w*h;++i)known[i]=source[i*4+3]>0;
  for(unsigned step=0;step<8;++step){auto next=rgb;auto valid=known;bool filled=false;
    for(unsigned y=0;y<h;++y)for(unsigned x=0;x<w;++x){auto at=y*w+x;if(known[at])continue;
      unsigned sum[3]={},count=0;for(const auto& d:std::array<std::array<int,2>,4>{{{{-1,0}},{{1,0}},{{0,-1}},{{0,1}}}}){auto n=Wrap(int(y)+d[1],h,wrapT)*w+Wrap(int(x)+d[0],w,wrapS);if(known[n]){++count;for(unsigned c=0;c<3;++c)sum[c]+=rgb[n*4+c];}}
      if(count){for(unsigned c=0;c<3;++c)next[at*4+c]=std::uint8_t((sum[c]+count/2)/count);valid[at]=1;filled=true;}
    }rgb=std::move(next);known=std::move(valid);if(!filled)break;
  }for(unsigned i=0;i<w*h;++i)rgb[i*4+3]=255;return rgb;
}
// Internal-example reconstruction: a half-resolution pyramid supplies paired
// low/high patches from this texture itself. A matched patch contributes its
// measured high-frequency residual, not an invented material or a sharpening
// kernel. Low-confidence matches and transparent/flat/pixel-art regions retain
// the spatial reconstruction. This is a non-neural method.
inline std::vector<std::uint8_t> Reconstruct2x(std::span<const std::uint8_t> source,unsigned w,unsigned h,
    bool high,Encoding encoding,unsigned wrapS,unsigned wrapT,std::uint64_t* detailPixels) {
  std::vector<std::uint8_t> result(std::size_t(w)*h*16);
  Resample(source,w,h,result,w*2,h*2,high,encoding,wrapS,wrapT);
  if(w<16 || h<16 || encoding==Encoding::Normal)return result;
  std::unordered_set<std::uint32_t> colors;
  for(std::size_t i=0;i<std::size_t(w)*h;i+=std::max<std::size_t>(1,std::size_t(w)*h/2048)){
    const auto* p=source.data()+i*4;
    colors.insert(std::uint32_t(p[0])|(std::uint32_t(p[1])<<8)|(std::uint32_t(p[2])<<16));
    if(colors.size()>64)break;
  }
  if(colors.size()<=64)return result; // deliberately preserve limited-palette art
  const unsigned lw=std::max(1u,w/2),lh=std::max(1u,h/2);
  std::vector<std::uint8_t> low(std::size_t(lw)*lh*4),lowUp(std::size_t(w)*h*4);
  for(unsigned y=0;y<lh;++y)for(unsigned x=0;x<lw;++x)for(unsigned c=0;c<4;++c){
    unsigned sum=0;
    for(unsigned j=0;j<2;++j)for(unsigned i=0;i<2;++i)
      sum+=source[(std::size_t(std::min(y*2+j,h-1))*w+std::min(x*2+i,w-1))*4+c];
    low[(std::size_t(y)*lw+x)*4+c]=std::uint8_t((sum+2)/4);
  }
  Resample(low,lw,lh,lowUp,w,h,high,encoding,wrapS,wrapT);
  const auto luma=[](const std::uint8_t* p){return (54.f*p[0]+183.f*p[1]+19.f*p[2])/256.f;};
  std::vector<float> originalY(std::size_t(w)*h),smallY(std::size_t(lw)*lh),residual(std::size_t(w)*h);
  for(std::size_t i=0;i<originalY.size();++i){originalY[i]=luma(source.data()+i*4);residual[i]=originalY[i]-luma(lowUp.data()+i*4);}
  for(std::size_t i=0;i<smallY.size();++i)smallY[i]=luma(low.data()+i*4);
  struct Patch {unsigned x=0,y=0;float mean=0,variance=0;std::array<float,9> values{};};
  const auto patch=[](const std::vector<float>& image,unsigned width,unsigned x,unsigned y){
    Patch p;p.x=x;p.y=y;unsigned n=0;
    for(int j=-1;j<=1;++j)for(int i=-1;i<=1;++i){p.values[n]=image[std::size_t(int(y)+j)*width+int(x)+i];p.mean+=p.values[n++];}
    p.mean/=9;
    for(float& v:p.values){v-=p.mean;p.variance+=v*v;}
    p.variance/=9;return p;
  };
  const auto bucket=[](const Patch& p){
    const float dx=p.values[5]-p.values[3],dy=p.values[7]-p.values[1];
    const unsigned orientation=unsigned((std::atan2(dy,dx)+3.141593f)*(8.f/6.283187f))%8;
    const unsigned contrast=p.variance<25?0:p.variance<100?1:p.variance<400?2:3;
    return orientation+8*contrast;
  };
  std::array<std::vector<Patch>,32> dictionary;
  std::array<unsigned,32> seen{};
  const unsigned candidates=high?32:12;
  for(unsigned y=1;y+1<lh;++y)for(unsigned x=1;x+1<lw;++x){
    const auto p=patch(smallY,lw,x,y);const auto b=bucket(p);const auto count=++seen[b];
    if(dictionary[b].size()<candidates)dictionary[b].push_back(p);
    else {const unsigned pick=(count*2654435761u)%count;if(pick<candidates)dictionary[b][pick]=p;}
  }
  for(unsigned y=1;y+1<h;++y)for(unsigned x=1;x+1<w;++x){
    bool opaque=true;
    for(int j=-1;j<=1;++j)for(int i=-1;i<=1;++i)
      opaque &= source[(std::size_t(int(y)+j)*w+int(x)+i)*4+3]==255;
    if(!opaque)continue;
    const auto query=patch(originalY,w,x,y);
    if(query.variance<16 || query.variance>4096)continue;
    const Patch* best=nullptr;float bestScore=1e30f;
    for(const auto& candidate:dictionary[bucket(query)]){
      if(candidate.variance<query.variance*.5f || candidate.variance>query.variance*2)continue;
      float score=0;for(unsigned k=0;k<9;++k){const float d=query.values[k]-candidate.values[k];score+=d*d;}
      score/=9;if(score<bestScore){bestScore=score;best=&candidate;}
    }
    const float tolerance=4+query.variance*.15f;
    if(!best || bestScore>=tolerance)continue;
    const float confidence=(1-bestScore/tolerance)*.65f;
    const float limit=std::min(20.f,std::sqrt(query.variance)*.5f);
    std::array<float,4> detail{};float average=0;
    for(unsigned j=0;j<2;++j)for(unsigned i=0;i<2;++i){
      const auto at=std::size_t(std::min(best->y*2+j,h-1))*w+std::min(best->x*2+i,w-1);
      detail[j*2+i]=std::clamp(residual[at],-limit,limit)*confidence;average+=detail[j*2+i]*.25f;
    }
    for(unsigned j=0;j<2;++j)for(unsigned i=0;i<2;++i){
      const auto at=(std::size_t(y*2+j)*(w*2)+x*2+i)*4;
      const float d=detail[j*2+i]-average;bool changed=false;
      for(unsigned c=0;c<3;++c){
        const auto value=std::uint8_t(std::lround(std::clamp(float(result[at+c])+d,0.f,255.f)));
        changed |= std::abs(int(value)-int(result[at+c]))>1;result[at+c]=value;
      }
      if(changed && detailPixels)++*detailPixels;
    }
  }
  return result;
}
// Authored GX mip levels remain byte-exact; only the new higher-resolution
// levels are reconstructed. This preserves unusual hand-authored mip chains.
inline std::optional<Image> Enhance(std::span<const std::uint8_t> source,unsigned w,unsigned h,
                                    unsigned mips,unsigned scale,bool high,Encoding encoding,
                                    unsigned wrapS,unsigned wrapT,std::uint64_t budget,
                                    std::uint64_t* detailPixels=nullptr) {
  if(!w || !h || !mips || mips>std::bit_width(std::max(w,h)) || (scale!=2 && scale!=4) || w>8192/scale || h>8192/scale ||
     wrapS>2 || wrapT>2 || MipBytes(w,h,mips)>source.size())return {};
  const unsigned extra=scale==4?2:1;
  const auto bytes=MipBytes(w*scale,h*scale,mips+extra);
  if(bytes>budget)return {};
  Image result{w*scale,h*scale,mips+extra,std::vector<std::uint8_t>(bytes)};
  std::size_t offset=0;
  const auto twice=Reconstruct2x(source,w,h,high,encoding,wrapS,wrapT,detailPixels);
  if(scale==4){
    const auto four=Reconstruct2x(twice,w*2,h*2,high,encoding,wrapS,wrapT,detailPixels);
    std::copy(four.begin(),four.end(),result.pixels.begin());offset+=four.size();
  }
  std::copy(twice.begin(),twice.end(),result.pixels.begin()+offset);offset+=twice.size();
  std::copy_n(source.begin(),std::size_t(MipBytes(w,h,mips)),result.pixels.begin()+offset);
  return result;
}
} // namespace GekkoAOT::GX::HD
