// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
namespace aurora::gfx::hd {
inline constexpr char DynamicTextureShader[]=R"WGSL(
fn hd_premul(v:vec4f)->vec4f{return vec4f(v.rgb*v.a,v.a);}
fn hd_luma(v:vec4f)->f32{return dot(v.rgb,vec3f(.299,.587,.114));}
// Twelve-tap oriented reconstruction of the current frame. Negative lobes
// restore edge shape; a local color/alpha envelope prevents ringing. This is
// spatial GPU reconstruction, not neural inference or frame interpolation.
fn hd_edge_reconstruct(t:texture_2d<f32>,s:sampler,uv:vec2f,base:vec4f)->vec4f{
 let size=vec2f(textureDimensions(t,0));let pos=uv*size-.5;let cell=floor(pos);let f=fract(pos);
 let a=hd_premul(textureSampleLevel(t,s,(cell+vec2f(.5,.5))/size,0.));
 let b=hd_premul(textureSampleLevel(t,s,(cell+vec2f(1.5,.5))/size,0.));
 let c=hd_premul(textureSampleLevel(t,s,(cell+vec2f(.5,1.5))/size,0.));
 let d=hd_premul(textureSampleLevel(t,s,(cell+vec2f(1.5,1.5))/size,0.));
 let gradient=vec2f(hd_luma(b+d-a-c),hd_luma(c+d-a-b));
 let magnitude=length(gradient);
 let alphaGradient=vec2f((b+d-a-c).a,(c+d-a-b).a);
 let edge=gradient+alphaGradient;
 let energy=dot(edge,edge);
 if(energy<.000001){return base;}
 let normal=edge*inverseSqrt(energy);let tangent=vec2f(-normal.y,normal.x);
 let confidence=clamp(sqrt(energy)*2.,0.,1.);
 let positions=array<vec2f,12>(vec2f(0.,-1.),vec2f(1.,-1.),vec2f(-1.,0.),vec2f(0.,0.),vec2f(1.,0.),vec2f(2.,0.),
   vec2f(-1.,1.),vec2f(0.,1.),vec2f(1.,1.),vec2f(2.,1.),vec2f(0.,2.),vec2f(1.,2.));
 var accum=vec4f(0.);var total=0.;
 for(var i=0u;i<12u;i++){
  let delta=positions[i]-f;
  let u=dot(delta,tangent);let v=dot(delta,normal);
  let q=min(2.,u*u*mix(1.,.5,confidence)+v*v*mix(1.,1.5,confidence));
  let lobe=.5*q-1.;let window=.4*q-1.;
  let weight=lobe*lobe*(1.5625*window*window-.5625);
  let sampleValue=hd_premul(textureSampleLevel(t,s,(cell+positions[i]+.5)/size,0.));
  accum+=sampleValue*weight;total+=weight;
 }
 let lower=min(min(a,b),min(c,d));let upper=max(max(a,b),max(c,d));
 let reconstructed=clamp(accum/max(total,.00001),lower,upper);
 let result=mix(hd_premul(base),reconstructed,.85);
 return vec4f(result.rgb/max(result.a,.00001),result.a);
}
fn hd_dynamic_sample_grad(t:texture_2d<f32>,s:sampler,uv:vec2f,dx:vec2f,dy:vec2f,enabled:f32)->vec4f{
 let base=textureSampleGrad(t,s,uv,dx,dy);let size=vec2f(textureDimensions(t,0));
 if(enabled<1. || max(length(dx*size),length(dy*size))>=1. || min(size.x,size.y)<4.){return base;}
 return hd_edge_reconstruct(t,s,uv,base);
}
fn hd_dynamic_sample(t:texture_2d<f32>,s:sampler,uv:vec2f,bias:f32,enabled:f32)->vec4f{
 return hd_dynamic_sample_grad(t,s,uv,dpdx(uv)*exp2(bias),dpdy(uv)*exp2(bias),enabled);
}
)WGSL";
}
