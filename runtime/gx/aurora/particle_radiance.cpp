// SPDX-License-Identifier: GPL-3.0-or-later
#include "gekkoaot_particle_radiance.hpp"
#include "../gx/gx.hpp"
#include "../gx/texture.hpp"
#include <algorithm>
#include <cmath>
namespace aurora::gfx::enhanced {
using Color=std::array<float,4>;
void capture_particle_texture(TextureRef& texture,ArrayRef<uint8_t> data) noexcept {
  texture.particleRadianceKnown=false;
  unsigned channels=texture.format==wgpu::TextureFormat::RGBA8Unorm?4:
    texture.format==wgpu::TextureFormat::RG8Unorm?2:texture.format==wgpu::TextureFormat::R8Unorm?1:0;
  const auto width=texture.size.width,height=texture.size.height;
  if(!channels || !width || !height || data.size()<uint64_t(width)*height*channels)return;
  // Keep the brightest covered samples, rather than diluting a flame with
  // its large transparent border. Original GX intensity swizzles are kept.
  Color sum{};float total=0;
  for(unsigned y=0;y<16;++y)for(unsigned x=0;x<16;++x){
    const unsigned px=std::min(width-1,(2*x+1)*width/32),py=std::min(height-1,(2*y+1)*height/32);
    const auto* p=data.data()+(py*width+px)*channels;
    Color c=channels==4?Color{p[0]/255.f,p[1]/255.f,p[2]/255.f,p[3]/255.f}:
      Color{p[0]/255.f,p[0]/255.f,p[0]/255.f,p[channels==2?1:0]/255.f};
    const float luma=c[0]*.299f+c[1]*.587f+c[2]*.114f;
    const float score=luma*luma*c[3];
    for(unsigned k=0;k<4;++k)sum[k]+=c[k]*score;
    total+=score;
  }
  if(total>0)for(auto& c:sum)c/=total;
  texture.particleRadiance=sum;texture.particleRadianceKnown=true;
}
ParticleMaterial particle_material(){
  ParticleMaterial result;const auto& s=gx::g_gxState;
  std::array<bool,8> used{};
  for(unsigned i=0;i<s.numTevStages;++i){const auto& stage=s.tevStages[i];
    const auto tex=[](GXTevColorArg arg){return arg==GX_CC_TEXC || arg==GX_CC_TEXA;};
    if(!(tex(stage.colorPass.a)||tex(stage.colorPass.b)||tex(stage.colorPass.c)||tex(stage.colorPass.d)||
       stage.alphaPass.a==GX_CA_TEXA||stage.alphaPass.b==GX_CA_TEXA||stage.alphaPass.c==GX_CA_TEXA||stage.alphaPass.d==GX_CA_TEXA))continue;
    const auto map=unsigned(stage.texMapId);if(map>=8 || used[map])continue;used[map]=true;
    const auto& obj=s.loadedTextures[map];if(!obj.data){result.known=false;continue;}
    TextureHandle texture;
    if(auto it=s.copyTextures.find(obj.data);it!=s.copyTextures.end())texture=it->second.handle;
    else if(gx::is_palette_format(obj.format())){
      if(unsigned(obj.tlut)>=s.loadedTluts.size()){result.known=false;continue;}
      texture=gx::texture::cached_particle_texture(obj,&s.loadedTluts[unsigned(obj.tlut)]);
    }else texture=gx::texture::cached_particle_texture(obj);
    if(!texture || !texture->particleRadianceKnown){result.known=false;continue;}
    result.textures[map]=texture->particleRadiance;
  }
  return result;
}
std::array<float,4> particle_radiance(const ParticleMaterial& source,const Color& vertexColor){
  const auto& s=gx::g_gxState;if(!source.known)return {};
  std::array<Color,4> regs{};
  for(unsigned r=0;r<4;++r)for(unsigned c=0;c<4;++c)regs[r][c]=s.colorRegs[r][c];
  const auto scalar=[](float f){return Color{f,f,f,f};};
  const auto konst=[&](unsigned selector,bool alpha){
    if(selector<8)return scalar(1.f-float(selector)*.125f);
    const unsigned index=selector&3u;Color value{};for(unsigned c=0;c<4;++c)value[c]=s.kcolors[index][c];
    if(selector>=16)return scalar(value[(selector-16)/4]);
    return alpha?Color{}:value;
  };
  const auto swap=[](Color c,const gx::TevSwap& s){return Color{c[unsigned(s.red)],c[unsigned(s.green)],c[unsigned(s.blue)],c[unsigned(s.alpha)]};};
  const auto operation=[](float a,float b,float c,float d,const gx::TevOp& op){
    float value=d+(op.op==GX_TEV_SUB?-1.f:1.f)*(a*(1-c)+b*c);
    value+=op.bias==GX_TB_ADDHALF?.5f:op.bias==GX_TB_SUBHALF?-.5f:0.f;
    value*=op.scale==GX_CS_SCALE_2?2.f:op.scale==GX_CS_SCALE_4?4.f:op.scale==GX_CS_DIVIDE_2?.5f:1.f;
    return op.clamp?std::clamp(value,0.f,1.f):std::clamp(value,-4.f,4.f);
  };
  for(unsigned i=0;i<s.numTevStages;++i){const auto& stage=s.tevStages[i];
    // Unsupported comparison/indirect programs keep the GPU observation path.
    if((stage.colorOp.op!=GX_TEV_ADD && stage.colorOp.op!=GX_TEV_SUB) ||
       (stage.alphaOp.op!=GX_TEV_ADD && stage.alphaOp.op!=GX_TEV_SUB) || stage.indTexMtxId!=GX_ITM_OFF)return {};
    Color tex{1,1,1,1};if(unsigned(stage.texMapId)<8)tex=source.textures[unsigned(stage.texMapId)];
    const unsigned channel=stage.channelId==GX_COLOR1 || stage.channelId==GX_COLOR1A1 || stage.channelId==GX_ALPHA1?1u:0u;
    if(s.colorChannelConfig[channel].lightingEnabled)return {};
    Color ras=vertexColor;
    if(s.colorChannelConfig[channel].matSrc==GX_SRC_REG)for(unsigned c=0;c<3;++c)ras[c]=s.colorChannelState[channel].matColor[c];
    if(s.colorChannelConfig[channel+2].matSrc==GX_SRC_REG)ras[3]=s.colorChannelState[channel].matColor[3];
    if(stage.channelId==GX_COLOR_NULL)ras={};
    tex=swap(tex,s.tevSwapTable[unsigned(stage.tevSwapTex)]);ras=swap(ras,s.tevSwapTable[unsigned(stage.tevSwapRas)]);
    const auto kc=konst(unsigned(stage.kcSel),false),ka=konst(unsigned(stage.kaSel),true);
    const auto color=[&](GXTevColorArg arg){const unsigned a=unsigned(arg);
      if(a<8)return (a&1u)?scalar(regs[a/2][3]):regs[a/2];
      switch(arg){case GX_CC_TEXC:return tex;case GX_CC_TEXA:return scalar(tex[3]);case GX_CC_RASC:return ras;
        case GX_CC_RASA:return scalar(ras[3]);case GX_CC_ONE:return scalar(1);case GX_CC_HALF:return scalar(.5f);
        case GX_CC_KONST:return kc;default:return Color{};}
    };
    const auto alpha=[&](GXTevAlphaArg arg){const unsigned a=unsigned(arg);if(a<4)return regs[a][3];
      switch(arg){case GX_CA_TEXA:return tex[3];case GX_CA_RASA:return ras[3];case GX_CA_KONST:return ka[3];default:return 0.f;}
    };
    const auto a=color(stage.colorPass.a),b=color(stage.colorPass.b),c=color(stage.colorPass.c),d=color(stage.colorPass.d);
    const float aa=alpha(stage.alphaPass.a),ab=alpha(stage.alphaPass.b),ac=alpha(stage.alphaPass.c),ad=alpha(stage.alphaPass.d);
    for(unsigned k=0;k<3;++k)regs[unsigned(stage.colorOp.outReg)][k]=operation(a[k],b[k],c[k],d[k],stage.colorOp);
    regs[unsigned(stage.alphaOp.outReg)][3]=operation(aa,ab,ac,ad,stage.alphaOp);
  }
  auto color=regs[0];
  for(auto& c:color)c=std::clamp(c,0.f,1.f);
  const bool alphaOver=s.blendFacDst==GX_BL_INVSRCALPHA;
  const bool hot=(color[0]>.25f && color[1]>.055f && color[0]>=color[1]*.85f && color[1]>color[2]*1.35f) ||
    (color[2]>.65f && color[2]>color[0]*1.4f && color[2]>color[1]*1.12f);
  if(alphaOver && !hot)return {0,0,0,1};
  const float alpha=s.blendFacSrc==GX_BL_SRCALPHA?color[3]:1.f;
  return {color[0]*alpha,color[1]*alpha,color[2]*alpha,1};
}
}
