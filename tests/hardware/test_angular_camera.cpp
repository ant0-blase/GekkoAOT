// SPDX-License-Identifier: GPL-3.0-or-later
#include "../../runtime/gx/angular_camera.h"
#include "../../runtime/gx/j3d_frustum.h"
#include "../../runtime/sdk/j3d_frustum_proof.h"
#include "../../runtime/sdk/angular_camera_proof.h"
#include "../../runtime/gx/tangent_camera.h"
#include "../../runtime/gx/reciprocal_camera.h"
#include "../../runtime/sdk/reciprocal_camera_proof.h"
#include "../../runtime/gx/scene_camera_selector.h"
#include <iostream>
#include <cstdlib>
#define CHECK(x) do {if(!(x)){std::cerr<<__LINE__<<": " #x "\n";std::exit(1);}}while(false)
int main(){
  using namespace GekkoAOT::GX;
  // J3D CPU side planes must match the renderer without rewriting camera
  // lens fields or near/far. A side object outside 4:3 becomes visible in 16:9.
  std::array<std::uint8_t,0x5c> clipper{};
  WriteBigEndianFloat(clipper.data()+0x4c,60);
  WriteBigEndianFloat(clipper.data()+0x50,4.f/3);
  WriteBigEndianFloat(clipper.data()+0x54,1);
  WriteBigEndianFloat(clipper.data()+0x58,1000);
  const auto lensFields=clipper;
  const float y=1.f/std::tan(60*.008726646259971648f);
  const std::array<float,6> retail{y/(4.f/3),0,y,0,-1.f/999,-1000.f/999};
  const auto outside=[&](float x,float z){return x*ReadBigEndianFloat(clipper.data()+0x1c)+z*ReadBigEndianFloat(clipper.data()+0x24)>0;};
  CHECK(AdjustJ3dFrustum(clipper.data(),retail,4.f/3,4.f/3,0));
  CHECK(outside(9,-10));
  CHECK(AdjustJ3dFrustum(clipper.data(),retail,4.f/3,16.f/9,0));
  CHECK(!outside(9,-10));CHECK(outside(11,-10));
  for(unsigned i=0x34;i<clipper.size();++i)CHECK(clipper[i]==lensFields[i]);
  const auto widenedClipper=clipper;
  for(unsigned i=0;i<100;++i)CHECK(AdjustJ3dFrustum(clipper.data(),retail,4.f/3,16.f/9,0));
  CHECK(clipper==widenedClipper); // no cumulative widening
  CHECK(AdjustJ3dFrustum(clipper.data(),retail,4.f/3,4.f/3,0));CHECK(outside(9,-10));
  auto unrelatedLens=retail;unrelatedLens[0]*=1.1f;
  const auto beforeReject=clipper;
  CHECK(!AdjustJ3dFrustum(clipper.data(),unrelatedLens,4.f/3,16.f/9,0));CHECK(clipper==beforeReject);
  CHECK(!AdjustJ3dFrustum(clipper.data(),retail,4.f/3,0,0));
  WriteBigEndianFloat(clipper.data()+0x54,100);
  CHECK(AdjustJ3dFrustum(clipper.data(),retail,4.f/3,16.f/9,0));
  CHECK(ReadBigEndianFloat(clipper.data()+0x54)==100);
  WriteBigEndianFloat(clipper.data()+0x54,1);
  // Split screens normalize guest aspect in the renderer; retain their actual
  // 8:3 lens in CPU planes while multiplying only by the host aspect change.
  WriteBigEndianFloat(clipper.data()+0x50,8.f/3);
  auto split=retail;split[0]=y/(8.f/3);
  CHECK(AdjustJ3dFrustum(clipper.data(),split,4.f/3,16.f/9,0));
  const float nx=ReadBigEndianFloat(clipper.data()+0x1c),nz=ReadBigEndianFloat(clipper.data()+0x24);
  CHECK(std::abs(nz/nx-std::tan(60*.008726646259971648f)*(32.f/9))<1e-5f);
  std::array<std::uint8_t,360> wrongBody{};
  CHECK(!GekkoAOTSdk::ProveJ3dFrustum(0x80000000,[&](auto,auto){return wrongBody.data();}));
  std::array<std::uint8_t,0xf0> camera{};
  WriteBigEndianFloat(camera.data()+16,.05f);WriteBigEndianFloat(camera.data()+20,1200);
  WriteBigEndianFloat(camera.data()+0xe4,640);WriteBigEndianFloat(camera.data()+0xe8,480);
  CHECK(SetAngularCamera(camera.data(),55,4.f/3,240));
  const auto original=AngularProjection(camera.data());
  CHECK(SameCameraProjection(original,original));
  CHECK(std::abs(original[2]-1/std::tan(55*.008726646259971648f))<1e-6);
  auto widened=camera;
  CHECK(SetAngularCamera(widened.data(),110,16.f/9,240));
  const auto p=AngularProjection(widened.data());
  CHECK(std::abs(p[2]/p[0]-16.f/9)<1e-6);
  CHECK(p[4]==original[4] && p[5]==original[5]);
  CHECK(!SameCameraProjection(p,original));
  CHECK(ReadBigEndianFloat(widened.data()+0xe4)==640); // Physical viewport retained.
  CHECK(ReadBigEndianFloat(widened.data()+0xe8)==480);
  CHECK(std::abs(ReadBigEndianFloat(widened.data()+0xec)-240/std::tan(110*.008726646259971648f))<1e-4);
  // A point culled by the retail side planes is inside the widened frustum.
  const float x=8,z=-10;
  CHECK(std::abs(original[0]*x)>-z && std::abs(p[0]*x)<-z);
  // The CPU screen projection used for shadows and the GPU use identical x/y.
  CHECK(std::abs(x/-z/std::tan(ReadBigEndianFloat(widened.data()+4))-p[0]*x/-z)<1e-6);
  CHECK(SetAngularCamera(widened.data(),55,4.f/3,240));
  CHECK(widened==camera);
  CHECK(!SetAngularCamera(widened.data(),0,1.3,240));
  CHECK(!SetAngularCamera(widened.data(),100,0,240));
  std::array<std::uint8_t,160> invalid{};
  CHECK(!GekkoAOTSdk::ProveAngularCameraSetter(0x80000000,[&](auto,auto){return invalid.data();}));
  std::array<std::uint8_t,0x144> tangent{};
  const float native_v=1/original[2],native_h=1/original[0];
  WriteBigEndianFloat(tangent.data()+0x24,.05f);WriteBigEndianFloat(tangent.data()+0x28,1200);
  WriteBigEndianFloat(tangent.data()+0x2c,native_h);WriteBigEndianFloat(tangent.data()+0x30,native_v);
  const auto native=tangent;
  TangentCameraAdjuster adjust;
  CHECK(adjust.Apply(0x80010000,tangent.data(),original,4.f/3,110,16.f/9));
  CHECK(std::abs(1/ReadBigEndianFloat(tangent.data()+0x2c)-p[0])<1e-6);
  CHECK(tangent[0x140]==1 && tangent[0x142]==0 && tangent[0x143]==0);
  const auto applied=tangent;
  CHECK(adjust.Apply(0x80010000,tangent.data(),original,4.f/3,110,16.f/9));
  CHECK(tangent==applied); // Projection caching may still expose the original lens.
  CHECK(adjust.Apply(0x80010000,tangent.data(),p,16.f/9,110,16.f/9));
  CHECK(tangent==applied); // No compounding when the game retains our lens.
  tangent=native; // The game may set its original lens before every cull pass.
  CHECK(adjust.Apply(0x80010000,tangent.data(),p,16.f/9,110,16.f/9));
  CHECK(tangent==applied);
  CHECK(adjust.Apply(0x80010000,tangent.data(),p,16.f/9,0,0));
  CHECK(ReadBigEndianFloat(tangent.data()+0x2c)==native_h);
  CHECK(ReadBigEndianFloat(tangent.data()+0x30)==native_v);
  CHECK(ReadBigEndianFloat(tangent.data()+0x24)==.05f && ReadBigEndianFloat(tangent.data()+0x28)==1200);
  auto wrong=original;wrong[0]*=1.1f;
  CHECK(!adjust.Apply(0x80010000,tangent.data(),wrong,4.f/3,110,16.f/9));
  // Submission-time repair keeps original camera bytes and near/far planes.
  std::array<std::uint8_t,96> planes{};
  std::array<std::uint8_t,16> portal{};
  const float sx=1.428148031f,sy=1.904197335f;
  auto initializePlanes=[&] {
    planes={};
    const float a=1/std::sqrt(1+sx*sx),b=1/std::sqrt(1+sy*sy);
    const std::array<float,24> values{-sx*a,0,-a,0,sx*a,0,-a,0,0,-sy*b,-b,0,0,sy*b,-b,0,0,0,-1,0,0,0,1,-1000000};
    for(unsigned i=0;i<24;++i)WriteBigEndianFloat(planes.data()+4*i,values[i]);
    const std::array<float,4> bounds{-1/sx,-1/sy,1/sx,1/sy};
    for(unsigned i=0;i<4;++i)WriteBigEndianFloat(portal.data()+4*i,bounds[i]);
  };
  initializePlanes();
  const auto retailPlanes=planes;
  CHECK(AdjustReciprocalFrustum(sx,sy,sx*.75f,sy,planes.data(),portal.data()));
  CHECK(std::equal(planes.begin()+64,planes.end(),retailPlanes.begin()+64));
  CHECK(std::abs(ReadBigEndianFloat(portal.data()+8)-1/(sx*.75f))<1e-6f);
  // A point rejected by the retail side plane lies inside the widened one.
  const float px=8,pz=-10;
  CHECK(ReadBigEndianFloat(retailPlanes.data())*px+ReadBigEndianFloat(retailPlanes.data()+8)*pz<0);
  CHECK(ReadBigEndianFloat(planes.data())*px+ReadBigEndianFloat(planes.data()+8)*pz>0);
  const auto widenedPlanes=planes;
  CHECK(!AdjustReciprocalFrustum(sx,sy,sx*.75f,sy,planes.data(),portal.data()));
  CHECK(planes==widenedPlanes); // More than one FIFO callback cannot compound.
  initializePlanes();
  const float ty=1/std::tan(100*.008726646259971648f),tx=ty/(21.f/9);
  CHECK(AdjustReciprocalFrustum(sx,sy,tx,ty,planes.data(),portal.data()));
  CHECK(std::abs(ReadBigEndianFloat(portal.data()+12)-1/ty)<1e-6f);
  initializePlanes();
  CHECK(!AdjustReciprocalFrustum(sx,sy,sx,sy,planes.data(),portal.data()));
  CHECK(planes==retailPlanes);
  portal[0]^=0x20;
  CHECK(!AdjustReciprocalFrustum(sx,sy,tx,ty,planes.data(),portal.data()));
  CHECK(planes==retailPlanes); // Unknown/corrupted bounds fail closed.
  CHECK(!AdjustReciprocalFrustum(sx,sy,0,ty,planes.data(),portal.data()));
  std::array<std::uint8_t,840> unrelated{};
  unrelated[0]=0x94;unrelated[1]=0x21;unrelated[2]=0xff;unrelated[3]=0xa8;
  CHECK(!GekkoAOTSdk::ProveReciprocalCameraFrustum(0x80000000,[&](auto,auto){return unrelated.data();}));
  CHECK(!GekkoAOTSdk::ProveReciprocalCameraFrustum(0x80000000,[](auto,auto)->const std::uint8_t*{return nullptr;}));
  SceneCameraSelector selector;
  const std::array<float,10> world_key{1,0,1,0,0,-1,0,0,640,480};
  auto weapon_key=world_key;weapon_key[0]=weapon_key[2]=1.7f;
  CHECK(selector.Observe(1,world_key,1,120));
  CHECK(!selector.Observe(1,weapon_key,1,12));
  CHECK(!selector.Observe(1,weapon_key,1,12));
  CHECK(selector.Observe(2,world_key,1,3));
  CHECK(!selector.Observe(2,weapon_key,.1f,1000));
  CHECK(selector.Observe(3,weapon_key,1,12)); // A new VI frame can change camera.
  std::cout<<"Angular camera projection, CPU frustum, screen coordinates and restore passed\n";
}
