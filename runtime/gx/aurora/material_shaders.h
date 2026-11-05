// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
namespace aurora::gfx::hd {
inline constexpr char MaterialShader[] = R"WGSL(
@group(2) @binding(16) var material_map: texture_2d<f32>;
struct MaterialFrame { t:vec3f,b:vec3f,n:vec3f,metric:vec2f };
fn material_frame(position:vec3f,uv:vec2f,smooth_normal:vec3f)->MaterialFrame {
  let dx=dpdx(position);let dy=dpdy(position);
  let ux=dpdx(uv);let uy=dpdy(uv);
  let determinant=ux.x*uy.y-ux.y*uy.x;
  let geometric=cross(dy,dx);let area=dot(geometric,geometric);
  if(abs(determinant)<1e-10 || area<1e-20){
    return MaterialFrame(vec3f(1.,0.,0.),vec3f(0.,1.,0.),vec3f(0.,0.,1.),vec2f(0.));
  }
  var n=geometric*inverseSqrt(area);if(dot(n,position)>0.){n=-n;}
  if(dot(smooth_normal,smooth_normal)>.25){let s=normalize(smooth_normal);if(dot(s,n)>.3){n=s;}}
  let raw_t=(dx*uy.y-dy*ux.y)/determinant;
  let raw_b=(dy*ux.x-dx*uy.x)/determinant;
  let projected=raw_t-n*dot(raw_t,n);
  let t=projected*inverseSqrt(max(dot(projected,projected),1e-20));
  let handed=select(-1.,1.,dot(cross(n,t),raw_b)>=0.);
  return MaterialFrame(t,cross(n,t)*handed,n,vec2f(length(raw_t),length(raw_b)));
}
fn material_edge(uv:vec2f)->vec2f {
  let flags=u32(max(0.,ubuf.material_params.w-1.));
  let half_texel=.5/vec2f(textureDimensions(material_map));
  return vec2f(select(uv.x,clamp(uv.x,half_texel.x,1.-half_texel.x),(flags&3u)==0u),
               select(uv.y,clamp(uv.y,half_texel.y,1.-half_texel.y),((flags>>2u)&3u)==0u));
}
fn material_parallax(uv:vec2f,dx:vec2f,dy:vec2f,position:vec3f,frame:MaterialFrame)->vec2f {
  if(ubuf.material_params.y<=0. || ubuf.material_params.z<1. || any(frame.metric<vec2f(1e-6))){return uv;}
  let v=normalize(-position);
  let view=vec3f(dot(v,frame.t),dot(v,frame.b),dot(v,frame.n));
  let size=vec2f(textureDimensions(material_map));
  let footprint=max(length(dx*size),length(dy*size));
  let lod=1.-smoothstep(1.,4.,footprint);
  let grazing=smoothstep(.15,.35,view.z);
  // Wide ray steps near silhouettes are unstable; LOD and grazing guards
  // reduce relief explicitly instead of letting offsets grow without bound.
  let world_height=ubuf.material_params.y*min(frame.metric.x,frame.metric.y)*lod*grazing;
  if(world_height<=1e-7){return uv;}
  let direction=view.xy/frame.metric/max(view.z,.2)*world_height;
  let maximum=select(12.,24.,ubuf.material_params.z>1.);
  let count=i32(round(mix(maximum,4.,clamp(view.z,0.,1.))));
  let step=1./f32(count);
  var current=uv;var previous=uv;var depth=0.;var before=0.;var after=0.;
  for(var i=0;i<24;i++){
    let height=1.-textureSampleGrad(material_map,tex0_samp,material_edge(current),dx,dy).a;
    after=depth-height;
    if(after>=0. || i>=count){break;}
    before=after;previous=current;depth+=step;current-=direction*step;
  }
  let ratio=clamp(after/max(after-before,1e-5),0.,1.);
  return material_edge(mix(current,previous,ratio));
}
// The surface's own zero-mean structure at two finer scales. Blurry console
// textures stop looking blurry up close, and the mips of the same map fade it
// out with distance, so it never shimmers.
fn material_micro_detail(uv:vec2f,dx:vec2f,dy:vec2f)->vec2f {
  if(ubuf.material_detail.x<=0.){return vec2f(0.);}
  let a=textureSampleGrad(material_map,tex0_samp,fract(uv*5.),dx*5.,dy*5.).xy-.5;
  let b=textureSampleGrad(material_map,tex0_samp,fract(uv*13.+.37),dx*13.,dy*13.).xy-.5;
  return (a+b*.7)*ubuf.material_detail.x;
}
fn material_surface(texel:vec4f,uv:vec2f,frame:MaterialFrame,micro:vec2f)->vec3f {
  var tangent=(texel.xy*255.-128.)/127.+micro*2.;
  let flags=u32(max(0.,ubuf.material_params.w-1.));
  if((flags&3u)==2u && (i32(floor(uv.x))&1)!=0){tangent.x=-tangent.x;}
  if(((flags>>2u)&3u)==2u && (i32(floor(uv.y))&1)!=0){tangent.y=-tangent.y;}
  tangent*=abs(ubuf.material_params.x);
  let local=normalize(vec3f(tangent,sqrt(max(.01,1.-dot(tangent,tangent)))));
  return normalize(frame.t*local.x+frame.b*local.y+frame.n*local.z);
}
)WGSL";
}
