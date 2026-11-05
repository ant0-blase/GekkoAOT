// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
namespace aurora::gfx::enhanced {
inline constexpr char Shader[] = R"WGSL(
override gekkoaot_dof_enabled:bool=false;
override gekkoaot_particle_enabled:bool=false;
struct ViewParams { projection:mat4x4f,inverse:mat4x4f,viewport:vec4f };
struct Params { views:array<ViewParams,15>, depth: vec4f,
                effects: vec4f, light: vec4f, quality: vec4f, work: vec4f, copy_rect: vec4f,
                env_sun: vec4f, env_sun_color: vec4f, env_sky: vec4f, env_ground: vec4f, env_up: vec4f,
                csm: array<mat4x4f,2>, csm_params: array<vec4f,2>, csm_info: vec4f,
                gi: vec4f, ssr: vec4f, dof: vec4f, emission:vec4f, emission_style:vec4f,
                emitters:array<vec4f,64>, emitter_views:array<vec4f,64>, emitter_colors:array<vec4f,64> };
@group(0) @binding(0) var<uniform> p: Params;
@group(0) @binding(1) var color: texture_2d<f32>;
@group(0) @binding(2) var normals: texture_2d<f32>;
@group(0) @binding(4) var depths: texture_2d<f32>;
@group(0) @binding(5) var occlusion: texture_2d<f32>;
@group(0) @binding(6) var contribution: texture_2d<f32>;
@group(0) @binding(7) var cascades: texture_depth_2d;
@group(0) @binding(8) var indirect: texture_2d<f32>;
@group(0) @binding(9) var reflections: texture_2d<f32>;
@group(0) @binding(10) var<storage,read> particle_sources: array<vec4f,16>;
struct ParticleHistory { energy:vec4f, key:u32, frame:u32, padding:vec2u };
@group(0) @binding(11) var<storage,read_write> particle_history: array<ParticleHistory,256>;
@group(0) @binding(12) var<storage,read_write> resolved_particles: array<vec4f,16>;
var<workgroup> particle_energy:array<vec3f,64>;
var<workgroup> particle_raw_energy:array<vec3f,64>;
// Three exact 11/11/10-bit integers fit binary16 without rounding. The fourth
// channel controls metadata replacement; fractional transparency retains the
// opaque receiver instead of averaging unrelated cameras or depths.
fn decode_capture(v:vec4f)->f32 {
  return bitcast<f32>(u32(v.x)|(u32(v.y)<<11u)|(u32(v.z)<<22u));
}
@vertex fn vs(@builtin(vertex_index) id: u32) -> @builtin(position) vec4f {
  var xy=array<vec2f,3>(vec2f(-1.,-1.),vec2f(3.,-1.),vec2f(-1.,3.));return vec4f(xy[id],0.,1.);
}
fn packed_at(q:vec2i)->f32 {
  if(any(q<vec2i(0)) || any(q>=vec2i(p.depth.zw))){return 0.;}return textureLoad(depths,q,0).x;
}
fn camera_at(q:vec2i)->u32{return bitcast<u32>(packed_at(q))&15u;}
// GX games use different world units. Derive the effect length unit from the
// captured lens instead of making a preset's radius work only on close weapons.
fn camera_scale(id:u32)->f32 {
  let near_point=vec4f(0.,0.,1.,1.)*p.views[id-1u].inverse;
  return max(.000001,abs(near_point.z/near_point.w)*4.);
}
fn inside(q: vec2i) -> bool {
  let id=camera_at(q);if(id==0u){return false;}let v=p.views[id-1u].viewport;
  let f=vec2f(q)+.5;return all(f>=v.xy)&&all(f<v.xy+v.zw);
}
// Depth of a packed texel whose camera is `id` (0: other camera, outside its
// viewport, or empty). One load: the effect loops call this per tap, and the
// old camera/viewport/depth helpers fetched the same texel three times.
fn depth_of(q:vec2i,bits:u32,id:u32)->f32 {
  if(id==0u || (bits&15u)!=id){return 0.;}
  let v=p.views[id-1u].viewport;let f=vec2f(q)+.5;
  if(any(f<v.xy) || any(f>=v.xy+v.zw)){return 0.;}
  return bitcast<f32>(bits&0xfffffff0u);
}
fn linear_at(q:vec2i)->f32 {let bits=bitcast<u32>(packed_at(q));return depth_of(q,bits,bits&15u);}
fn linear_same(q:vec2i,id:u32)->f32 {return depth_of(q,bitcast<u32>(packed_at(q)),id);}
// View-space position of texel q at linear depth z, for a known camera.
fn position_in(q: vec2i,z: f32,id: u32) -> vec3f {
  let view=p.views[id-1u];
  let ndc=vec2f(2.,-2.)*((vec2f(q)+.5-view.viewport.xy)/view.viewport.zw)+vec2f(-1.,1.);
  let h=vec4f(ndc,.5,1.)*view.inverse;return h.xyz*(-z/h.z);
}
fn position_at(q: vec2i,z: f32) -> vec3f {return position_in(q,z,camera_at(q));}
fn normal_at(q: vec2i,z: f32) -> vec3f {
  // GX lighting normals can be in a game's custom lighting space. Projection
  // alone does not prove that basis. Derive the visible geometric view normal
  // from reconstructed positions instead of treating lighting data as view data.
  let id=camera_at(q);let center=position_in(q,z,id);
  let l=q-vec2i(1,0);let r=q+vec2i(1,0);let t=q-vec2i(0,1);let b=q+vec2i(0,1);
  let zl=linear_same(l,id);let zr=linear_same(r,id);let zt=linear_same(t,id);let zb=linear_same(b,id);
  var dx=vec3f(1.,0.,0.);var dy=vec3f(0.,-1.,0.);
  if(zl>0.&& (zr<=0.||abs(zl-z)<abs(zr-z))){dx=center-position_in(l,zl,id);}
  else if(zr>0.){dx=position_in(r,zr,id)-center;}
  if(zt>0.&& (zb<=0.||abs(zt-z)<abs(zb-z))){dy=center-position_in(t,zt,id);}
  else if(zb>0.){dy=position_in(b,zb,id)-center;}
  var n=normalize(cross(dy,dx));if(dot(n,center)>0.){n=-n;}return n;
}
fn noise(q: vec2i) -> f32 {
  var h=(u32(q.x)*1597334677u) ^ (u32(q.y)*3812015801u);h=(h^(h>>16u))*2246822519u;
  return f32(h&65535u)/65536.;
}
// Enhanced normals use XY octahedral encoding; Z packs normal validity and
// reflection evidence in the low three bits, roughness in the upper seven bits.
fn gx_normal_at(q:vec2i)->vec3f {
  let encoded=textureLoad(normals,q,0);
  if((u32(round(encoded.z*1023.))&4u)==0u){return vec3f(0.);}
  let oct=encoded.xy;
  let xy=oct*2.-1.;var n=vec3f(xy,1.-abs(xy.x)-abs(xy.y));
  if(n.z<0.){n=vec3f((1.-abs(n.yx))*select(vec2f(-1.),vec2f(1.),n.xy>=vec2f(0.)),n.z);}
  return normalize(n);
}
// Reflection parameters are estimates from explicit GX evidence, not semantic
// guesses such as "water" or "metal" based on brightness. X roughness, Y weight.
fn reflection_material(q:vec2i)->vec2f {
  let material_bits=u32(round(textureLoad(normals,q,0).z*1023.));
  let code=material_bits&3u;let roughness=f32(material_bits>>3u)/127.;
  if(code==3u){return vec2f(roughness,1.);}
  if(code==2u){return vec2f(roughness,.75);}
  return vec2f(1.,0.);
}
fn surface_roughness_at(q:vec2i)->f32 {
  return f32(u32(round(textureLoad(normals,q,0).z*1023.))>>3u)/127.;
}
// PCF taps compare against the receiver's plane at that tap. A constant depth
// bias alone creates broad self-shadow bands on walls under a low sun.
fn cascade_sample(pos:vec3f,n:vec3f,c:u32)->f32 {
  let size=i32(p.csm_info.y);
    let params=p.csm_params[c];
    let clip=vec4f(pos+n*params.x*.5,1.)*p.csm[c];
    let uv=clip.xy*vec2f(.5,-.5)+.5;
    if(any(uv<vec2f(.001)) || any(uv>vec2f(.999)) || clip.z<=0. || clip.z>=1.){return 1.;}
    let rows=p.csm[c];let sx=length(rows[0].xyz);let sy=length(rows[1].xyz);let sz=length(rows[2].xyz);
    let light_n=vec3f(dot(n,rows[0].xyz)/sx,dot(n,rows[1].xyz)/sy,dot(n,rows[2].xyz)/sz);
    let gradient=-light_n.xy/max(light_n.z,.02)*sz/vec2f(sx,sy);
    let texel=uv*f32(size)-.5;let cell=vec2i(floor(texel));let f=texel-floor(texel);
    var lit=0.;var total=0.;
    for(var y=-1;y<=2;y++){for(var x=-1;x<=2;x++){
      let w=select(select(1.,f.x,x==2),1.-f.x,x==-1)*select(select(1.,f.y,y==2),1.-f.y,y==-1);
      let local=clamp(cell+vec2i(x,y),vec2i(0),vec2i(size-1));
      let delta=(vec2f(local)+.5-uv*f32(size))*vec2f(2.,-2.)/f32(size);
      let receiver=clip.z+dot(gradient,delta)+params.y;
      let tap=local+vec2i(i32(c)*size,0);
      lit+=w*select(0.,1.,receiver>=textureLoad(cascades,tap,0));total+=w;
    }}
    return lit/total;
}
fn cascade_visibility(pos:vec3f,n:vec3f,id:u32)->f32 {
  let count=u32(p.csm_info.x);
  // csm_info.z: bit (id - 1) for every depth slice of the shadowed camera.
  if(count==0u || id==0u || ((u32(p.csm_info.z)>>(id-1u))&1u)==0u || dot(n,p.env_sun.xyz)<=0.){return 1.;}
  let distance=-pos.z;
  if(count>1u && distance<p.csm_params[0].z){
    let near=cascade_sample(pos,n,0u);
    if(distance<=p.csm_params[0].w){return near;}
    return mix(near,cascade_sample(pos,n,1u),smoothstep(p.csm_params[0].w,p.csm_params[0].z,distance));
  }
  let last=count-1u;
  return mix(cascade_sample(pos,n,last),1.,smoothstep(p.csm_params[last].w,p.csm_params[last].z,distance));
}
// GEKKOAOT_ENHANCED_VIEW=cascades: receiver light depth, stored depth at its
// texel, cascade (blue .25/.75) or magenta outside every cascade.
fn cascade_debug(pos:vec3f,n:vec3f,id:u32)->vec4f {
  if(u32(p.csm_info.x)==0u){return vec4f(0.,0.,0.,1.);}
  let size=i32(p.csm_info.y);
  for(var c=0u;c<u32(p.csm_info.x);c++){
    let clip=vec4f(pos+n*p.csm_params[c].x*1.5,1.)*p.csm[c];
    let uv=clip.xy*vec2f(.5,-.5)+.5;
    if(any(uv<vec2f(0.)) || any(uv>vec2f(1.))){continue;}
    let tap=clamp(vec2i(uv*f32(size)),vec2i(0),vec2i(size-1))+vec2i(i32(c)*size,0);
    return vec4f(clamp(clip.z,0.,1.),textureLoad(cascades,tap,0),f32(c)*.5+.25,1.);
  }
  return vec4f(1.,0.,1.,1.);
}
@fragment fn ao(@builtin(position) frag: vec4f) -> @location(0) vec4f {
  let q=vec2i(frag.xy*p.depth.zw/p.work.xy);
  let z=linear_at(q);if(z<=0.){return vec4f(1.);}
  let id=camera_at(q);let view=p.views[id-1u];
  let scale=camera_scale(id);let radius=p.effects.y*scale;
  let shadow_distance=p.effects.w*scale;let thickness=p.light.w*scale;
  let pos=position_at(q,z);let n=normal_at(q,z);let jitter=noise(q);
  var ambient=0.;
  let directions=u32(p.quality.x);let steps=u32(p.quality.y);
  // A view-space sphere projects to an ellipse for anisotropic lenses and
  // rectangular viewports. X focal length alone truncates vertical AO reach
  // in widescreen/multi-camera passes even though reconstructed depth is valid.
  let focal=abs(vec2f(view.projection[0][0],view.projection[1][1]));
  let reach=focal*view.viewport.zw*.5/z;
  let radius_pixels=clamp(reach*radius,vec2f(1.),vec2f(128.));
  // Screen-space occlusion needs a footprint of several pixels. Below that it
  // only classifies tessellation creases and depth noise (concave sky domes,
  // distant terrain). Fade by the footprint relative to the camera viewport
  // height, so the decision does not change with the output resolution.
  let ao_weight=smoothstep(.004,.012,max(reach.x,reach.y)*radius/view.viewport.w);
  let shadow_weight=smoothstep(.004,.012,max(reach.x,reach.y)*shadow_distance/view.viewport.w);
  if(p.effects.x>0. && ao_weight>0.){
    for(var i=0u;i<8u;i++){if(i>=directions){break;}
      let angle=(f32(i)+jitter)*6.2831853/f32(directions);let dir=vec2f(cos(angle),sin(angle));var horizon=0.;
      for(var j=1u;j<=8u;j++){if(j>steps){break;}
        let distance_pixels=radius_pixels*(f32(j)-.5+jitter*.5)/f32(steps);
        let tap=q+vec2i(round(dir*distance_pixels));
        // A subpixel tap can round back to the receiver. Re-evaluating the
        // same position after GPU contraction need not cancel bit-for-bit;
        // that residual is not an occluder, however small the effect radius.
        if(all(tap==q)){continue;}
        let sample_z=linear_same(tap,id);if(sample_z<=0.){continue;}
        let delta=position_in(tap,sample_z,id)-pos;let distance_view=length(delta);
        if(distance_view>.00001*scale && distance_view<radius){
          let elevation=max(0.,dot(n,delta/distance_view)-.08);
          horizon=max(horizon,elevation*(1.-distance_view/radius));
        }
      }
      ambient+=horizon;
    }
    ambient=mix(1.,clamp(1.-ambient/max(f32(directions),1.)*p.effects.x,0.,1.),ao_weight);
  }else{ambient=1.;}
  var shadow=1.;
  if(p.effects.z>0. && shadow_weight>0. && dot(p.light.xyz,p.light.xyz)>1e-8){
    let light=normalize(p.light.xyz);let count=u32(p.quality.w);
    let tangent=normalize(cross(light,select(vec3f(0.,1.,0.),vec3f(1.,0.,0.),abs(light.y)>.9)));
    let direction=normalize(light+tangent*(jitter-.5)*p.quality.z*.1);
    // A contact visibility correction applies only on the side facing this
    // light. A ray behind the receiver cannot shade its front-facing color.
    for(var j=1u;j<=32u;j++){if(j>count || dot(n,direction)<=0.){break;}
      let distance_view=shadow_distance*(f32(j)+jitter*.25)/f32(count);
      let candidate=pos+n*thickness*.5+direction*distance_view;
      if(candidate.z>=-.0001*scale){break;}
      let clip=vec4f(candidate,1.)*view.projection;if(clip.w<=0.){break;}
      let uv=clip.xy/clip.w*vec2f(.5,-.5)+.5;
)WGSL"
    R"WGSL(      let tap=vec2i(view.viewport.xy+uv*view.viewport.zw);let sample_z=linear_same(tap,id);
      if(all(tap==q) || sample_z<=0.){continue;}
      // Reject the receiver's own tangent plane. Nearest texel ray tests
      // otherwise shadow a sloped surface against itself, especially when
      // the march distance projects to less than one pixel.
      let surface_delta=position_in(tap,sample_z,id)-pos;
      let receiver_error=max(z*.000008,thickness*.02);
      // A blocker must be above the receiver plane. Testing abs() here also
      // admitted geometry behind it, creating irregular false wall shadows.
      if(dot(surface_delta,n)<=receiver_error){continue;}
      let gap=-candidate.z-sample_z;
      if(sample_z>0. && gap>thickness*.05 && gap<thickness){
        shadow=1.-shadow_weight*p.effects.z*(1.-distance_view/shadow_distance)*(1.-smoothstep(thickness*.5,thickness,gap));break;
      }
    }
  }
  if(u32(p.depth.x)==9u){return cascade_debug(pos,n,id);}
  return vec4f(ambient,shadow,cascade_visibility(pos,n,id),1.);
}
fn filtered(q: vec2i,axis: vec2i) -> vec4f {
  let dims=vec2i(textureDimensions(occlusion));let full=vec2i((vec2f(q)+.5)*p.depth.zw/vec2f(dims));let z=linear_at(full);
  if(z<=0.){return vec4f(1.);}
  let id=camera_at(full);var sum=vec4f(0.);var total=0.;
  for(var i=-2;i<=2;i++){
    let tap=clamp(q+axis*i,vec2i(0),dims-1);let at=vec2i((vec2f(tap)+.5)*p.depth.zw/vec2f(dims));let other=linear_same(at,id);
    if(other<=0.){continue;}
    let weight=exp(-f32(i*i)*.5)*exp(-abs(other-z)/max(.001*camera_scale(id),p.effects.y*camera_scale(id)*.05+z*.002));
    sum+=textureLoad(occlusion,tap,0)*weight;total+=weight;
  }
  return sum/max(total,.00001);
}
@fragment fn blur_x(@builtin(position) frag: vec4f) -> @location(0) vec4f {return filtered(vec2i(frag.xy),vec2i(1,0));}
// Original color at an EFB position (the copy rectangle may scale it).
fn color_at(q:vec2i)->vec3f {
  let dims=vec2f(textureDimensions(color));
  let t=vec2i((vec2f(q)+.5-p.copy_rect.xy)/p.copy_rect.zw*dims);
  return textureLoad(color,clamp(t,vec2i(0),vec2i(dims)-1),0).rgb;
}
fn fresnel(cos_theta:f32)->f32 {let m=1.-clamp(cos_theta,0.,1.);return .04+.96*m*m*m*m*m;}
// One-bounce diffuse light: visible receivers in front of the surface, whose
// own normal faces it, send their original color (cosine and distance falloff).
@fragment fn gi(@builtin(position) frag: vec4f) -> @location(0) vec4f {
  let q=vec2i(frag.xy*p.depth.zw/p.work.xy);
  let z=linear_at(q);if(z<=0.){return vec4f(0.);}
  let id=camera_at(q);let view=p.views[id-1u];
  if(p.gi.x<=0.){return vec4f(particle_lighting(q,z,id,surface_normal(q,z)),1.);}
  let scale=camera_scale(id);let radius=p.gi.y*scale;
  let pos=position_at(q,z);let n=surface_normal(q,z);let jitter=noise(q+vec2i(17,31));
  let focal=abs(vec2f(view.projection[0][0],view.projection[1][1]));
  let radius_pixels=clamp(focal*view.viewport.zw*.5/z*radius,vec2f(2.),vec2f(160.));
  let directions=u32(p.gi.w);let steps=u32(p.gi.z);
  var sum=vec3f(0.);var count=0.;
  for(var i=0u;i<8u;i++){if(i>=directions){break;}
    let angle=(f32(i)+jitter)*6.2831853/f32(directions);let dir=vec2f(cos(angle),sin(angle));
    for(var j=1u;j<=8u;j++){if(j>steps){break;}
      let t=(f32(j)-.5+jitter*.5)/f32(steps);
      let tap=q+vec2i(round(dir*radius_pixels*t*t));
      if(all(tap==q)){continue;}
      let sample_z=linear_same(tap,id);if(sample_z<=0.){continue;}
      let delta=position_in(tap,sample_z,id)-pos;let distance_view=length(delta);
      if(distance_view<.00001*scale || distance_view>radius){continue;}
      count+=1.;
      let l=delta/distance_view;let receiving=dot(n,l);
      if(receiving<=.05){continue;}
      let sender=gx_normal_at(tap);
      if(dot(sender,sender)>.25 && dot(normalize(sender),-l)<=0.){continue;}
      sum+=color_at(tap)*receiving*(1.-distance_view/radius);
    }
  }
  return vec4f(sum/max(count,1.)*2.*p.gi.x+particle_lighting(q,z,id,n),1.);
}
// Reflections: view-space march along the reflected ray with growing steps,
// a depth-thickness hit test and a binary refinement. Alpha is confidence.
@fragment fn ssr(@builtin(position) frag: vec4f) -> @location(0) vec4f {
  let q=vec2i(frag.xy*p.depth.zw/p.work.xy);
  let z=linear_at(q);if(z<=0.){return vec4f(0.);}
  let material=reflection_material(q);if(material.y<=0.){return vec4f(0.);}
  let id=camera_at(q);let view=p.views[id-1u];
  let pos=position_at(q,z);let n=surface_normal(q,z);
  let v=normalize(pos);let r=reflect(v,n);
  if(fresnel(dot(-v,n))*p.ssr.x<.01){return vec4f(0.);}
  let length_view=z*p.ssr.y;let steps=u32(p.ssr.z);let jitter=noise(q+vec2i(5,11));
  let start=pos+n*z*.002;
  var previous=0.;
  for(var k=1u;k<=48u;k++){if(k>steps){break;}
    let t=(f32(k)-1.+jitter)/f32(steps);let d=length_view*t*t;
    let point_view=start+r*d;
    if(point_view.z>=-.0001*z){break;}
    let clip=vec4f(point_view,1.)*view.projection;if(clip.w<=0.){break;}
    let ndc=clip.xy/clip.w;if(any(abs(ndc)>vec2f(1.))){break;}
    let uv=ndc*vec2f(.5,-.5)+.5;let tap=vec2i(view.viewport.xy+uv*view.viewport.zw);
    // Within two pixels of the start the receiver would hit itself.
    if(dot(vec2f(tap-q),vec2f(tap-q))<4.){previous=d;continue;}
    let scene=linear_same(tap,id);
    let gap=-point_view.z-scene;
    if(scene>0. && gap>0. && gap<max(scene*.03,(d-previous)*2.)){
      var low=previous;var high=d;var hit=tap;
      for(var b=0;b<5;b++){
        let mid=.5*(low+high);let s=start+r*mid;let c=vec4f(s,1.)*view.projection;
        let m=view.viewport.xy+((c.xy/c.w)*vec2f(.5,-.5)+.5)*view.viewport.zw;let mt=vec2i(m);
        let mz=linear_same(mt,id);
        if(mz>0. && -s.z>mz){high=mid;hit=mt;}else{low=mid;}
      }
      // A surface not facing the ray is no hit; edges and the far end fade.
      let facing=-dot(r,surface_normal(hit,linear_same(hit,id)));
      if(facing>.05){
        let edge=min(min(uv.x,1.-uv.x),min(uv.y,1.-uv.y));
        // A small depth-aware cone footprint softens rough specular surfaces
        // while refusing taps from another camera or across a depth edge.
        let radius=clamp(material.x*material.x*d/z*12.,0.,4.);
        let hit_z=linear_same(hit,id);var reflected=color_at(hit);var weight=1.;
        for(var j=0u;j<4u;j++){
          let offsets=array<vec2f,4>(vec2f(1.,0.),vec2f(-1.,0.),vec2f(0.,1.),vec2f(0.,-1.));
          let at=hit+vec2i(round(offsets[j]*radius));let depth=linear_same(at,id);
          if(depth>0. && abs(depth-hit_z)<max(z*.01,hit_z*.02)){reflected+=color_at(at);weight+=1.;}
        }
        return vec4f(reflected/weight,material.y*(1.-material.x*.5)*smoothstep(0.,.08,edge)*(1.-t*t)*smoothstep(.05,.3,facing));
      }
    }
    previous=d;
  }
  return vec4f(0.);
}
@fragment fn blur_y(@builtin(position) frag: vec4f) -> @location(0) vec4f {return filtered(vec2i(frag.xy),vec2i(0,1));}
// GEKKOAOT_ENHANCED_VIEW diagnostics. Rejected writers carry their reason as
// a negative prepared depth (never a receiver; see the prepare pass).
// Codes: 3 texture producer, 4 orthographic interface, 5 no depth test,
// 6 unproved depth, 7 color writes disabled, 8 unproved blend, 9 Z texture,
// 10 screen fill, 11 perspective interface card, 12 inset viewport,
// 13 camera rejected/invalid, 14 translucent fragment of a receiver draw,
// 15 non-positive view depth.
fn reason_color(code:u32)->vec3f {
  if(code==3u){return vec3f(0.,.6,.6);}
  if(code==4u){return vec3f(0.,.8,1.);}
  if(code==5u){return vec3f(.6,0.,1.);}
  if(code==6u){return vec3f(1.,0.,0.);}
  if(code==7u){return vec3f(.5,0.,0.);}
  if(code==8u){return vec3f(1.,0.,1.);}
  if(code==9u){return vec3f(.6,.4,.2);}
  if(code==10u){return vec3f(1.);}
  if(code==11u){return vec3f(.5,.7,1.);}
  if(code==12u){return vec3f(.2,.2,.9);}
  if(code==13u){return vec3f(1.,.5,0.);}
  if(code==15u){return vec3f(1.,.6,.8);}
  return vec3f(.4);
}
fn camera_color(id:u32)->vec3f {
  let k=(id-1u)%3u;let hue=select(select(vec3f(.1,.7,.6),vec3f(.2,.5,1.),k==1u),vec3f(.1,.9,.2),k==0u);
  return select(hue,hue*.6+vec3f(.3),id>3u);
}
fn luminance(c:vec3f)->f32{return dot(c,vec3f(.299,.587,.114));}
fn diagnostic_rejected(original:vec4f,q:vec2i,mode:u32)->vec4f {
  let l=luminance(original.rgb);
  if(mode==1u || mode==3u){return vec4f(l*.15,l*.15,l*.3+.08,original.a);}
  let encoded=-packed_at(q);
  if(encoded>0.){
    let code=u32(floor(encoded));
    // A translucent receiver fragment shows its exact alpha in green.
    if(code==14u){return vec4f(1.,fract(encoded)*2048./1023.,0.,original.a);}
    return vec4f(reason_color(code)*(.3+.7*l),original.a);
  }
  return vec4f(vec3f(l*.2),original.a);
}
// Environment lighting: the captured receiver color already holds the game's
// lighting. A view-space sky hemisphere (occluded by AO) and a world-stable sun
// (occluded by contact shadows) give a relative relight factor normalised to
// the average exposure; without environment light this is AO * contact.
// Smooth GX vertex normals replace the faceted depth normal when they agree
// with the visible surface (lighting spaces that differ are rejected).
fn surface_normal(q:vec2i,z:f32)->vec3f {
  let n=normal_at(q,z);
  let encoded=gx_normal_at(q);
  if(dot(encoded,encoded)>.25){let g=normalize(encoded);if(dot(g,n)>.3){return g;}}
  // CPU-lit meshes can supply only vertex colors, with no GX normals. Their
  // depth derivatives are triangle-flat. Reconstruct a smooth local normal
  // without crossing silhouettes, other cameras or sharp surface creases.
  let id=camera_at(q);let center=position_in(q,z,id);
  let radius=max(1,i32(round(p.depth.w/240.)));
  let left=q-vec2i(radius,0);let right=q+vec2i(radius,0);
  let top=q-vec2i(0,radius);let bottom=q+vec2i(0,radius);
  let zl=linear_same(left,id);let zr=linear_same(right,id);
  let zt=linear_same(top,id);let zb=linear_same(bottom,id);
  if(min(min(zl,zr),min(zt,zb))<=0.){return n;}
  let dx=position_in(right,zr,id)-position_in(left,zl,id);
  let dy=position_in(bottom,zb,id)-position_in(top,zt,id);
  var candidate=cross(dy,dx);let magnitude=dot(candidate,candidate);
  if(magnitude<1e-18){return n;}
  candidate*=inverseSqrt(magnitude);if(dot(candidate,center)>0.){candidate=-candidate;}
  // A wider derivative blends adjacent smooth triangles, with a crease gate
  // and a tangent-plane test that prevent mixing silhouettes and depth layers.
  let spread=max(length(dx),length(dy));
  let error=max(max(abs(zl-z),abs(zr-z)),max(abs(zt-z),abs(zb-z)));
  let continuous=1.-smoothstep(spread,spread*2.,error);
  return normalize(mix(n,candidate,smoothstep(.7,.95,dot(n,candidate))*continuous));
}
fn environment_factor(n:vec3f,ao:f32,shadow:f32,sun_visibility:f32)->vec3f {
  let base=vec3f(ao*shadow);
  if(p.env_sun_color.w<=0.){
    // Original lighting with sun cascades: the GX/TEV colors stay; receivers
    // turned to the shadow light only darken where the cascades occlude it.
    if(p.csm_info.x<1. || p.csm_info.w<=0. || sun_visibility>=1.){return base;}
    let facing=smoothstep(0.,.25,dot(n,p.env_sun.xyz));
    return base*mix(1.,sun_visibility,p.csm_info.w*facing);
  }
  // Unit-mean sky (neutral until the vertical is locked) occluded by AO, and
  // a unit-luminance sun (mean of 3 max(0,n.l) about 1) occluded by contact
  // shadows and cascades, weighted by their filtered confidences.
  let oriented=mix(p.env_ground.rgb,p.env_sky.rgb,dot(n,p.env_up.xyz)*.5+.5);
  let sky=mix(vec3f(1.),oriented,p.env_ground.w)*ao;
  let sun=p.env_sun_color.rgb*3.*max(0.,dot(n,p.env_sun.xyz))*shadow*sun_visibility;
  let total=p.env_sky.w+p.env_sun.w;
  if(total<=.0001){return base;}
  // Whatever the settings, a surface keeps at least 45% of its original
  // light: the sky and bounced light never vanish behind a low, strong sun.
  return max(mix(base,(sky*p.env_sky.w+sun*p.env_sun.w)/total,p.env_sun_color.w),base*.45);
}
// Highlight shoulder for brightened receivers: values below .75 are exact.
fn shoulder(x:vec3f)->vec3f {
  return select(x,.75+.25*(1.-exp(-(x-.75)/.25)),x>vec3f(.75));
}
)WGSL"
    R"WGSL(// Resolve only once per source. Alpha-over flames and additive flashes both
