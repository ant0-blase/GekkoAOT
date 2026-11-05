// SPDX-License-Identifier: GPL-3.0-or-later
#include "gekkoaot_mesh_resources.hpp"
#include "mesh_refinement.h"
#include "gekkoaot_hd_resources.hpp"
#include "gekkoaot_enhanced_renderer.hpp"
#include "../internal.hpp"
#include "recording.hpp"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <fstream>
#include <filesystem>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <xxhash.h>
namespace aurora::gfx::geometry {
namespace {
using namespace GekkoAOT::GX::Mesh;
std::atomic<unsigned> quality{0};
std::atomic<bool> displacement{false};std::atomic<float> displacementDepth{.015f};
std::atomic<bool> compareMode{false};
thread_local unsigned comparisonHalf=0;
thread_local bool comparisonOriginal=false;
thread_local bool canonical=false;
std::uint64_t appliedDraws=0,originalTriangles=0,refinedTriangles=0,poseUpdates=0,poseNanoseconds=0;
struct {std::uint64_t boundary=0,hard=0,curving=0;} edgeStats; // mesh worker only
struct Attribute {
  unsigned offset=0,count=0,bytes=0,first=0,stride=0;
  GXAttrType mode=GX_NONE;gx::VtxAttrFmt format{};bool le=false;
  std::span<const std::uint8_t> view;
  std::vector<std::uint8_t> owned;
};
struct Source {
  std::shared_ptr<const HeightField> height;float displacementAmount=0;
  GXPrimitive primitive;unsigned count=0,stride=0,level=1;bool negativeEligible=true;
  std::vector<std::uint8_t> raw;
  std::array<Attribute,gx::MaxVtxAttr> attributes;
};
struct Entry {
  std::shared_ptr<Source> source;
  std::shared_ptr<const Prepared> result;
  std::uint64_t used=0,shape=0,asset=0,poseKey=0;
  std::shared_ptr<const Prepared> pose;std::size_t bytes=0;bool pending=true;
};
std::uint32_t Read(const std::uint8_t* p,unsigned n,bool le){
  std::uint32_t v=0;for(unsigned i=0;i<n;++i)v|=std::uint32_t(p[i])<<(8*(le?i:n-1-i));return v;
}
float Scalar(const std::uint8_t* p,GXCompType type,unsigned frac,bool le){
  switch(type){
  case GX_U8:return std::ldexp(float(*p),-int(frac));
  case GX_S8:return std::ldexp(float(std::int8_t(*p)),-int(frac));
  case GX_U16:return std::ldexp(float(Read(p,2,le)),-int(frac));
  case GX_S16:return std::ldexp(float(std::int16_t(Read(p,2,le))),-int(frac));
  case GX_F32:return std::bit_cast<float>(Read(p,4,le));
  default:return NAN;
  }
}
std::array<float,4> Color(const std::uint8_t* p,GXCompType type,bool le){
  const auto bits=Read(p,type==GX_RGB565||type==GX_RGBA4?2:type==GX_RGB8||type==GX_RGBA6?3:4,le);
  switch(type){
  case GX_RGB565:return {float(bits>>11)/31,float((bits>>5)&63)/63,float(bits&31)/31,1};
  case GX_RGB8:return {float(bits>>16)/255,float((bits>>8)&255)/255,float(bits&255)/255,1};
  case GX_RGBX8:return {float(bits>>24)/255,float((bits>>16)&255)/255,float((bits>>8)&255)/255,1};
  case GX_RGBA4:return {float(bits>>12)/15,float((bits>>8)&15)/15,float((bits>>4)&15)/15,float(bits&15)/15};
  case GX_RGBA6:return {float(bits>>18)/63,float((bits>>12)&63)/63,float((bits>>6)&63)/63,float(bits&63)/63};
  case GX_RGBA8:return {float(bits>>24)/255,float((bits>>16)&255)/255,float((bits>>8)&255)/255,float(bits&255)/255};
  default:return {};
  }
}
void Float(std::vector<std::uint8_t>& out,float v){const auto u=std::bit_cast<std::uint32_t>(v);for(int i=3;i>=0;--i)out.push_back(u>>(i*8));}
bool Decode(Source& source,std::span<const std::uint8_t> raw,std::vector<Vertex>& input,Prepared& layout){
  input.assign(source.count,{});for(unsigned v=0;v<source.count;++v)input[v].sourceIndex=v;
  auto* prepared=&layout;
  prepared->matrixIndices.fill(255);
  for(unsigned attr=0;attr<GX_VA_POS;++attr)if(source.attributes[attr].mode!=GX_NONE){
    prepared->descriptors[attr]=GX_DIRECT;
    prepared->matrixIndices[attr]=raw[source.attributes[attr].offset];
  }
  for(unsigned attr=GX_VA_POS;attr<=GX_VA_TEX7;++attr){
    const auto& a=source.attributes[attr];if(a.mode==GX_NONE)continue;
    prepared->descriptors[attr]=GX_DIRECT;
    auto& f=prepared->format.attrs[attr];f=a.format;f.frac=0;
    f.type=(attr==GX_VA_CLR0||attr==GX_VA_CLR1)?GX_RGBA8:GX_F32;
    if(attr==GX_VA_CLR0||attr==GX_VA_CLR1)f.cnt=GX_CLR_RGBA;
    for(unsigned v=0;v<source.count;++v){
      const auto* p=raw.data()+v*source.stride+a.offset;
      if(a.mode!=GX_DIRECT){const auto index=Read(p,a.mode==GX_INDEX8?1:2,false);p=(a.owned.empty()?a.view.data():a.owned.data())+(index-a.first)*a.stride;}
      auto& vertex=input[v];
      if(attr==GX_VA_CLR0||attr==GX_VA_CLR1){
        const auto color=Color(p,a.format.type,a.le);std::copy(color.begin(),color.end(),vertex.attributes.begin()+(attr-GX_VA_CLR0)*4);
      }else{
        const unsigned bytes=gx::comp_type_size(GXAttr(attr),a.format.type);
        const unsigned frac=attr==GX_VA_NRM?(a.format.type==GX_S8?6:a.format.type==GX_S16?14:0):a.format.frac;
        float values[3]{};
        for(unsigned c=0;c<a.count;++c)values[c]=Scalar(p+c*bytes,a.format.type,frac,a.le);
        if(attr==GX_VA_POS)vertex.position={values[0],values[1],values[2]};
        else if(attr==GX_VA_NRM){vertex.normal={values[0],values[1],values[2]};vertex.lightingNormal=vertex.normal;vertex.normal=Normalize(vertex.normal);}
        else for(unsigned c=0;c<a.count;++c){if(!std::isfinite(values[c])){source.negativeEligible=false;return false;}vertex.attributes[8+(attr-GX_VA_TEX0)*2+c]=values[c];}
      }
    }
  }
  return true;
}
void Bounds(Prepared& mesh,std::span<const Vertex> input){
  V3 low=input[0].position,high=low;
  for(const auto& v:input){low={std::min(low.x,v.position.x),std::min(low.y,v.position.y),std::min(low.z,v.position.z)};
    high={std::max(high.x,v.position.x),std::max(high.y,v.position.y),std::max(high.z,v.position.z)};}
  const auto center=(low+high)*.5f;mesh.center={center.x,center.y,center.z};mesh.radius=Length(high-low)*.5f;
}
void Encode(Prepared& mesh,unsigned level,std::span<const Vertex> vertices){
  auto& bytes=mesh.vertices[level];
  unsigned stride=12+(mesh.descriptors[GX_VA_NRM]!=GX_NONE?12:0);
  for(unsigned attr=0;attr<GX_VA_POS;++attr)stride+=mesh.descriptors[attr]!=GX_NONE;
  for(unsigned attr=GX_VA_CLR0;attr<=GX_VA_TEX7;++attr)if(mesh.descriptors[attr]!=GX_NONE)
    stride+=attr<=GX_VA_CLR1?4:gx::comp_cnt_count(GXAttr(attr),mesh.format.attrs[attr].cnt)*4;
  bytes.reserve(vertices.size()*stride);
  for(const auto& vertex:vertices){
    for(unsigned attr=0;attr<GX_VA_POS;++attr)if(mesh.descriptors[attr]!=GX_NONE)bytes.push_back(mesh.matrixIndices[attr]);
    Float(bytes,vertex.position.x);Float(bytes,vertex.position.y);Float(bytes,vertex.position.z);
    if(mesh.descriptors[GX_VA_NRM]!=GX_NONE){Float(bytes,vertex.lightingNormal.x);Float(bytes,vertex.lightingNormal.y);Float(bytes,vertex.lightingNormal.z);}
    for(unsigned attr=GX_VA_CLR0;attr<=GX_VA_TEX7;++attr){
      if(mesh.descriptors[attr]==GX_NONE)continue;
      if(attr<=GX_VA_CLR1)for(unsigned c=0;c<4;++c)bytes.push_back(std::uint8_t(std::lround(std::clamp(vertex.attributes[(attr-GX_VA_CLR0)*4+c],0.f,1.f)*255)));
      else for(unsigned c=0;c<gx::comp_cnt_count(GXAttr(attr),mesh.format.attrs[attr].cnt);++c)Float(bytes,vertex.attributes[8+(attr-GX_VA_TEX0)*2+c]);
    }
  }
  mesh.counts[level]=vertices.size();
}
std::shared_ptr<Prepared> Build(Source& source){
  std::vector<Vertex> input;auto prepared=std::make_shared<Prepared>();
  if(!Decode(source,source.raw,input,*prepared))return {};
  // Reject flat cards even when their normal attributes fake curvature.
  // This prevents HUD cards, billboards, sprites and text from acquiring
  // geometry just because they were submitted through a perspective lens.
  const auto origin=input[0].position;unsigned farthest=0;float extent=0;
  for(unsigned i=1;i<input.size();++i){const float distance=Length(input[i].position-origin);if(distance>extent){extent=distance;farthest=i;}}
  V3 plane{};float area=0;
  for(const auto& v:input){const auto candidate=Cross(input[farthest].position-origin,v.position-origin);const float size=Length(candidate);if(size>area){area=size;plane=candidate;}}
  if(extent<1e-8f || area<extent*extent*1e-6f)return {};
  plane=Normalize(plane);float depth=0;for(const auto& v:input)depth=std::max(depth,std::abs(Dot(v.position-origin,plane)));
  if(depth<extent*.0001f && !source.height)return {};
  prepared->reference=input;prepared->animated=source.attributes[GX_VA_NRM].mode!=GX_NONE;
  std::map<PositionKey,unsigned> welds;
  for(unsigned i=0;i<input.size();++i){auto [it,added]=welds.emplace(Key(input[i].position),i);if(!added)prepared->welds.emplace_back(it->second,i);}
  std::vector<Triangle> triangles;
  if(source.primitive==GX_TRIANGLES){if(source.count%3)return {};for(unsigned i=0;i<source.count;i+=3)triangles.push_back({{i,i+1,i+2}});}
  else if(source.primitive==GX_QUADS){if(source.count%4)return {};for(unsigned i=0;i<source.count;i+=4){triangles.push_back({{i,i+1,i+2}});triangles.push_back({{i,i+2,i+3}});}}
  else for(unsigned i=2;i<source.count;++i){
    if(source.primitive==GX_TRIANGLEFAN)triangles.push_back({{0,i-1,i}});
    else triangles.push_back(i&1?Triangle{{i-1,i-2,i}}:Triangle{{i-2,i-1,i}});
  }
  if(source.attributes[GX_VA_NRM].mode==GX_NONE && !ReconstructNormals(input,triangles))return {};
  Bounds(*prepared,input);
  for(unsigned level=1;level<=source.level;++level){
    auto result=Refine(input,triangles,source.height?level+1:level,bool(source.height));
    if(level==1){edgeStats.boundary+=result.boundaryEdges;edgeStats.hard+=result.hardEdges;edgeStats.curving+=result.curvingEdges;}
    if(level==1 && result.vertices.empty() && source.attributes[GX_VA_NRM].mode!=GX_NONE){
      // Some games deliberately use normals in a custom lighting space.
      // Reconstruct ONLY the geometric normals; their GX lighting normals
      // remain a distinct interpolated attribute with the original XF basis.
      prepared->animated=false;
      if(ReconstructNormals(input,triangles))result=Refine(input,triangles,level);
    }
    if(result.vertices.empty())continue;
    if(source.height){
      if(!Displace(result,input,*source.height,source.displacementAmount))continue;
      prepared->displaced=true;
    }
    prepared->originalTriangles=result.originalTriangles;
    Encode(*prepared,level-1,result.vertices);
    prepared->patches[level-1]=std::move(result.patches);prepared->samples[level-1]=std::move(result.samples);
  }
  if(!prepared->counts[0]){
    static unsigned dumps=0;static const char* directory=std::getenv("GEKKOAOT_MESH_DUMP_DIR");
    if(directory && dumps<16){
      std::error_code ec;std::filesystem::create_directories(directory,ec);
      const auto path=std::filesystem::path(directory)/("mesh-v3-"+std::to_string(dumps++)+".bin");
      std::ofstream file(path,std::ios::binary);const std::uint32_t header[]={3,sizeof(Vertex),unsigned(input.size()),unsigned(triangles.size()),unsigned(source.primitive),unsigned(source.attributes[GX_VA_NRM].mode)};
      file.write(reinterpret_cast<const char*>(header),sizeof(header));file.write(reinterpret_cast<const char*>(input.data()),input.size()*sizeof(Vertex));file.write(reinterpret_cast<const char*>(triangles.data()),triangles.size()*sizeof(Triangle));
      std::fprintf(stderr,"GEKKOAOT_MESH_REJECT_SAMPLE vertices=%zu triangles=%zu normals=%u path=%s\n",input.size(),triangles.size(),unsigned(source.attributes[GX_VA_NRM].mode),path.string().c_str());
    }
  }
  return prepared->counts[0]?prepared:nullptr;
}
float ProjectedPixels(const Prepared& mesh) noexcept{
  // Current object-to-view transform, never included in the stable mesh key:
  // rigid animation shares one refined object-space mesh across every frame.
  const auto& state=gx::g_gxState;const auto index=mesh.matrixIndices[GX_VA_PNMTXIDX]<64?mesh.matrixIndices[GX_VA_PNMTXIDX]:state.currentPnMtx;
  if(index+2>=gx::MaxXfPosRows)return 0.f;
  const auto& rows=state.xfPosRows;
  const auto transform=[&](const auto& row){return row[0]*mesh.center[0]+row[1]*mesh.center[1]+row[2]*mesh.center[2]+row[3];};
  const float z=transform(rows[index+2]);
  float scale=0;
  for(unsigned r=0;r<3;++r){const auto& row=rows[index+r];scale=std::max(scale,std::sqrt(row[0]*row[0]+row[1]*row[1]+row[2]*row[2]));}
  const float radius=mesh.radius*scale;
  if(!std::isfinite(z)||!std::isfinite(radius)||radius<=0 || z>=radius)return 0.f;
  return std::abs(state.proj[1][1])*radius/std::max(-z,radius*.1f)*state.logicalViewport.height;
}
std::shared_ptr<Prepared> Pose(const Prepared& reference,Source& source,std::span<const std::uint8_t> raw){
  if(reference.displaced || !reference.animated || reference.reference.size()!=source.count)return {};
  thread_local std::vector<Vertex> input;
  auto mesh=std::make_shared<Prepared>();if(!Decode(source,raw,input,*mesh))return {};
  Bounds(*mesh,input);mesh->originalTriangles=reference.originalTriangles;
  const float tolerance=std::max(mesh->radius*.0002f,1e-5f);
  for(auto [a,b]:reference.welds)if(Length(input[a].position-input[b].position)>tolerance)return {};
  thread_local std::vector<Vertex> output;
  thread_local std::vector<Patch> patches;
  const unsigned wanted=quality.load()>1 && !reference.samples[1].empty() && ProjectedPixels(*mesh)>=140?1:0;
  for(unsigned level=0;level<2;++level){
    if(level!=wanted)continue;
    if(reference.samples[level].empty())continue;
    patches.clear();patches.reserve(reference.patches[level].size());
    for(const auto& layout:reference.patches[level]){
      Patch patch{{input[layout.vertices[0]],input[layout.vertices[1]],input[layout.vertices[2]]},{},{}};
      for(const auto& v:patch.v)if(!Finite(v.position)||!Finite(v.normal)||!Finite(v.lightingNormal)||Length(v.normal)<.5f)return {};
      const auto cross=Cross(patch.v[1].position-patch.v[0].position,patch.v[2].position-patch.v[0].position);
      if(Length(cross)<1e-12f)return {};
      const auto face=Normalize(cross);const float alignment=Dot(face,patch.v[0].normal);
      if(std::abs(alignment)<.3f || Dot(face,patch.v[1].normal)*alignment<.09f || Dot(face,patch.v[2].normal)*alignment<.09f)return {};
      float referenceLength=0,currentLength=0;
      for(unsigned e=0;e<3;++e){
        referenceLength+=Length(reference.reference[layout.vertices[e]].position-reference.reference[layout.vertices[(e+1)%3]].position);
        currentLength+=Length(patch.v[e].position-patch.v[(e+1)%3].position);
      }
      if(referenceLength<1e-10f)return {};
      const float scale=currentLength/referenceLength;
      for(unsigned e=0;e<3;++e){
        const auto p=patch.v[e].position,q=patch.v[(e+1)%3].position;
        const float edge=Length(q-p),originalEdge=Length(reference.reference[layout.vertices[e]].position-reference.reference[layout.vertices[(e+1)%3]].position);
        if(edge<originalEdge*scale*.65f || edge>originalEdge*scale*1.35f)return {};
        const bool straight=(layout.straight&(1u<<e))!=0;
        const auto n=input[layout.normals[e*2]].normal,m=input[layout.normals[e*2+1]].normal;
        if(!straight && (Dot(n,patch.v[e].normal)<.98f || Dot(m,patch.v[(e+1)%3].normal)<.98f))return {};
        patch.tangent[e*2]=(p*2+q-(straight?V3{}:n*Dot(q-p,n)))*(1.f/3);
        patch.tangent[e*2+1]=(q*2+p-(straight?V3{}:m*Dot(p-q,m)))*(1.f/3);
      }
      V3 average{};for(auto point:patch.tangent)average=average+point*(1.f/6);
      const auto center=(patch.v[0].position+patch.v[1].position+patch.v[2].position)*(1.f/3);
      patch.center=average+(average-center)*.5f;patches.push_back(patch);
    }
    output.clear();output.reserve(reference.samples[level].size());
    for(const auto& sample:reference.samples[level])output.push_back(patches[sample.patch].Evaluate(sample.bary[0],sample.bary[1],sample.bary[2]));
    Encode(*mesh,level,output);
  }
  return mesh->counts[0]||mesh->counts[1]?mesh:nullptr;
}
std::size_t Bytes(const Prepared& mesh){
  std::size_t n=mesh.reference.size()*sizeof(Vertex)+mesh.welds.size()*sizeof(std::pair<unsigned,unsigned>);
  for(unsigned l=0;l<2;++l)n+=mesh.vertices[l].size()+mesh.samples[l].size()*sizeof(Sample)+mesh.patches[l].size()*sizeof(PatchLayout);
  return n;
}
class Cache {
  std::mutex mutex;std::condition_variable condition;bool stopping=false;
  std::unordered_map<std::uint64_t,std::shared_ptr<Entry>> entries;
  std::unordered_map<std::uint64_t,std::weak_ptr<Entry>> templates;
  std::deque<std::shared_ptr<Entry>> queue;
  std::unordered_set<std::uint64_t> rejectedShapes;std::deque<std::uint64_t> rejectedOrder;
  std::uint64_t lastFrame=~std::uint64_t(0);unsigned admissions=0;
  std::thread worker;
  std::size_t bytes=0,pendingBytes=0;std::uint64_t tick=0,built=0,rejected=0,hits=0;
  double milliseconds=0;
  void Run(){
    for(;;){
      std::shared_ptr<Entry> entry;
      {std::unique_lock lock(mutex);condition.wait(lock,[&]{return stopping||!queue.empty();});if(stopping)return;entry=queue.front();queue.pop_front();}
      const auto start=std::chrono::steady_clock::now();std::shared_ptr<Prepared> result;
      try{result=Build(*entry->source);}catch(...){}
      std::lock_guard lock(mutex);
      milliseconds+=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
      const bool negativeEligible=entry->source->negativeEligible;
      pendingBytes-=entry->bytes;entry->source.reset();entry->pending=false;entry->result=std::move(result);entry->bytes=0;
      if(entry->result){entry->bytes=Bytes(*entry->result);bytes+=entry->bytes;++built;
        if(entry->result->animated){
          if(templates.size()>2048)for(auto it=templates.begin();it!=templates.end();)if(it->second.expired())it=templates.erase(it);else ++it;
          templates[entry->asset]=entry;
        }
        if(built<=8)std::fprintf(stderr,"GEKKOAOT_MESH_READY original_triangles=%u refined_triangles=%u/%u bytes=%zu collision=original\n",entry->result->originalTriangles,entry->result->counts[0]/3,entry->result->counts[1]/3,entry->bytes);
      }else {
        ++rejected;
        if(negativeEligible && rejectedShapes.insert(entry->shape).second){
          rejectedOrder.push_back(entry->shape);
          if(rejectedOrder.size()>16384){rejectedShapes.erase(rejectedOrder.front());rejectedOrder.pop_front();}
        }
      }
    }
  }
public:
  Cache():worker([this]{Run();}){}
  ~Cache(){Stop();}
  void Stop(){
    {std::lock_guard lock(mutex);if(stopping)return;stopping=true;}condition.notify_one();if(worker.joinable())worker.join();
    std::fprintf(stderr,"GEKKOAOT_MESH_EDGES boundary=%llu hard=%llu curving=%llu\n",(unsigned long long)edgeStats.boundary,(unsigned long long)edgeStats.hard,(unsigned long long)edgeStats.curving);
    std::fprintf(stderr,"GEKKOAOT_MESH_CACHE built=%llu rejected=%llu hits=%llu cpu_generation_ms=%.3f cached_bytes=%zu pending_bytes=%zu\n",(unsigned long long)built,(unsigned long long)rejected,(unsigned long long)hits,milliseconds,bytes,pendingBytes);
  }
  std::shared_ptr<const Prepared> Existing(std::uint64_t key){
    std::lock_guard lock(mutex);auto it=entries.find(key);if(it==entries.end())return {};
    it->second->used=++tick;if(it->second->result)++hits;return it->second->result;
  }
  std::pair<std::shared_ptr<const Prepared>,std::shared_ptr<const Prepared>> Template(std::uint64_t asset,std::uint64_t key){
    std::lock_guard lock(mutex);auto it=templates.find(asset);if(it==templates.end())return {};
    auto entry=it->second.lock();if(!entry){templates.erase(it);return {};}
    entry->used=++tick;if(entry->poseKey==key&&entry->pose){++hits;return {entry->result,entry->pose};}
    return {entry->result,{}};
  }
  void PublishPose(std::uint64_t asset,std::uint64_t key,std::shared_ptr<const Prepared> pose){
    std::lock_guard lock(mutex);auto it=templates.find(asset);if(it==templates.end())return;
    if(auto entry=it->second.lock()){
      if(entry->pose){const auto n=Bytes(*entry->pose);bytes-=n;entry->bytes-=n;}
      const auto n=Bytes(*pose);entry->pose.reset();entry->poseKey=0;
      if(bytes+n<=64ull<<20){bytes+=n;entry->bytes+=n;entry->pose=std::move(pose);entry->poseKey=key;}++hits;
    }
  }
  template<class Snapshot> std::shared_ptr<const Prepared> Get(std::uint64_t key,std::uint64_t shape,std::uint64_t asset,std::size_t size,Snapshot&& snapshot){
    std::lock_guard lock(mutex);++tick;
    if(auto it=entries.find(key);it!=entries.end()){it->second->used=tick;if(it->second->result)++hits;return it->second->result;}
    if(rejectedShapes.contains(shape))return {};
    const auto frame=gekkoaot_recording_frame_id();if(lastFrame!=frame){lastFrame=frame;admissions=0;}
    // Bound preprocessing work even if CPU-lit vertex colors or streamed
    // buffers change every draw. Cache hits are never subject to this budget.
    if(admissions>=4)return {};
    while(entries.size()>=1024 || bytes>64ull<<20){
      auto oldest=entries.end();for(auto it=entries.begin();it!=entries.end();++it)if(!it->second->pending&&(oldest==entries.end()||it->second->used<oldest->second->used))oldest=it;
      if(oldest==entries.end())return {};bytes-=oldest->second->bytes;entries.erase(oldest);
    }
    if(stopping||queue.size()>=16||pendingBytes+size>8ull<<20)return {};
    auto entry=std::make_shared<Entry>();entry->source=snapshot();entry->bytes=size;entry->used=tick;entry->shape=shape;entry->asset=asset;pendingBytes+=size;++admissions;
    entries.emplace(key,entry);queue.push_back(entry);condition.notify_one();return {};
  }
};
std::unique_ptr<Cache> cache;
}
void configure(unsigned value,bool compare,bool displace,float depth) noexcept{
  displacement=displace;displacementDepth=depth;
  if(displace)value=std::max(value,2u);
  compareMode=compare;
  value=std::min(value,2u);
  if(quality.exchange(value)!=value)std::fprintf(stderr,"GEKKOAOT_MESH_CONFIG quality=%u async=1 cache_budget_mib=64 pending_budget_mib=8\n",value);
}
bool comparison_enabled() noexcept{return compareMode.load()&&quality.load()!=0;}
bool comparison_original() noexcept{return comparisonOriginal;}
ClipRect comparison_scissor(ClipRect clip) noexcept{
  if(!comparisonHalf)return clip;
  const auto& viewport=gx::g_gxState.renderViewport;
  const int origin=int(std::lround(viewport.left)),limit=int(std::lround(viewport.left+viewport.width));
  const int middle=(origin+limit)/2;
  const int left=comparisonHalf==1?origin:middle,right=comparisonHalf==1?middle:limit;
  const int start=std::clamp(clip.x,left,right),end=std::clamp(clip.x+clip.width,left,right);
  clip.x=start;clip.width=std::max(0,end-start);return clip;
}
ComparisonScope::ComparisonScope(unsigned half,bool original):half_(comparisonHalf),canonical_(canonical),original_(comparisonOriginal),clip_(gx::g_gxState.renderScissor){
  comparisonHalf=half;comparisonOriginal=original;canonical=original;
  gx::set_render_scissor(clip_);
}
ComparisonScope::~ComparisonScope(){comparisonHalf=half_;canonical=canonical_;comparisonOriginal=original_;gx::set_render_scissor(clip_);}
bool enabled() noexcept{return quality.load()!=0&&!canonical;}
bool canonical_active() noexcept{return canonical;}
std::shared_ptr<const Prepared> request(GXPrimitive primitive,GXVtxFmt fmt,unsigned count,std::span<const std::uint8_t> raw){
  // Geometry admission is independent of screen-space receiver admission:
  // first-person models can deliberately disable depth tests. Orthographic
  // interfaces stay untouched; the worker proves volumetric topology before
  // refining any perspective draw (including copied scene render targets).
  if(!enabled()||gx::g_gxState.projType!=GX_PERSPECTIVE||count<3||count>4096||
     (primitive!=GX_TRIANGLES&&primitive!=GX_TRIANGLESTRIP&&primitive!=GX_TRIANGLEFAN&&primitive!=GX_QUADS))return {};
  auto& state=gx::g_gxState;
  // Alpha-blended draws that leave depth alone (particles, smoke, glass)
  // have no silhouette to refine, and particle vertices are new every frame:
  // Medal of Honor spent 16% of its samples hashing thousands of them per frame
  // for the worker to reject each one.
  if(state.blendMode==GX_BM_BLEND && !state.depthUpdate)return {};
  if(state.vtxDesc[GX_VA_POS]==GX_NONE ||
     gx::comp_cnt_count(GX_VA_POS,state.vtxFmts[fmt].attrs[GX_VA_POS].cnt)!=3 ||
     (state.vtxDesc[GX_VA_NRM]!=GX_NONE && gx::comp_cnt_count(GX_VA_NRM,state.vtxFmts[fmt].attrs[GX_VA_NRM].cnt)!=3))return {};
  Source source;
  if(displacement.load() && state.vtxDesc[GX_VA_TEX0]!=GX_NONE){source.height=gx::displacement_height_for_draw();source.displacementAmount=displacementDepth.load();}
  source.primitive=primitive;source.count=count;source.level=quality.load();
  thread_local std::unique_ptr<XXH3_state_t,decltype(&XXH3_freeState)> hashOwner(XXH3_createState(),XXH3_freeState);
  auto* hash=hashOwner.get();if(!hash)return {};
  XXH3_64bits_reset(hash);XXH3_64bits_update(hash,raw.data(),raw.size());XXH3_64bits_update(hash,&primitive,sizeof(primitive));XXH3_64bits_update(hash,&source.level,sizeof(source.level));
  std::size_t total=raw.size();
  for(unsigned a=0;a<GX_VA_POS;++a){
    auto& attr=source.attributes[a];attr.mode=state.vtxDesc[a];attr.offset=source.stride;
    XXH3_64bits_update(hash,&attr.mode,sizeof(attr.mode));
    if(attr.mode==GX_NONE)continue;
    if(attr.mode!=GX_DIRECT)return {};
    ++source.stride;
  }
  for(unsigned a=GX_VA_POS;a<=GX_VA_TEX7;++a){
    auto& attr=source.attributes[a];attr.mode=state.vtxDesc[a];attr.format=state.vtxFmts[fmt].attrs[a];attr.offset=source.stride;
    XXH3_64bits_update(hash,&attr.mode,sizeof(attr.mode));XXH3_64bits_update(hash,&attr.format,sizeof(attr.format));
    if(attr.mode==GX_NONE)continue;
    attr.count=gx::comp_cnt_count(GXAttr(a),attr.format.cnt);attr.bytes=gx::comp_type_size(GXAttr(a),attr.format.type)*attr.count;
    if(attr.count>3)return {};
    source.stride+=attr.mode==GX_DIRECT?attr.bytes:attr.mode==GX_INDEX8?1:2;
  }
  if(std::size_t(source.stride)*count!=raw.size())return {};
  for(unsigned a=0;a<GX_VA_POS;++a){
    const auto& attr=source.attributes[a];if(attr.mode==GX_NONE)continue;
    const auto matrix=raw[attr.offset];
    if(matrix>=64)return {};
    for(unsigned v=1;v<count;++v)if(raw[v*source.stride+attr.offset]!=matrix)return {};
  }
  for(unsigned a=GX_VA_POS;a<=GX_VA_TEX7;++a){
    auto& attr=source.attributes[a];if(attr.mode==GX_NONE||attr.mode==GX_DIRECT)continue;
    unsigned minimum=65535,maximum=0;
    for(unsigned v=0;v<count;++v){const auto index=Read(raw.data()+v*source.stride+attr.offset,attr.mode==GX_INDEX8?1:2,false);minimum=std::min(minimum,index);maximum=std::max(maximum,index);}
    const auto& array=state.arrays[a];const std::size_t first=std::size_t(minimum)*array.stride,end=std::size_t(maximum)*array.stride+attr.bytes;
    if(!array.data||!array.stride||end>array.size||end-first>65536)return {};
    attr.first=minimum;attr.stride=array.stride;attr.le=array.le;
    attr.view={static_cast<const std::uint8_t*>(array.data)+first,end-first};total+=attr.view.size();
    XXH3_64bits_update(hash,attr.view.data(),attr.view.size());XXH3_64bits_update(hash,&attr.le,sizeof(attr.le));XXH3_64bits_update(hash,&attr.stride,sizeof(attr.stride));
  }
  if(total>256*1024)return {};
  // Most draws repeat from frame to frame: the exact-content key alone finds them.
  if(source.height){XXH3_64bits_update(hash,&source.height->identity,sizeof(source.height->identity));XXH3_64bits_update(hash,&source.displacementAmount,sizeof(source.displacementAmount));}
  const auto key=XXH3_64bits_digest(hash);
  if(!cache)cache=std::make_unique<Cache>();
  if(auto existing=cache->Existing(key)){
    static unsigned logged=0;if(existing->displaced && logged++<8)std::fprintf(stderr,"GEKKOAOT_DISPLACEMENT_APPLIED original_triangles=%u output_vertices=%u/%u amplitude=%.4f\n",existing->originalTriangles,existing->counts[0],existing->counts[1],source.displacementAmount);
    return existing;
  }
  // Topology rejection depends on position/normal content and connectivity,
  // not on animated UVs or CPU-lit colors. Keep those identities separate.
  thread_local std::unique_ptr<XXH3_state_t,decltype(&XXH3_freeState)> shapeOwner(XXH3_createState(),XXH3_freeState);
  auto* shape=shapeOwner.get();if(!shape)return {};
  XXH3_64bits_reset(shape);XXH3_64bits_update(shape,&primitive,sizeof(primitive));XXH3_64bits_update(shape,&count,sizeof(count));XXH3_64bits_update(shape,&source.level,sizeof(source.level));
  for(unsigned a=0;a<=GX_VA_NRM;++a){
    const auto& attr=source.attributes[a];
    XXH3_64bits_update(shape,&attr.mode,sizeof(attr.mode));XXH3_64bits_update(shape,&attr.format,sizeof(attr.format));
    if(attr.mode==GX_NONE)continue;
    const unsigned width=a<GX_VA_POS?1:attr.mode==GX_DIRECT?attr.bytes:attr.mode==GX_INDEX8?1:2;
    for(unsigned v=0;v<count;++v)XXH3_64bits_update(shape,raw.data()+v*source.stride+attr.offset,width);
    XXH3_64bits_update(shape,&attr.le,sizeof(attr.le));XXH3_64bits_update(shape,&attr.stride,sizeof(attr.stride));
    if(!attr.view.empty())XXH3_64bits_update(shape,attr.view.data(),attr.view.size());
  }
  // Asset identity excludes the current pose and CPU-lit colors. Indexed UVs
  // and index connectivity remain part of it. A cached stencil is accepted
  // only after checking its current welded positions and face/normal geometry.
  thread_local std::unique_ptr<XXH3_state_t,decltype(&XXH3_freeState)> assetOwner(XXH3_createState(),XXH3_freeState);
  auto* asset=assetOwner.get();if(!asset)return {};
  XXH3_64bits_reset(asset);XXH3_64bits_update(asset,&primitive,sizeof(primitive));XXH3_64bits_update(asset,&count,sizeof(count));XXH3_64bits_update(asset,&source.level,sizeof(source.level));
  for(unsigned a=0;a<=GX_VA_TEX7;++a){
    const auto& attr=source.attributes[a];XXH3_64bits_update(asset,&attr.mode,sizeof(attr.mode));XXH3_64bits_update(asset,&attr.format,sizeof(attr.format));
    if(attr.mode==GX_NONE || a<GX_VA_POS || a==GX_VA_CLR0 || a==GX_VA_CLR1)continue;
    for(unsigned v=0;v<count;++v){
      const auto* p=raw.data()+v*source.stride+attr.offset;
      if(attr.mode!=GX_DIRECT){const unsigned index=Read(p,attr.mode==GX_INDEX8?1:2,false);const unsigned normalized=index-attr.first;
        XXH3_64bits_update(asset,&normalized,sizeof(normalized));p=attr.view.data()+std::size_t(normalized)*attr.stride;}
      if(a>=GX_VA_TEX0)XXH3_64bits_update(asset,p,attr.bytes);
    }
  }
  if(source.height){
    for(auto* digest:{shape,asset}){XXH3_64bits_update(digest,&source.height->identity,sizeof(source.height->identity));XXH3_64bits_update(digest,&source.displacementAmount,sizeof(source.displacementAmount));}
  }
  const auto assetKey=XXH3_64bits_digest(asset);
  auto [reference,pose]=cache->Template(assetKey,key);
  if(pose)return pose;
  if(reference){
    const auto poseStart=std::chrono::steady_clock::now();
    auto adapted=Pose(*reference,source,raw);
    if(adapted){
      const auto nanoseconds=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-poseStart).count();
      poseNanoseconds+=nanoseconds;++poseUpdates;
      if(poseUpdates<=4)std::fprintf(stderr,"GEKKOAOT_MESH_POSE input_vertices=%u output_vertices=%u topology_rebuild=0 cpu_us=%.3f\n",source.count,adapted->counts[0]+adapted->counts[1],nanoseconds/1000.);
      cache->PublishPose(assetKey,key,adapted);return adapted;
    }
  }
  return cache->Get(key,XXH3_64bits_digest(shape),assetKey,total,[&]{
    auto snapshot=std::make_shared<Source>(std::move(source));snapshot->raw.assign(raw.begin(),raw.end());
    for(auto& attr:snapshot->attributes){if(!attr.view.empty())attr.owned.assign(attr.view.begin(),attr.view.end());attr.view={};}
    return snapshot;
  });
}
unsigned select_level(const Prepared& mesh) noexcept{
  const float pixels=ProjectedPixels(mesh);
  if(pixels<24)return 0;
  const unsigned level=quality.load()>1&&mesh.counts[1]&&(mesh.displaced||pixels>=140)?2:1;
  ++appliedDraws;originalTriangles+=mesh.originalTriangles;refinedTriangles+=mesh.counts[level-1]/3;
  if(appliedDraws<=8)std::fprintf(stderr,"GEKKOAOT_MESH_APPLIED original_triangles=%u refined_triangles=%u level=%u projected_pixels=%.1f\n",
    mesh.originalTriangles,mesh.counts[level-1]/3,level,pixels);
  return level;
}
FormatScope::FormatScope(const Prepared& mesh):descriptors_(gx::g_gxState.vtxDesc),format_(gx::g_gxState.vtxFmts[GX_VTXFMT7]){
  canonical=true;auto& state=gx::g_gxState;state.vtxDesc=mesh.descriptors;state.vtxFmts[GX_VTXFMT7]=mesh.format;
  state.clearVtxSizeCache();state.dirty|=gx::DirtyAll;
}
FormatScope::~FormatScope(){auto& state=gx::g_gxState;state.vtxDesc=descriptors_;state.vtxFmts[GX_VTXFMT7]=format_;state.clearVtxSizeCache();state.dirty|=gx::DirtyAll;canonical=false;}
void shutdown() noexcept{
  if(cache){cache->Stop();cache.reset();}
  std::fprintf(stderr,"GEKKOAOT_MESH_RENDER draws=%llu original_triangles=%llu refined_triangles=%llu\n",
    (unsigned long long)appliedDraws,(unsigned long long)originalTriangles,(unsigned long long)refinedTriangles);
  std::fprintf(stderr,"GEKKOAOT_MESH_POSE_STATS updates=%llu cpu_ms=%.3f\n",(unsigned long long)poseUpdates,poseNanoseconds/1e6);
  appliedDraws=originalTriangles=refinedTriangles=poseUpdates=poseNanoseconds=0;
}
}
