// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <map>
#include <span>
#include <vector>

namespace GekkoAOT::GX::Mesh {
struct V3 {float x=0,y=0,z=0;};
inline V3 operator+(V3 a,V3 b){return {a.x+b.x,a.y+b.y,a.z+b.z};}
inline V3 operator-(V3 a,V3 b){return {a.x-b.x,a.y-b.y,a.z-b.z};}
inline V3 operator*(V3 a,float b){return {a.x*b,a.y*b,a.z*b};}
inline float Dot(V3 a,V3 b){return a.x*b.x+a.y*b.y+a.z*b.z;}
inline V3 Cross(V3 a,V3 b){return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};}
inline float Length(V3 a){return std::sqrt(Dot(a,a));}
inline V3 Normalize(V3 a){const float n=Length(a);return n>1e-12f?a*(1/n):V3{};}
inline bool Finite(V3 a){return std::isfinite(a.x)&&std::isfinite(a.y)&&std::isfinite(a.z);}
// The adapter preserves every original per-corner UV/color in this payload.
// Position welding is used only for topology, never to merge UV seams.
struct Vertex {V3 position,normal;std::array<float,24> attributes{};V3 lightingNormal;unsigned sourceIndex=0;};
struct Triangle {std::array<unsigned,3> corners{};};
struct PatchLayout {std::array<unsigned,3> vertices{};std::array<unsigned,6> normals{};unsigned straight=0;};
struct Sample {unsigned patch=0;std::array<float,3> bary{};};
struct Result {
  std::vector<Vertex> vertices;
  std::vector<PatchLayout> patches;
  std::vector<Sample> samples;
  unsigned originalTriangles=0,refinedTriangles=0,boundaryEdges=0,hardEdges=0,curvingEdges=0;
  float maximumMovement=0;
};
struct PositionKey {
  std::array<std::uint32_t,3> bits;
  auto operator<=>(const PositionKey&)const=default;
};
inline PositionKey Key(V3 p){
  return {{std::bit_cast<std::uint32_t>(p.x==0?0.f:p.x),std::bit_cast<std::uint32_t>(p.y==0?0.f:p.y),
           std::bit_cast<std::uint32_t>(p.z==0?0.f:p.z)}};
}
// Missing GX normals may be reconstructed only from a validated connected
// triangle mesh. Face corners join across consistently oriented edges with a
// dihedral below 45 degrees; sharp edges and open boundaries remain separate.
inline bool ReconstructNormals(std::vector<Vertex>& vertices,std::vector<Triangle>& triangles){
  if(vertices.empty()||triangles.empty()||triangles.size()>4096)return false;
  std::map<PositionKey,unsigned> positions;std::vector<unsigned> ids;
  for(const auto& v:vertices){if(!Finite(v.position))return false;auto [it,added]=positions.emplace(Key(v.position),positions.size());ids.push_back(it->second);}
  std::vector<Triangle> valid;
  for(const auto& t:triangles){if(*std::max_element(t.corners.begin(),t.corners.end())>=vertices.size())return false;
    const auto a=vertices[t.corners[0]].position,b=vertices[t.corners[1]].position,c=vertices[t.corners[2]].position;
    if(Length(Cross(b-a,c-a))>1e-12f)valid.push_back(t);
  }
  if(valid.empty())return false;
  std::vector<unsigned> parent(valid.size()*3);for(unsigned i=0;i<parent.size();++i)parent[i]=i;
  const auto find=[&](unsigned i){while(parent[i]!=i){parent[i]=parent[parent[i]];i=parent[i];}return i;};
  const auto join=[&](unsigned a,unsigned b){a=find(a);b=find(b);if(a!=b)parent[b]=a;};
  std::vector<V3> normals(valid.size());
  struct Side{unsigned face,corner;bool forward;unsigned count=1;};
  std::map<std::pair<unsigned,unsigned>,Side> edges;
  for(unsigned f=0;f<valid.size();++f){const auto& t=valid[f];
    const auto a=vertices[t.corners[0]].position,b=vertices[t.corners[1]].position,c=vertices[t.corners[2]].position;
    normals[f]=Normalize(Cross(b-a,c-a));
    for(unsigned c0=0;c0<3;++c0){const unsigned a0=ids[t.corners[c0]],b0=ids[t.corners[(c0+1)%3]];const bool forward=a0<b0;
      auto [it,added]=edges.emplace(std::pair{std::min(a0,b0),std::max(a0,b0)},Side{f,c0,forward});
      if(added)continue;
      auto& previous=it->second;if(previous.count++!=1||previous.forward==forward)return false;
      if(Dot(normals[f],normals[previous.face])<.70710678f)continue;
      join(f*3+c0,previous.face*3+(previous.corner+1)%3);
      join(f*3+(c0+1)%3,previous.face*3+previous.corner);
    }
  }
  std::vector<V3> sums(parent.size());
  for(unsigned f=0;f<valid.size();++f)for(unsigned c=0;c<3;++c){const auto& t=valid[f];const auto p=vertices[t.corners[c]].position;
    const auto a=vertices[t.corners[(c+1)%3]].position-p,b=vertices[t.corners[(c+2)%3]].position-p;
    const float angle=std::atan2(Length(Cross(a,b)),Dot(a,b));const auto root=find(f*3+c);sums[root]=sums[root]+normals[f]*angle;
  }
  std::vector<Vertex> corners;std::vector<Triangle> output;corners.reserve(parent.size());output.reserve(valid.size());
  for(unsigned f=0;f<valid.size();++f){Triangle t;
    for(unsigned c=0;c<3;++c){auto v=vertices[valid[f].corners[c]];v.normal=Normalize(sums[find(f*3+c)]);if(Length(v.normal)<.5f)return false;
      t.corners[c]=corners.size();corners.push_back(v);}
    output.push_back(t);
  }
  vertices=std::move(corners);triangles=std::move(output);return true;
}
struct Edge {unsigned count=0;bool forward=false,hard=false,crease=false;V3 n0,n1,face;unsigned source0=0,source1=0;};
using EdgeKey=std::pair<unsigned,unsigned>;
// Shared vertex normals do not prove a rounded surface: low-poly props often
// smooth their normals across real corners, and PN patches then inflate them
// (a rifle's receiver, sights and stock). An edge curves only if the geometry
// itself is rounded there: shallow dihedral, normals close to both faces.
inline constexpr float CurveDihedralCos=.682f; // 47 degrees; an octagon rounds, a hexagon or box does not
inline constexpr float CurveNormalCos=.766f;   // 40 degrees
// Below this share of edges that really curve, a mesh is a hard-surface model
// with a few rounded spots: patching only those looks melted, so it stays as is.
inline constexpr float CurveMinimumShare=1.f/3;
inline constexpr unsigned CurveBoundaryLimit=4; // curving edges needed per open border edge
// Cubic PN position patch, following Vlachos et al., Curved PN Triangles
// (I3D 2001), doi:10.1145/364338.364387. Boundary and hard-edge coefficients
// remain on the original straight edge on BOTH incident faces.
struct Patch {
  std::array<Vertex,3> v;
  std::array<V3,6> tangent;
  V3 center;
  Vertex Evaluate(float a,float b,float c)const{
    Vertex out;
    out.position=v[0].position*(a*a*a)+v[1].position*(b*b*b)+v[2].position*(c*c*c)+
      tangent[0]*(3*a*a*b)+tangent[1]*(3*a*b*b)+tangent[2]*(3*b*b*c)+
      tangent[3]*(3*b*c*c)+tangent[4]*(3*c*c*a)+tangent[5]*(3*c*a*a)+center*(6*a*b*c);
    out.normal=Normalize(v[0].normal*a+v[1].normal*b+v[2].normal*c);
    out.lightingNormal=v[0].lightingNormal*a+v[1].lightingNormal*b+v[2].lightingNormal*c;
    for(unsigned i=0;i<out.attributes.size();++i)
      out.attributes[i]=v[0].attributes[i]*a+v[1].attributes[i]*b+v[2].attributes[i]*c;
    return out;
  }
};
inline Result Refine(std::span<const Vertex> vertices,std::span<const Triangle> input,unsigned level,bool allowFlat=false){
  Result result;
  if(vertices.empty() || vertices.size()>16384 || input.empty() || input.size()>4096 || level<1 || level>3)return result;
  const unsigned steps=1u<<level;
  if(input.size()*steps*steps*3>65535)return result;
  std::map<PositionKey,unsigned> positions;
  std::vector<unsigned> ids;ids.reserve(vertices.size());
  for(const auto& v:vertices){
    if(!Finite(v.position)||!Finite(v.normal)||!Finite(v.lightingNormal)||Length(v.normal)<.5f||Length(v.normal)>1.5f)return {};
    for(float attribute:v.attributes)if(!std::isfinite(attribute))return {};
    auto [it,added]=positions.emplace(Key(v.position),positions.size());ids.push_back(it->second);
  }
  std::map<EdgeKey,Edge> edges;
  std::vector<Triangle> triangles;
  for(const auto& t:input){
    if(*std::max_element(t.corners.begin(),t.corners.end())>=vertices.size())return {};
    const auto& a=vertices[t.corners[0]];const auto& b=vertices[t.corners[1]];const auto& c=vertices[t.corners[2]];
    const auto cross=Cross(b.position-a.position,c.position-a.position);
    const float scale=std::max({Length(b.position-a.position),Length(c.position-b.position),Length(a.position-c.position)});
    if(scale<1e-10f || Length(cross)<scale*scale*1e-7f)continue; // strip connectors have no visible area
    const auto face=Normalize(cross);
    const float alignment=Dot(face,Normalize(a.normal));
    if(std::abs(alignment)<.3f || Dot(face,Normalize(b.normal))*alignment<.09f ||
       Dot(face,Normalize(c.normal))*alignment<.09f)return {}; // unproved/custom normal space
    triangles.push_back(t);
    for(unsigned e=0;e<3;++e){
      const unsigned i=t.corners[e],j=t.corners[(e+1)%3],pi=ids[i],pj=ids[j];
      const bool forward=pi<pj;auto& edge=edges[{std::min(pi,pj),std::max(pi,pj)}];
      const auto n0=Normalize(vertices[forward?i:j].normal),n1=Normalize(vertices[forward?j:i].normal);
      const float side=alignment<0?-1.f:1.f;
      const bool crease=Dot(face,n0)*side<CurveNormalCos || Dot(face,n1)*side<CurveNormalCos;
      if(edge.count==0){edge.forward=forward;edge.n0=n0;edge.n1=n1;edge.face=face;edge.crease=crease;edge.source0=vertices[forward?i:j].sourceIndex;edge.source1=vertices[forward?j:i].sourceIndex;}
      else{
        if(edge.count>=2 || edge.forward==forward)return {}; // non-manifold or inconsistent winding
        edge.hard=Dot(edge.n0,n0)<.98f || Dot(edge.n1,n1)<.98f || edge.crease || crease || Dot(edge.face,face)<CurveDihedralCos;
      }
      ++edge.count;
    }
  }
  if(triangles.empty())return {};
  for(auto& [key,edge]:edges){result.boundaryEdges+=edge.count==1;result.hardEdges+=edge.hard;}
  if(!allowFlat){
    // Coplanar diagonals neither curve nor crease: only edges that do either count.
    const std::vector<V3> byId=[&]{std::vector<V3> p(positions.size());for(unsigned i=0;i<vertices.size();++i)p[ids[i]]=vertices[i].position;return p;}();
    unsigned curving=0,shaped=0;
    for(const auto& [key,edge]:edges){
      if(edge.count==1 || edge.hard){++shaped;continue;}
      const auto along=byId[key.second]-byId[key.first];
      if(std::abs(Dot(along,edge.n0))+std::abs(Dot(along,edge.n1))>Length(along)*.02f){++curving;++shaped;}
    }
    result.curvingEdges=curving;
    if(float(curving)<float(shaped)*CurveMinimumShare)return {};
    // Games draw a model as many small strips, each its own open mesh here. Its
    // border stays straight, so a rounded part only bulges between its straight
    // sides and the surface comes out wavy, while the silhouette never rounds.
    if(result.boundaryEdges*CurveBoundaryLimit>curving)return {};
  }
  result.originalTriangles=triangles.size();result.vertices.reserve(triangles.size()*steps*steps*3);
  const auto emit=[&](const Patch& patch,unsigned patchIndex,std::array<float,3> a,std::array<float,3> b,std::array<float,3> c){
    for(const auto& bary:{a,b,c}){
      auto v=patch.Evaluate(bary[0],bary[1],bary[2]);
      const auto original=patch.v[0].position*bary[0]+patch.v[1].position*bary[1]+patch.v[2].position*bary[2];
      const float movement=Length(v.position-original);
      result.maximumMovement=std::max(result.maximumMovement,movement);
      result.vertices.push_back(v);result.samples.push_back({patchIndex,bary});
    }
    ++result.refinedTriangles;
  };
  float extent=0;
  for(const auto& t:triangles){
    Patch patch{{vertices[t.corners[0]],vertices[t.corners[1]],vertices[t.corners[2]]},{},{}};
    for(auto& v:patch.v)v.normal=Normalize(v.normal);
    PatchLayout layout;for(unsigned c=0;c<3;++c)layout.vertices[c]=patch.v[c].sourceIndex;
    const auto coefficient=[&](unsigned i,unsigned j,bool straight,V3 normal){
      const auto p=patch.v[i].position,q=patch.v[j].position;
      return (p*2+q-(straight?V3{}:normal*Dot(q-p,normal)))*(1.f/3);
    };
    for(unsigned e=0;e<3;++e){
      const unsigned i=t.corners[e],j=t.corners[(e+1)%3];
      const auto& edge=edges.at({std::min(ids[i],ids[j]),std::max(ids[i],ids[j])});
      const bool straight=edge.count==1 || edge.hard;
      if(straight)layout.straight|=1u<<e;
      layout.normals[e*2]=ids[i]<ids[j]?edge.source0:edge.source1;
      layout.normals[e*2+1]=ids[i]<ids[j]?edge.source1:edge.source0;
      const auto ni=ids[i]<ids[j]?edge.n0:edge.n1,nj=ids[i]<ids[j]?edge.n1:edge.n0;
      patch.tangent[e*2]=coefficient(e,(e+1)%3,straight,ni);
      patch.tangent[e*2+1]=coefficient((e+1)%3,e,straight,nj);
      extent=std::max(extent,Length(patch.v[e].position-patch.v[(e+1)%3].position));
    }
    V3 average{};for(auto point:patch.tangent)average=average+point*(1.f/6);
    const auto centroid=(patch.v[0].position+patch.v[1].position+patch.v[2].position)*(1.f/3);
    patch.center=average+(average-centroid)*.5f;
    const unsigned patchIndex=result.patches.size();result.patches.push_back(layout);
    const auto bary=[steps](unsigned i,unsigned j){return std::array<float,3>{1.f-float(i+j)/steps,float(i)/steps,float(j)/steps};};
    for(unsigned i=0;i<steps;++i)for(unsigned j=0;j<steps-i;++j){
      emit(patch,patchIndex,bary(i,j),bary(i+1,j),bary(i,j+1));
      if(i+j+1<steps)emit(patch,patchIndex,bary(i+1,j),bary(i+1,j+1),bary(i,j+1));
    }
  }
  // Flat faces do not incur extra triangles. Invalid normal data must never
  // create an unbounded bulge or modify the guest's collision geometry.
  if((!allowFlat && result.maximumMovement<extent*1e-6f) || result.maximumMovement>extent*.25f)return {};
  return result;
}
}
