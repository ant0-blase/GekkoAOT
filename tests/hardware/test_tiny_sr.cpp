// SPDX-License-Identifier: GPL-3.0-or-later
#include "../../runtime/gx/tiny_sr.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
using namespace GekkoAOT::GX;
static void Check(bool condition,const char* text){if(!condition){std::fprintf(stderr,"FAIL: %s\n",text);std::exit(1);}}
int main(){
  std::vector<float> input(64);for(unsigned i=0;i<64;++i)input[i]=float((i*17+3)%101)/100;
  // Independent NumPy HWIO convolution of the pinned upstream constants,
  // SAME zero padding, ReLU, subpixel channels and tanh. No framework needed.
  constexpr unsigned indices[]={0,1,3,4,17,99,255};
  constexpr float expected2[]={.0541735217f,.0728149414f,.0599290803f,.128785834f,.711671591f,.141784728f,.629666567f};
  constexpr float expected4[]={.120615117f,.110254183f,.143739685f,.090227142f,.207066655f,.226117194f,.663443565f};
  const auto twice=TinySR::Infer<2>(input,8,8),four=TinySR::Infer<4>(input,8,8);
  for(unsigned i=0;i<7;++i){Check(std::abs(twice[indices[i]]-expected2[i])<.00003f,"2x matches independent model inference");Check(std::abs(four[indices[i]]-expected4[i])<.00003f,"4x matches independent model inference");}
  constexpr unsigned w=32,h=32,mips=6;
  std::vector<std::uint8_t> texture(HD::MipBytes(w,h,mips),73);
  for(unsigned y=0;y<h;++y)for(unsigned x=0;x<w;++x){const auto i=(y*w+x)*4;texture[i]=80+(x*9+y*3)%70;texture[i+1]=70+(x*7+y*5)%80;texture[i+2]=60+(x*3+y*13)%90;texture[i+3]=255;}
  const auto original=texture;const auto start=std::chrono::steady_clock::now();
  std::uint64_t changed=0;
  auto image=TinySR::Enhance(texture,w,h,mips,2,1,1,16<<20,&changed);
  Check(image && image->width==64 && image->height==64 && image->mips==7,"native pretrained model produces a 2x GPU image");
  Check(changed>500,"learned reconstruction differs from the spatial resize at actual pixels");
  Check(std::memcmp(image->pixels.data()+64*64*4,texture.data(),texture.size())==0,"authored mip levels preserved byte-exact");
  for(unsigned i=0;i<64*64;++i)Check(image->pixels[i*4+3]==255,"opaque alpha stays exact");
  image=TinySR::Enhance(texture,w,h,mips,4,2,0,16<<20);
  Check(image&&image->width==128&&image->mips==8,"4x model and mirror/clamp context supported");
  Check(std::memcmp(image->pixels.data()+128*128*4+64*64*4,texture.data(),texture.size())==0,"4x retains every authored mip");
  Check(texture==original,"source texels never change");
  texture[3]=254;Check(!TinySR::Enhance(texture,w,h,mips,2,0,0,16<<20),"alpha texture selects explicit non-AI fallback");
  texture=original;for(unsigned i=0;i<w*h;++i)texture[i*4]=texture[i*4+1]=texture[i*4+2]=(i%4)*70;
  Check(!TinySR::Eligible(texture,w,h),"limited palette art is protected");
  Check(!TinySR::Enhance(original,w,h,mips,2,0,0,1),"allocation budget enforced");
  std::vector<std::uint8_t> glyph(16*16*4);
  for(unsigned y=0;y<16;++y)for(unsigned x=0;x<16;++x)for(unsigned c=0;c<4;++c)glyph[(y*16+x)*4+c]=(x==y||x==y+1||x==4)?255:0;
  auto contour=HD::EnhanceCoverage(glyph,16,16,1,4,0,0,1<<20);
  auto blocks=HD::EnhanceGraphic(glyph,16,16,1,4,0,0,1<<20);
  Check(contour && blocks && contour->pixels!=blocks->pixels,"font contours differ from copied pixel blocks");
  Check(std::memcmp(contour->pixels.data()+64*64*4+32*32*4,glyph.data(),glyph.size())==0,"glyph authored mip stays immutable");
  unsigned antialiased=0;for(unsigned i=0;i<64*64;++i)antialiased+=contour->pixels[i*4+3]>0&&contour->pixels[i*4+3]<255;
  Check(antialiased>50,"new subpixel coverage smooths glyph contours");
  // A solid one-source-pixel vertical stroke must not develop transparent
  // pinholes where independently estimated crossing tangents used to meet.
  for(unsigned y=2;y<14;++y)for(unsigned sy=0;sy<4;++sy)
    for(unsigned sx=1;sx<3;++sx)
      Check(contour->pixels[((y*4+sy)*64+4*4+sx)*4+3]>200,"solid glyph stroke remains connected");
  // Low-opacity serifs must survive next to full-opacity strokes. A global
  // 50% contour threshold used to erase these even though GX blends them.
  auto serif=glyph;
  for(unsigned y=2;y<14;++y)for(unsigned c=0;c<4;++c)serif[(y*16+11)*4+c]=102;
  auto fine=HD::EnhanceCoverage(serif,16,16,1,4,0,0,1<<20);
  Check(bool(fine),"antialiased font admitted");
  for(unsigned y=2;y<14;++y)
    Check(fine->pixels[((y*4+2)*64+11*4+2)*4+3]>40,"low-coverage serif is never erased");
  std::vector<std::uint8_t> photograph(16*16*4);for(unsigned i=0;i<256;++i)for(unsigned c=0;c<4;++c)photograph[i*4+c]=i;
  Check(!HD::CoverageEncoding(photograph,16,16).kind,"continuous-tone images are not mistaken for text");

  auto high=TinySR::Enhance(original,w,h,mips,2,1,1,16<<20,nullptr,true);
  auto balanced=TinySR::Enhance(original,w,h,mips,2,1,1,16<<20);
  const auto degradationError=[&](const HD::Image& output){
    double error=0;
    for(unsigned y=0;y<h;++y)for(unsigned x=0;x<w;++x){
      float mean=0;
      for(unsigned j=0;j<2;++j)for(unsigned i=0;i<2;++i){auto p=output.pixels.data()+((y*2+j)*w*2+x*2+i)*4;mean+=(.299f*p[0]+.587f*p[1]+.114f*p[2])*.25f;}
      auto p=original.data()+(y*w+x)*4;float delta=mean-(.299f*p[0]+.587f*p[1]+.114f*p[2]);error+=delta*delta;
    }
    return error/(w*h);
  };
  Check(high&&balanced,"both quality profiles reconstruct");
  const double before=degradationError(*balanced),after=degradationError(*high);
  Check(after<before*.3,"high quality restores authored source structure under downprojection");
  Check(high->pixels!=balanced->pixels,"neural quality control changes reconstruction");
  auto highFour=TinySR::Enhance(original,w,h,mips,4,1,1,16<<20,nullptr,true);
  Check(bool(highFour),"high quality 4x reconstructs");
  Check(std::memcmp(highFour->pixels.data()+128*128*4,high->pixels.data(),64*64*4)==0,"4x intermediate mip uses independently reconstructed 2x image");
  Check(std::memcmp(highFour->pixels.data()+128*128*4+64*64*4,original.data(),original.size())==0,"high quality preserves all authored mips");
  std::printf("NATIVE_SR_SOURCE_CONSISTENCY mse_before=%.3f mse_after=%.3f\n",before,after);
  // Compare tiled reconstruction against an independent whole-image forward
  // pass on both sides of the 128-source-texel tile boundary.
  constexpr unsigned wide=140,shortSide=16,pw=wide+8,ph=shortSide+8;
  std::vector<std::uint8_t> strip(wide*shortSide*4);
  for(unsigned i=0;i<wide*shortSide;++i){strip[i*4]=50+i%170;strip[i*4+1]=40+(i*7)%180;strip[i*4+2]=60+(i*13)%150;strip[i*4+3]=255;}
  auto tiled=TinySR::Enhance(strip,wide,shortSide,1,2,1,1,16<<20);
  Check(bool(tiled),"tiled wide texture reconstructs");
  std::vector<float> context(pw*ph);
  for(unsigned y=0;y<ph;++y)for(unsigned x=0;x<pw;++x){auto i=(HD::Wrap(int(y)-4,shortSide,1)*wide+HD::Wrap(int(x)-4,wide,1))*4;context[y*pw+x]=(.299f*strip[i]+.587f*strip[i+1]+.114f*strip[i+2])/255.f;}
  auto whole=TinySR::Infer<2>(context,pw,ph);
  std::vector<std::uint8_t> spatial(wide*shortSide*16);
  HD::Resample(strip,wide,shortSide,spatial,wide*2,shortSide*2,true,HD::Encoding::Linear,1,1);
  for(unsigned y=0;y<shortSide*2;++y)for(unsigned x=250;x<266;++x){
    auto at=(y*wide*2+x)*4;float predicted=std::clamp(whole[((y/2+4)*pw+x/2+4)*4+(y%2)*2+x%2],0.f,1.f);
    float base=(.299f*spatial[at]+.587f*spatial[at+1]+.114f*spatial[at+2])/255.f;
    float lo=std::min({spatial[at],spatial[at+1],spatial[at+2]}),hi=std::max({spatial[at],spatial[at+1],spatial[at+2]});
    float delta=std::clamp(std::clamp(predicted-base,-.2f,.2f)*255,-lo,255-hi);
    for(unsigned c=0;c<3;++c)Check(std::abs(int(tiled->pixels[at+c])-int(std::lround(spatial[at+c]+delta)))<=1,"tile borders match whole-image network");
  }
  std::printf("NATIVE_ESPCN_PASSED parameters=21284/24752 changed_pixels=%llu cpu_ms=%.3f dependencies=none\n",(unsigned long long)changed,std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count());
  auto transparent=original;for(unsigned i=0;i<w*h;++i)transparent[i*4+3]=(i%7)?180:0;
  auto translucent=TinySR::Enhance(transparent,w,h,mips,2,1,1,16<<20,nullptr,false,true);
  Check(bool(translucent),"2D mode reconstructs textured transparent RGBA");
  std::vector<std::uint8_t> alphaReference(w*h*16);
  HD::Resample(transparent,w,h,alphaReference,w*2,h*2,true,HD::Encoding::Linear,1,1);
  for(unsigned i=0;i<w*h*4;++i)Check(translucent->pixels[i*4+3]==alphaReference[i*4+3],"neural colour reconstruction preserves filtered alpha exactly");
  Check(std::memcmp(translucent->pixels.data()+64*64*4,transparent.data(),transparent.size())==0,"transparent authored mip remains byte exact");
  std::vector<std::uint8_t> font(16*16*4);
  for(unsigned y=0;y<16;++y)for(unsigned x=0;x<16;++x){auto p=font.data()+(y*16+x)*4;p[0]=p[1]=p[2]=255;p[3]=(x==y||x==4||x==11||y==8)?255:0;}
  Check(HD::LimitedPalette(font,16,16),"font and flat art classified separately from photo texture");
  auto fontHD=HD::EnhanceGraphic(font,16,16,1,4,0,0,16<<20);
  Check(fontHD&&fontHD->width==64&&fontHD->mips==3,"font atlas reconstructs at 4x");
  for(unsigned i=0;i<64*64;++i)Check((fontHD->pixels[i*4+3]==0||fontHD->pixels[i*4+3]==255)&&fontHD->pixels[i*4]==255,"font palette/alpha remain exact");
  Check(std::memcmp(fontHD->pixels.data()+64*64*4+32*32*4,font.data(),font.size())==0,"original font authored mip remains intact");

  auto authored=original;
  auto enhancedMips=TinySR::Enhance(authored,w,h,mips,4,1,1,16<<20);
  Check(bool(enhancedMips),"HD mip appearance fixture");
  const auto unchangedMips=enhancedMips->pixels;
  HD::ReconstructMipAppearance(*enhancedMips,authored,w,h,mips,4);
  const unsigned baseOffset=128*128*4+64*64*4;
  Check(std::memcmp(enhancedMips->pixels.data()+baseOffset,unchangedMips.data()+baseOffset,w*h*4)!=0,"reconstructed appearance remains at the authored base-resolution mip");
  for(unsigned i=0;i<w*h;++i)Check(enhancedMips->pixels[baseOffset+i*4+3]==authored[i*4+3],"HD mip reconstruction preserves authored alpha");
  Check(std::memcmp(enhancedMips->pixels.data()+baseOffset+w*h*4,authored.data()+w*h*4,authored.size()-w*h*4)==0,"deliberately different authored mip artwork is retained");

}