// leave a positive non-receiver residual. Alpha alone is not emission: reject
// dark/neutral smoke and green foliage, and require a visible hot colour.
// This is conservative screen-space evidence, not semantic material tagging.
fn emitter_energy(i:u32)->vec3f {
  // Actual decoded texture + GX/TEV source colour does not depend on which
  // framebuffer pixels happen to lie behind this draw's centroid.
  if(p.emitter_colors[i].w>.5){return p.emitter_colors[i].rgb;}
  let id=u32(p.emitter_views[i].x);if(id==0u){return vec3f(0.);}
  let light=p.emitters[i];let view=p.views[id-1u];
  let clip=vec4f(light.xyz,1.)*view.projection;
  if(clip.w<=0.){return vec3f(0.);}
  let at=view.viewport.xy+(clip.xy/clip.w*vec2f(.5,-.5)+.5)*view.viewport.zw;
  if(any(at<view.viewport.xy) || any(at>=view.viewport.xy+view.viewport.zw)){return vec3f(0.);}
  let background=linear_same(vec2i(at),id);
  if(background>0. && background < -light.z-light.w*.15){return vec3f(0.);}
  var energy=vec3f(0.);var hot=false;
  let footprint=clamp(abs(view.projection[1][1])*view.viewport.w*.5*light.w/(-light.z*8.),1.,24.);
  for(var j=0u;j<9u;j++){
    var offset=vec2f(0.);
    if(j>0u){let angle=f32(j-1u)*.78539816;offset=vec2f(cos(angle),sin(angle))*footprint;}
    let sample=vec2i(at+offset);
    if(any(sample<vec2i(0)) || any(sample>=vec2i(p.depth.zw))){continue;}
    let residual=max(vec3f(0.),color_at(sample)-textureLoad(contribution,sample,0).rgb);
    energy=max(energy,residual);
    let warm=residual.r>.25 && residual.g>.055 && residual.r>residual.g*1.12 && residual.g>residual.b*1.2;
    let blue=residual.b>.65 && residual.b>residual.r*1.4 && residual.b>residual.g*1.12;
    hot=hot || warm || blue;
  }
  if(luminance(energy)<.05 || (p.emitter_views[i].y>.5 && !hot)){return vec3f(0.);}
  return energy;
}
@compute @workgroup_size(64) fn resolve_emitters(@builtin(local_invocation_index) i:u32) {
  let frame=u32(p.emission.w);
  if(f32(i)<p.emission.y){
    let energy=emitter_energy(i);particle_raw_energy[i]=energy;
    let key=u32(p.emitter_views[i].z);let cached=particle_history[key&255u];
    let recent=cached.key==key && ((frame-cached.frame)&65535u)<120u;
    // A crosshair, clipped centre or animated transparent texel must not
    // extinguish a source already observed hot. Keep colour, not stale 3D
    // positions: positions still come from this scene's actual GX geometry.
    particle_energy[i]=energy;
    if(recent && (p.emitter_colors[i].w<.5 || luminance(energy)>.05)){
      particle_energy[i]=select(cached.energy.rgb,mix(cached.energy.rgb,energy,.15),luminance(energy)>.05);}
  }
  workgroupBarrier();
  storageBarrier();
  if(i!=0u){return;}
  // One writer makes colliding cache slots and multiple sprites sharing a
  // texture deterministic; no CPU readback or per-fragment atomics.
  for(var j=0u;j<64u;j++){
    if(f32(j)>=p.emission.y){break;}
    if(luminance(particle_raw_energy[j])>.05){
      let key=u32(p.emitter_views[j].z);
      particle_history[key&255u]=ParticleHistory(vec4f(particle_energy[j],1.),key,frame,vec2u(0));
    }
  }
  var scores:array<f32,64>;
  for(var j=0u;j<64u;j++){scores[j]=luminance(particle_energy[j]);}
  for(var k=0u;k<8u;k++){
    resolved_particles[k*2u]=vec4f(0.);resolved_particles[k*2u+1u]=vec4f(0.);
    if(f32(k)>=p.emission.z){continue;}
    var selected=0u;var best=0.;
    for(var j=0u;j<64u;j++){
      if(f32(j)>=p.emission.y){break;}
      if(scores[j]>best){best=scores[j];selected=j;}
    }
    if(best<=.0){continue;}
    let base=p.emitters[selected];let id=u32(p.emitter_views[selected].x);
    var centre=vec3f(0.);var total=0.;var energy=vec3f(0.);var radius=base.w;
    for(var j=0u;j<64u;j++){
      if(f32(j)>=p.emission.y){break;}
      if(scores[j]<=0. || u32(p.emitter_views[j].x)!=id){continue;}
      let other=p.emitters[j];let distance=length(other.xyz-base.xyz);
      if(j!=selected && distance>min(other.w,base.w)*.45){continue;}
      centre+=other.xyz*scores[j];energy+=particle_energy[j]*scores[j];total+=scores[j];
      radius=max(radius,other.w);scores[j]=0.;
    }
    resolved_particles[k*2u]=vec4f(centre/max(total,.00001),radius);
    // Low four bits identify the receiver camera; the rest identify the
    // representative source for opt-in GPU diagnostic counters.
    var radiance=energy/max(total,.00001);
    // Warm sources project fire-coloured light; neutral flashes and blue
    // effects keep their original colour. The original sprite is untouched.
    if(radiance.r>=radiance.g*.85 && radiance.g>radiance.b*1.35){
      let peak=max(radiance.r,max(radiance.g,radiance.b));
      radiance=mix(radiance,vec3f(1.,.36,.06)*peak,p.emission_style.y)*1.25;
    }
    resolved_particles[k*2u+1u]=vec4f(radiance,f32(id+(selected+1u)*16u));
  }
}
// Bounded 4/8 selected lights; no per-pixel source-colour reconstruction and
// no CPU readback. Positions are decoded from original, ungathered GX indices.
fn particle_lighting(q:vec2i,z:f32,id:u32,n:vec3f)->vec3f {
  if(!gekkoaot_particle_enabled || p.emission.x<=0. || p.emission.y<1.){return vec3f(0.);}
  let view=p.views[id-1u];let point=position_in(q,z,id);var sum=vec3f(0.);
  for(var i=0u;i<8u;i++){
    if(f32(i)>=p.emission.z){break;}
    let source=particle_sources[i*2u+1u];
    if(source.a<1.){continue;}
    if((u32(source.a)&15u)!=id){continue;}
    let light=particle_sources[i*2u];let delta=light.xyz-point;let distance=length(delta);
    // Illumination reach is independent of source clustering, so increasing
    // the radius cannot merge unrelated fires or move their positions.
    let range=max(light.w,camera_scale(id)*p.emission_style.z)*p.emission_style.x;
    if(distance>=range || distance<=.00001){continue;}
    let receiving=max(0.,dot(n,delta/distance));if(receiving<=.001){continue;}
    var visibility=1.;
    // Visible-depth occlusion, bounded to three taps. Offscreen geometry is
    // unavailable: these are contact shadows, not invented local shadow maps.
    for(var step=1u;step<=3u;step++){
      let along=mix(point,light.xyz,f32(step)*.22);
      let h=vec4f(along,1.)*view.projection;
      let uv=view.viewport.xy+(h.xy/h.w*vec2f(.5,-.5)+.5)*view.viewport.zw;
      let d=linear_same(vec2i(uv),id);
      if(d>0.){
        let tolerance=max(camera_scale(id)*.01,light.w*.025);
        let blockage=smoothstep(tolerance,tolerance*3.,-along.z-d);
        visibility*=mix(1.,.15,blockage);
      }
    }
    let squared=distance*distance/(range*range);let edge=1.-squared;
    let falloff=edge*edge/(1.+3.*squared);
    sum+=source.rgb*receiving*falloff*visibility*p.emission.x*2.;
  }
  // Preserve flame hue at high intensity; clipping RGB independently turns
  // several orange sources into a flat yellow/white wash.
  let peak=max(sum.r,max(sum.g,sum.b));
  return sum*min(1.,4./max(peak,.00001));
}
// Far-only circle of confusion: a fixed lens-relative plane never hunts
// when the crosshair passes an edge. Foreground weapons stay sharp. Invalid
// receiver metadata (HUD, glyphs and other cameras) is never blurred into 3D.
fn depth_of_field(frag:vec2f,q:vec2i,z:f32,id:u32,original:vec4f)->vec4f {
  if(!gekkoaot_dof_enabled || p.dof.x<=0.){return original;}
  let focus=p.dof.y*camera_scale(id);
  let coc=clamp((z-focus)/max(z,.00001),0.,1.)*p.dof.x;
  if(coc<.015){return original;}
  let dims=vec2f(textureDimensions(color));
  let radius=coc*max(4.,min(dims.x,dims.y)*.012);
  var sum=original.rgb;var total=1.;
  for(var i=0u;i<24u;i++){
    if(f32(i)>=p.dof.z){break;}
    let angle=f32(i)*2.39996323;
    let delta=vec2f(cos(angle),sin(angle))*sqrt((f32(i)+.5)/p.dof.z)*radius;
    let at=clamp(vec2i(frag+delta),vec2i(0),vec2i(dims)-1);
    let efb=vec2i(p.copy_rect.xy+(vec2f(at)+.5)/dims*p.copy_rect.zw);
    let other=linear_same(efb,id);
    if(other<=0. || textureLoad(normals,efb,0).w<.99){continue;}
    // Reject sharp foreground: avoids bright halos around weapons and walls.
    let weight=smoothstep(focus*.9,focus*1.1,other)*exp(-max(0.,z-other)/max(z*.1,.00001));
    sum+=textureLoad(color,at,0).rgb*weight;total+=weight;
  }
  return vec4f(sum/total,original.a);
}
@fragment fn compose(@builtin(position) frag: vec4f) -> @location(0) vec4f {
  var original=textureLoad(color,vec2i(frag.xy),0);
  let efb=p.copy_rect.xy+frag.xy/vec2f(textureDimensions(color))*p.copy_rect.zw;
  let q=vec2i(efb);let z=linear_at(q);let mode=u32(p.depth.x);
  // Split view: the left half keeps the original image of this very frame.
  if(mode==4u && frag.x<f32(textureDimensions(color).x)*.5){return original;}
  // GEKKOAOT_ENHANCED_VIEW=cascade-atlas: the sun's view of the casters
  // (cascades side by side), brighter = nearer the sun.
  if(mode==13u){
    let atlas=vec2f(textureDimensions(cascades));
    let d=textureLoad(cascades,vec2i(frag.xy/vec2f(textureDimensions(color))*atlas),0);
    return vec4f(vec3f(select(0.,.15+.85*fract(d*8.),d>0.)),1.);
  }
  if(z<=0. || textureLoad(normals,q,0).w<.99){
    if(mode==0u || mode==4u || mode==8u || mode==10u){return original;}return diagnostic_rejected(original,q,mode);
  }
  let id=camera_at(q);
  let dims=vec2i(textureDimensions(occlusion));let location=efb*vec2f(dims)/p.depth.zw-.5;let base=vec2i(floor(location));
  if(mode==5u){return vec4f(surface_normal(q,z)*.5+.5,original.a);}
  if(mode==6u){return vec4f(camera_color(id),original.a);}
  var value=vec3f(0.);var total=0.;var bounce=vec3f(0.);var mirror=vec4f(0.);
  for(var y=0;y<2;y++){for(var x=0;x<2;x++){
    let tap=clamp(base+vec2i(x,y),vec2i(0),dims-1);let at=vec2i((vec2f(tap)+.5)*p.depth.zw/vec2f(dims));let d=linear_same(at,id);
    if(d<=0.){continue;}
    let weight=max(.001,1.-abs(location.x-f32(base.x+x)))*max(.001,1.-abs(location.y-f32(base.y+y)))*exp(-abs(d-z)/max(.001*camera_scale(id),p.effects.y*camera_scale(id)*.03+z*.001));
    value+=textureLoad(occlusion,tap,0).xyz*weight;total+=weight;
    if(p.gi.x>0. || p.emission.y>0.){bounce+=textureLoad(indirect,tap,0).rgb*weight;}
    if(p.ssr.x>0.){mirror+=textureLoad(reflections,tap,0)*weight;}
  }}
  if(total<.00001){
    if(mode==0u || mode==4u || mode==8u || mode==10u){return original;}value=vec3f(1.);total=1.;
  }
  value/=total;bounce/=total;mirror/=total;
  if(mode==1u){return vec4f(vec3f(value.x*value.y*value.z),original.a);}
  if(mode==9u){return vec4f(value,original.a);}
  // One surface normal per pixel for every term below.
  let n=surface_normal(q,z);
  if(mode==7u){return vec4f(environment_factor(n,value.x,value.y,value.z)*.5,original.a);}
  if(mode==2u){return vec4f(camera_color(id)*(.3+.7*luminance(original.rgb)),original.a);}
  if(mode==3u){
    // View depth relative to this lens' far clip distance (reversed or not).
    let ndc0=vec4f(0.,0.,0.,1.)*p.views[id-1u].inverse;let ndc1=vec4f(0.,0.,1.,1.)*p.views[id-1u].inverse;
    let d0=abs(ndc0.z/ndc0.w);let d1=abs(ndc1.z/ndc1.w);
    let clip_near=max(min(d0,d1),1e-6);let clip_far=max(max(d0,d1),clip_near*1.001);
    return vec4f(vec3f(clamp(log(max(z,clip_near)/clip_near)/log(clip_far/clip_near),0.,1.)),original.a);
  }
  let opaque=textureLoad(contribution,q,0).rgb;
  // Split views: the left half keeps AO/contact only (environment-split) or
  // the environment without sun cascades (shadow-split).
  let left=frag.x<f32(textureDimensions(color).x)*.5;
  var factor=environment_factor(n,value.x,value.y,select(value.z,1.,mode==10u && left));
  if(mode==8u && left){factor=vec3f(value.x*value.y);}
  var lit=max(vec3f(0.),original.rgb+opaque*(factor-1.+bounce));
  if(p.ssr.x>0. && mirror.a>0. && reflection_material(q).y>0.){
    let v=normalize(position_at(q,z));
    // Only the receiver's own share of the pixel reflects.
    let share=clamp(dot(opaque,vec3f(1.))/max(dot(original.rgb,vec3f(1.)),.0001),0.,1.);
    lit=mix(lit,mirror.rgb,clamp(mirror.a*fresnel(dot(-v,n))*p.ssr.x*share,0.,1.));
  }
  if(p.ssr.w>0.){
    // Highlights the original lighting never had: GGX for the sun, and the sky
)WGSL"
    R"WGSL(    // at grazing angles on smooth surfaces, only on the receiver's share.
    let v=-normalize(position_at(q,z));
    let rough=clamp(surface_roughness_at(q),.08,1.);
    let share=clamp(dot(opaque,vec3f(1.))/max(dot(original.rgb,vec3f(1.)),.0001),0.,1.);
    let ndv=max(dot(n,v),.001);
    var response=vec3f(0.);
    if(p.env_sun_color.w>0. && p.env_sun.w>0.){
      let l=p.env_sun.xyz;let h=normalize(v+l);
      let ndl=max(dot(n,l),0.);let ndh=max(dot(n,h),0.);
      let a=rough*rough;let a2=a*a;let d=ndh*ndh*(a2-1.)+1.;
      let k=(rough+1.)*(rough+1.)*.125;
      let visibility=.25/max((ndl*(1.-k)+k)*(ndv*(1.-k)+k),.001);
      let highlight=min(a2/(3.14159265*d*d)*fresnel(max(dot(v,h),0.))*visibility*ndl,4.);
      response+=p.env_sun_color.rgb*highlight*value.y*value.z;
    }
    let sky_tint=mix(p.env_ground.rgb,p.env_sky.rgb,dot(reflect(-v,n),p.env_up.xyz)*.5+.5);
    let smoothness=(1.-rough)*(1.-rough);
    response+=mix(vec3f(1.),sky_tint,p.env_ground.w)*fresnel(ndv)*smoothness*value.x*.5;
    lit+=response*p.ssr.w*share;
  }
  if(mode==11u){return vec4f(bounce,original.a);}
  if(mode==12u){return vec4f(mirror.rgb*mirror.a,original.a);}
  if(p.env_sun_color.w<=0. && p.gi.x<=0. && p.emission.y<=0.){return vec4f(lit,original.a);}
  return vec4f(select(lit,original.rgb+shoulder(lit)-shoulder(original.rgb),lit>original.rgb),original.a);
}
// Full-image lens pass after all scene lighting and shadow composition.
@fragment fn dof_compose(@builtin(position) frag:vec4f)->@location(0) vec4f {
  let original=textureLoad(color,vec2i(frag.xy),0);
  if(u32(p.depth.x)==4u && frag.x<f32(textureDimensions(color).x)*.5){return original;}
  let efb=p.copy_rect.xy+frag.xy/vec2f(textureDimensions(color))*p.copy_rect.zw;
  let q=vec2i(efb);let z=linear_at(q);
  if(z<=0. || textureLoad(normals,q,0).w<.99){return original;}
  return depth_of_field(frag.xy,q,z,camera_at(q),original);
}
// Copy alias fallback: same size as the copy, texel for texel.
@fragment fn passthrough(@builtin(position) frag: vec4f) -> @location(0) vec4f {
  return textureLoad(color,vec2i(frag.xy),0);
}
)WGSL";
inline constexpr char DepthShader[] = R"WGSL(
@fragment fn prepare(@builtin(position) frag:vec4f)->@location(0) f32 {
  let q=vec2i(frag.xy);
  if(any(q<vec2i(0)) || any(q>=vec2i(p.depth.zw)) || textureLoad(normals,q,0).w<.99){return 0.;}
  let captured_depth=read_capture(q);let id=bitcast<u32>(captured_depth)&15u;
  let z=bitcast<f32>(bitcast<u32>(captured_depth)&0xfffffff0u);
  // Coverage view: a rejected writer stored its reason in the low bits and
  // its fragment alpha above them, never a normal (non-denormal) depth.
  let bits=bitcast<u32>(captured_depth);
  if(p.depth.x>1.5 && id!=0u && (bits>>23u)==0u){return -(f32(id)+f32((bits>>11u)&1023u)/2048.);}
  if(id==0u || !(z>0. && z<1e20)){return 0.;}return captured_depth;
}
)WGSL";
}
