// SPDX-License-Identifier: GPL-3.0-or-later
#include "../../runtime/gx/mesh_displacement.h"
#include <cstdio>
#include <cstdlib>
using namespace GekkoAOT::GX::Mesh;
static void Check(bool v,const char* text){if(!v){std::fprintf(stderr,"FAIL: %s\n",text);std::exit(1);}}
int main(){
  // Closed icosahedron with smooth radial normals: every original face is
  // flat, while its shallow dihedrals and the normals prove a rounded surface.
  const float phi=(1+std::sqrt(5.f))/2;
  std::vector<Vertex> vertices;
  for(V3 v:std::array<V3,12>{{{-1,phi,0},{1,phi,0},{-1,-phi,0},{1,-phi,0},{0,-1,phi},{0,1,phi},{0,-1,-phi},{0,1,-phi},{phi,0,-1},{phi,0,1},{-phi,0,-1},{-phi,0,1}}}){const auto n=Normalize(v);vertices.push_back({n,n,{}});}
  const std::array<Triangle,20> faces{{{{0,11,5}},{{0,5,1}},{{0,1,7}},{{0,7,10}},{{0,10,11}},{{1,5,9}},{{5,11,4}},{{11,10,2}},{{10,7,6}},{{7,1,8}},{{3,9,4}},{{3,4,2}},{{3,2,6}},{{3,6,8}},{{3,8,9}},{{4,9,5}},{{2,4,11}},{{6,2,10}},{{8,6,7}},{{9,8,1}}}};
  auto refined=Refine(vertices,faces,2);
  Check(refined.originalTriangles==20 && refined.refinedTriangles==320,"subdivision produces 16 real triangles per face");
  Check(refined.boundaryEdges==0 && refined.hardEdges==0,"closed smooth topology validated");
  Check(refined.maximumMovement>.05f,"rounded shape moves rendered vertices beyond flat triangle interiors");
  float best=0;
  for(const auto& v:refined.vertices){
    best=std::max(best,Length(v.position));
    Check(std::abs(Length(v.normal)-1)<1e-5f,"refined normals remain normalized");
    Check(Finite(v.position),"refined positions remain finite");
  }
  float flatMid=Length((vertices[0].position+vertices[5].position)*.5f);
  Check(best<=1.02f && best>flatMid,"surface moves toward the sphere and never past it");
  auto invalid=faces;invalid[1]=faces[0];Check(Refine(vertices,invalid,1).vertices.empty(),"duplicate/non-manifold winding rejected");
  vertices[0].normal={0,0,0};Check(Refine(vertices,faces,1).vertices.empty(),"missing reliable normal rejected");vertices[0].normal=Normalize(vertices[0].position);
  std::vector<Vertex> corners;std::vector<Triangle> split;
  for(unsigned t=0;t<faces.size();++t){Triangle face;for(unsigned c=0;c<3;++c){face.corners[c]=corners.size();auto v=vertices[faces[t].corners[c]];v.attributes[0]=float(t);corners.push_back(v);}split.push_back(face);}
  const auto seams=Refine(corners,split,1);
  Check(seams.refinedTriangles==80,"UV/color seams retain geometric adjacency");
  for(unsigned t=0;t<faces.size();++t)for(unsigned c=0;c<12;++c)
    Check(seams.vertices[t*12+c].attributes[0]==float(t),"per-face UV attributes are not welded across seams");
  // Hard face normals on the same mesh must keep its sharp silhouette.
  for(unsigned t=0;t<faces.size();++t){const auto& f=faces[t];const auto n=Normalize(Cross(vertices[f.corners[1]].position-vertices[f.corners[0]].position,vertices[f.corners[2]].position-vertices[f.corners[0]].position));for(unsigned c=0;c<3;++c)corners[t*3+c].normal=n;}
  Check(Refine(corners,split,2).vertices.empty(),"hard edges stay sharp without meaningless subdivision");
  // Hard-surface props with normals smoothed across real corners: a rifle part
  // is a long thin box, and PN patches would inflate it into a bulging shape.
  const auto prism=[](unsigned sides,float length,std::vector<Vertex>& out,std::vector<Triangle>& tris){
    out.clear();tris.clear();
    for(unsigned z=0;z<2;++z)for(unsigned i=0;i<sides;++i){
      const float a=6.28318531f*(i+.5f)/sides;const V3 p{std::cos(a),std::sin(a),z?length:-length};
      out.push_back({p,Normalize(p),{}});
    }
    for(unsigned i=0;i<sides;++i){const unsigned j=(i+1)%sides;
      tris.push_back({{i,j,sides+j}});tris.push_back({{i,sides+j,sides+i}});
    }
    for(unsigned i=1;i+1<sides;++i){tris.push_back({{0,i+1,i}});tris.push_back({{sides,sides+i,sides+i+1}});}
  };
  std::vector<Vertex> bar;std::vector<Triangle> barFaces;
  prism(4,6,bar,barFaces);
  Check(Refine(bar,barFaces,2).vertices.empty(),"a box with shared normals is not inflated");
  prism(6,6,bar,barFaces);
  Check(Refine(bar,barFaces,2).vertices.empty(),"a hexagonal prism with shared normals keeps its shape");
  // A rounded part drawn as one strip: its open border would stay straight and
  // its middle bulge, which turned a rifle's round handguard into a wavy shape.
  std::vector<Vertex> strip;std::vector<Triangle> stripFaces;
  for(unsigned i=0;i<12;++i){const float a=.25f*i;const V3 n{std::cos(a),std::sin(a),0};
    strip.push_back({V3{n.x,n.y,0},n,{}});strip.push_back({V3{n.x,n.y,3},n,{}});}
  for(unsigned i=0;i+1<12;++i){stripFaces.push_back({{i*2,(i+1)*2,i*2+1}});stripFaces.push_back({{i*2+1,(i+1)*2,(i+1)*2+1}});}
  Check(Refine(strip,stripFaces,2).vertices.empty(),"an open strip is never curved between straight borders");
  // A mostly hard model with one rounded area stays untouched: partial patches look melted.
  auto mixed=corners;std::vector<Triangle> mixedFaces=split;
  for(unsigned t=0;t<faces.size();++t)for(unsigned c=0;c<3;++c)mixed[t*3+c].normal=vertices[faces[t].corners[c]].normal;
  for(unsigned t=0;t<faces.size();++t)if(t!=0)for(unsigned c=0;c<3;++c)mixed[t*3+c].normal=corners[t*3+c].normal;
  Check(Refine(mixed,mixedFaces,1).vertices.empty(),"mostly hard meshes are not patched");
  // Reconstructed normals on a shallow closed mesh still prove a smooth surface.
  std::vector<Vertex> ico;for(const auto& v:vertices)ico.push_back({v.position,{},{}});
  std::vector<Triangle> icoFaces(faces.begin(),faces.end());
  Check(ReconstructNormals(ico,icoFaces),"missing normals reconstruct from oriented connected topology");
  const auto rounded=Refine(ico,icoFaces,1);
  Check(rounded.refinedTriangles==80 && rounded.maximumMovement>.05f,"reconstructed normals produce real curved vertices");
  std::vector<Vertex> octa;for(V3 p:std::array<V3,6>{{{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}}})octa.push_back({p,{},{}});
  std::vector<Triangle> hardFaces{{{{0,2,4}},{{4,2,1}},{{1,2,5}},{{5,2,0}},{{4,3,0}},{{1,3,4}},{{5,3,1}},{{0,3,5}}}};
  Check(ReconstructNormals(octa,hardFaces),"sharp no-normal mesh is valid");
  Check(Refine(octa,hardFaces,1).vertices.empty(),"steep dihedrals preserve the original hard shape");
  std::printf("MESH_REFINEMENT_PASSED triangles=20->%u max_movement=%.6f radius=%.6f\n",refined.refinedTriangles,refined.maximumMovement,best);
  std::vector<Vertex> wall(4);const std::array<V3,4> positions{{{0,0,0},{1,0,0},{1,1,0},{0,1,0}}};
  for(unsigned i=0;i<4;++i){wall[i].sourceIndex=i;wall[i].position=positions[i];wall[i].normal={0,0,1};wall[i].lightingNormal={0,0,1};wall[i].attributes[8]=positions[i].x;wall[i].attributes[9]=positions[i].y;}
  const std::array<Triangle,2> wallFaces{{{{0,1,2}},{{0,2,3}}}};
  Check(Refine(wall,wallFaces,2).vertices.empty(),"plain flat wall retains original mesh when displacement is off");
  auto displaced=Refine(wall,wallFaces,3,true);
  HeightField height{16,16,1,1,1,std::vector<std::uint8_t>(256,230)};
  Check(displaced.refinedTriangles==128,"displacement surface gets 64 real triangles per source triangle");
  Check(Displace(displaced,wall,height,.05f),"height field moves actual subdivided positions");
  unsigned moved=0;
  for(unsigned i=0;i<displaced.vertices.size();++i){const auto& v=displaced.vertices[i];const auto& b=displaced.samples[i].bary;
    if(std::min({b[0],b[1],b[2]})==0)Check(v.position.z==0,"UV seams and patch boundaries remain crack free");
    else moved+=v.position.z>0;
    Check(v.position.z<=.0283f,"displacement is locally bounded");
    Check(v.lightingNormal.z==1,"authored GX lighting attributes preserved");
  }
  Check(moved>100,"displacement moves triangle interiors beyond interpolation");

}
