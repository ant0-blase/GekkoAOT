// SPDX-License-Identifier: GPL-3.0-or-later
#include "gekkoaot_hd_resources.hpp"
#include "gekkoaot_mesh_resources.hpp"
#include "hd_texture.h"
#include "hd_cache_budget.h"
#include "compact_sr.h"
#include "compact_sr_gpu.h"
#include <SDL3/SDL_filesystem.h>
#include "gekkoaot_material_reconstruction.h"
#include "gekkoaot_enhanced_renderer.hpp"
#include "texture_convert.hpp"
#include "dds_io.hpp"
#include "../gx/texture.hpp"
#include "../gx/gx.hpp"
#include "recording.hpp"
#include "../internal.hpp"
#include <atomic>
#include <chrono>
#include <ctime>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <unordered_map>
#include <xxhash.h>

namespace aurora::gfx::hd {
namespace {
using Clock=std::chrono::steady_clock;
double WorkerCpuMs(){
#if defined(CLOCK_THREAD_CPUTIME_ID)
  timespec t{};if(clock_gettime(CLOCK_THREAD_CPUTIME_ID,&t)==0)return t.tv_sec*1000.+t.tv_nsec/1e6;
#endif
  return -1.;
}
std::atomic<unsigned> scale{1},quality{0},backendMode{1},budget{512},diskBudget{4096};
std::atomic<unsigned> materialMode{0},pomQuality{0},comparisonMode{0};
std::atomic<float> normalStrength{.65f},relief{.015f};
MaterialBinding boundMaterial;
std::uint64_t materialBound=0,materialBytes=0,colorBound=0,colorBytes=0;
std::atomic<std::uint64_t> pendingBytes{0},requests{0},hits{0},generated{0},declined{0},generationNs{0},detailPixels{0};
std::mutex neuralWorker;
std::shared_ptr<const GekkoAOT::GX::CompactSR::Model> compactModel;
std::once_flag compactModelOnce;
std::shared_ptr<const GekkoAOT::GX::CompactSR::Model> CompactModel(){
  std::call_once(compactModelOnce,[]{
    std::vector<std::filesystem::path> paths;
    if(auto* dir=std::getenv("GEKKOAOT_SR_MODEL_DIR"))paths.emplace_back(std::filesystem::path(dir)/"general-x4v3.gsr");
    if(auto* base=SDL_GetBasePath()){
      paths.emplace_back(std::filesystem::path(base)/"models/srvgg/general-x4v3.gsr");
      paths.emplace_back(std::filesystem::path(base)/"../share/gekkoaot/models/srvgg/general-x4v3.gsr");
    }
#ifdef GEKKOAOT_COMPACT_SR_MODEL_PATH
    paths.emplace_back(GEKKOAOT_COMPACT_SR_MODEL_PATH);
#endif
    for(const auto& path:paths)if((compactModel=GekkoAOT::GX::CompactSR::Load(path.string()))){
      std::fprintf(stderr,"GEKKOAOT_COMPACT_SR_MODEL loaded=1 bytes=4853660 dependencies=none path=%s\n",path.string().c_str());return;}
    std::fprintf(stderr,"GEKKOAOT_COMPACT_SR_MODEL loaded=0 reason=missing-or-invalid-model\n");
  });return compactModel;
}
std::atomic<std::uint64_t> neuralGenerated{0},neuralPixels{0};
std::atomic<float> surfaceDetail{0};
struct Observation {std::uint64_t hash=0,frame=0;unsigned stable=0;bool wait=false,admitted=false,background=false;};
std::unordered_map<std::uint64_t,Observation> observations;
std::uint64_t ObservationKey(const GXTexObj_& obj){
  const std::array<std::uint64_t,3> key{std::uint64_t(reinterpret_cast<std::uintptr_t>(obj.data)),(std::uint64_t(obj.width())<<32)|obj.height(),obj.format()};
  return XXH3_64bits(key.data(),sizeof(key));
}
bool DiskCached(const std::filesystem::path& cachePath);
bool Admit(const GXTexObj_& obj,const texture::TextureSourceKey& source,const std::filesystem::path& cachePath){
  const auto frame=gekkoaot_recording_frame_id();const auto key=ObservationKey(obj);
  if(observations.size()>=4096&&!observations.contains(key)){auto oldest=std::min_element(observations.begin(),observations.end(),[](const auto& a,const auto& b){return a.second.frame<b.second.frame;});observations.erase(oldest);}
  auto [it,added]=observations.try_emplace(key);auto& o=it->second;
  const auto hash=source.textureHash^std::rotl(source.tlutHash,17);
  if(added){o.hash=hash;o.frame=frame;o.stable=1;o.wait=obj.no_cache();}
  else if(o.hash!=hash){o.hash=hash;o.stable=0;o.wait=true;o.admitted=false;}
  else if(frame!=o.frame)++o.stable;
  o.frame=frame;o.background=enhanced::background_draw();
  if(o.wait && o.stable<30 && !DiskCached(cachePath))return false;
  o.admitted=true;return true;
}
constexpr std::uint64_t PendingLimit=32ull<<20;
std::uint64_t ResourceBudget(){return std::min<std::uint64_t>(256ull<<20,(std::uint64_t(budget.load())<<20)/2);}

struct Job {
  std::mutex mutex;
  std::string name;
  std::filesystem::path path;
  std::vector<std::uint8_t> source,palette;
  unsigned width=0,height=0,mips=0,format=0,paletteFormat=0,paletteEntries=0;
  unsigned scale=1,quality=0,backend=0,wrapS=0,wrapT=0;
  std::atomic<std::shared_ptr<const GekkoAOT::GX::Mesh::HeightField>> heightField;
  bool neural=false,graphic=false,coverage=false;
  bool material=false,background=false,world=false;
  std::atomic<unsigned> materialKind{0};
  std::atomic<float> confidence{0};
  bool bound=false;
  std::atomic<int> cacheHint{-1};
  std::uint64_t pending=0;
};
// Generation identities only. GPU handles, worker queues, replacement
// selection and eviction remain owned by Aurora's existing replacement service.
std::unordered_map<std::string,std::unique_ptr<Job>> jobs;
// Per-draw lookups of a known resource skip rebuilding its name and cache
// path (fmt + std::filesystem + a locked disk-catalogue probe per sampled
// texture per draw): the derived key fixes both, so map it to its job.
struct JobMemo {Job* job=nullptr;texture::TextureSourceKey key;};
std::unordered_map<std::uint64_t,JobMemo> jobMemo;
void Put32(std::vector<std::uint8_t>& b,unsigned at,std::uint32_t v){
  for(unsigned i=0;i<4;++i)b[at+i]=std::uint8_t(v>>(i*8));
}
std::uint32_t Get32(const std::vector<std::uint8_t>& b,unsigned at){
  std::uint32_t v=0;for(unsigned i=0;i<4;++i)v|=std::uint32_t(b[at+i])<<(i*8);return v;
}
void PublishHeight(Job& job,std::span<const std::uint8_t> pixels){
  if(!job.material || job.materialKind.load()!=2 || job.confidence.load()<=.8f || pixels.size()<std::size_t(job.width)*job.height*4)return;
  auto field=std::make_shared<GekkoAOT::GX::Mesh::HeightField>();
  field->width=std::min(job.width,128u);field->height=std::min(job.height,128u);field->wrapS=job.wrapS;field->wrapT=job.wrapT;
  field->identity=XXH3_64bits(job.name.data(),job.name.size());field->pixels.resize(field->width*field->height);
  for(unsigned y=0;y<field->height;++y)for(unsigned x=0;x<field->width;++x)
    field->pixels[y*field->width+x]=pixels[(std::size_t(y*job.height/field->height)*job.width+x*job.width/field->width)*4+3];
  job.heightField.store(std::move(field));
}
bool Read(Job& job,std::vector<std::uint8_t>& bytes){
  std::ifstream file(job.path,std::ios::binary|std::ios::ate);
  if(!file)return false;
  const auto n=file.tellg();
  const std::uint64_t limit=std::uint64_t(budget.load())<<20;
  if(n<128 || std::uint64_t(n)>limit)return false;
  bytes.resize(std::size_t(n));file.seekg(0);file.read(reinterpret_cast<char*>(bytes.data()),n);
  if(!file || !dds::parse_dds_bytes({bytes.data(),bytes.size()})){bytes.clear();return false;}
  const unsigned extra=job.scale==4?2:job.scale==2?1:0;
  if(Get32(bytes,16)!=job.width*job.scale || Get32(bytes,12)!=job.height*job.scale ||
     Get32(bytes,28)!=job.mips+extra){bytes.clear();return false;}
  if(!job.material && !job.background && Get32(bytes,48)!=0x53520005u){
    const auto base=128+GekkoAOT::GX::HD::MipBytes(job.width*job.scale,job.height*job.scale,extra),count=std::uint64_t(job.width)*job.height*4;
    if(base+count<=bytes.size() && GekkoAOT::GX::HD::CoverageEncoding({bytes.data()+base,std::size_t(count)},job.width,job.height).kind){bytes.clear();return false;}
  }
  if(!job.material && job.backend && (Get32(bytes,48)&0xffff0000u)!=0x53520000u){
    // Upgrade earlier spatial/graphic fallbacks selectively. Existing neural
    // images remain valid; a coverage fix must not discard the entire cache.
    const auto base=128+GekkoAOT::GX::HD::MipBytes(job.width*job.scale,job.height*job.scale,extra);
    const auto count=std::size_t(job.width)*job.height*4;
    if(base+count<=bytes.size()){
      const std::span<const std::uint8_t> original{bytes.data()+base,count};
      bool legacyPalette=true;std::unordered_set<std::uint32_t> colors;unsigned vectorLike=0;
      for(std::size_t i=0;i<count;i+=4){const auto* p=original.data()+i;
        if(p[3]&&colors.size()<=128)colors.insert(unsigned(p[0])|(unsigned(p[1])<<8)|(unsigned(p[2])<<16));
        const float x=p[0]/127.5f-1,y=p[1]/127.5f-1,z=p[2]/127.5f-1,n=x*x+y*y+z*z;
        vectorLike+=p[2]>180&&p[2]>p[0]+20&&p[2]>p[1]+20&&n>.8f&&n<1.2f;
      }
      legacyPalette=colors.size()<=128;
      const bool legacyExcluded=job.width<16||job.height<16||job.width>512||job.height>512 || vectorLike>=std::uint64_t(job.width)*job.height*8/10;
      if((legacyExcluded||legacyPalette) && !GekkoAOT::GX::HD::LimitedPalette(original,job.width,job.height)){
        static unsigned upgrades=0;if(upgrades++<16)std::fprintf(stderr,"GEKKOAOT_HD_CACHE_UPGRADE source=%ux%u reason=expanded-neural-coverage\n",job.width,job.height);
        bytes.clear();return false;
      }
    }
  }
  if(job.material){
    if(Get32(bytes,32)!=0x4d415431u || Get32(bytes,36)>2u || Get32(bytes,40)>1000u)return false;
    job.materialKind=Get32(bytes,36);job.confidence=Get32(bytes,40)/1000.f;
  }
  PublishHeight(job,{bytes.data()+128,bytes.size()-128});
  job.cacheHint=1;++hits;return true;
}
GekkoAOT::GX::HD::DiskBudget diskAccounting;
bool DiskCached(const std::filesystem::path& cachePath){return diskAccounting.Contains(cachePath);}
bool Load(void* data,const char* path,std::vector<std::uint8_t>& bytes){
  auto& job=*static_cast<Job*>(data);
  if(!path || job.name!=path)return false; // mip levels are in the DDS itself
  std::lock_guard lock(job.mutex);
  struct ReleasePayload {
    Job& job;
    ~ReleasePayload(){pendingBytes-=job.pending;job.pending=0;
      std::vector<std::uint8_t>().swap(job.source);std::vector<std::uint8_t>().swap(job.palette);}
  } release{job};
  try {
    if(Read(job,bytes)){
      pendingBytes-=job.pending;job.pending=0;
      std::vector<std::uint8_t>().swap(job.source);std::vector<std::uint8_t>().swap(job.palette);
      return true;
    }
    if(job.source.empty())return false;
    const auto start=Clock::now();const auto cpuStart=WorkerCpuMs();
    auto decoded=job.palette.empty()
      ?convert_texture(job.format,job.width,job.height,job.mips,{job.source.data(),job.source.size()})
      :convert_texture_palette(job.format,job.width,job.height,job.mips,{job.source.data(),job.source.size()},
        GXTlutFmt(job.paletteFormat),std::uint16_t(job.paletteEntries),{job.palette.data(),job.palette.size()});
    if(decoded.format==wgpu::TextureFormat::R8Unorm){
      ByteBuffer rgba{decoded.data.size()*4};
      for(std::size_t i=0;i<decoded.data.size();++i)std::fill_n(rgba.data()+i*4,4,decoded.data.data()[i]);
      decoded.data=std::move(rgba);decoded.format=wgpu::TextureFormat::RGBA8Unorm;
    }
    if(decoded.format!=wgpu::TextureFormat::RGBA8Unorm)return false;
    // GX TEV samples UNORM data. Keep that transfer function by default;
    // explicit material metadata can opt into SRGB or vector reconstruction.
    std::uint64_t reconstructed=0;
    std::optional<GekkoAOT::GX::HD::Image> image;
    if(job.material){
      auto surface=GekkoAOT::GX::Material::Reconstruct({decoded.data.data(),decoded.data.size()},
        job.width,job.height,job.mips,job.wrapS,job.wrapT,job.world);
      job.materialKind=unsigned(surface.kind);job.confidence=surface.confidence;
      if(surface.kind==GekkoAOT::GX::Material::Kind::Unknown)return false;
      image=std::move(surface.map);
    }else {
      if(!job.background)image=GekkoAOT::GX::HD::EnhanceCoverage({decoded.data.data(),decoded.data.size()},job.width,job.height,job.mips,job.scale,job.wrapS,job.wrapT,ResourceBudget());
      job.coverage=image.has_value();
      // Small palettes are how the console stored most textures (C4 walls, wood,
      // skin), not only interface art: Real-ESRGAN takes them too. Its largest
      // use stays bounded, since a full-screen image costs seconds of GPU time.
      // A network with a 34-pixel receptive field has nothing to work with on a
      // few rows: EA movies draw each frame as strips 4 texels high, which it
      // turned into horizontal lines. Those keep the rule-based path.
      const bool esrgan=job.backend==2 && std::max(job.width,job.height)<=512u && std::min(job.width,job.height)>=16u;
      job.graphic=!job.background && !esrgan && (job.backend!=2 || job.coverage || std::min(job.width,job.height)<16u) &&
        GekkoAOT::GX::HD::LimitedPalette({decoded.data.data(),decoded.data.size()},job.width,job.height);
      job.graphic|=job.coverage;
      if(job.graphic && !image)image=GekkoAOT::GX::HD::EnhanceGraphic({decoded.data.data(),decoded.data.size()},job.width,job.height,job.mips,
        job.scale,job.wrapS,job.wrapT,ResourceBudget());
      if(!image && esrgan){
        std::lock_guard neuralLock(neuralWorker);
        if(auto model=CompactModel())image=GekkoAOT::GX::CompactSR::Enhance(*model,{decoded.data.data(),decoded.data.size()},job.width,job.height,job.mips,
          job.scale,job.wrapS,job.wrapT,ResourceBudget(),&reconstructed,true,compact_gpu_infer_tile);
        job.neural=image.has_value();if(job.neural){++neuralGenerated;neuralPixels+=reconstructed;}
      }
      if(!image && job.backend==1){
        std::lock_guard neuralLock(neuralWorker);
        image=GekkoAOT::GX::TinySR::Enhance({decoded.data.data(),decoded.data.size()},job.width,job.height,job.mips,
          job.scale,job.wrapS,job.wrapT,ResourceBudget(),&reconstructed,job.quality!=0,true);
        job.neural=image.has_value();if(job.neural){++neuralGenerated;neuralPixels+=reconstructed;}
      }
      if(!image)image=GekkoAOT::GX::HD::Enhance({decoded.data.data(),decoded.data.size()},job.width,job.height,job.mips,
        job.scale,job.quality!=0,GekkoAOT::GX::HD::Encoding::Linear,job.wrapS,job.wrapT,
        ResourceBudget(),&reconstructed);
    }
    if(!image)return false;
    PublishHeight(job,image->pixels);
    if(!job.material && !job.graphic && job.quality)GekkoAOT::GX::HD::ReconstructMipAppearance(*image,{decoded.data.data(),decoded.data.size()},job.width,job.height,job.mips,job.scale);
    if(job.background)GekkoAOT::GX::HD::PreserveBackgroundEdges(*image,{decoded.data.data(),decoded.data.size()},job.width,job.height,job.wrapS,job.wrapT);
    auto encoded=dds::encode_rgba8_dds(image->width,image->height,{image->pixels.data(),image->pixels.size()});
    bytes.assign(encoded.data(),encoded.data()+encoded.size());
    if(!job.material && !job.graphic && job.quality)Put32(bytes,44,0x48444d31u);
    if(!job.material)Put32(bytes,48,0x53520000u|(job.coverage?5u:job.neural?2u:job.graphic?1u:0u));
    Put32(bytes,8,0x2100fu);Put32(bytes,28,image->mips);Put32(bytes,108,0x401008u);
    if(job.material){Put32(bytes,32,0x4d415431u);Put32(bytes,36,job.materialKind.load());Put32(bytes,40,unsigned(job.confidence.load()*1000));}
    std::error_code ec;std::filesystem::create_directories(job.path.parent_path(),ec);
    if(!ec){
      auto temporary=job.path;temporary+="."+std::to_string(Clock::now().time_since_epoch().count())+".tmp";
      std::ofstream file(temporary,std::ios::binary|std::ios::trunc);
      file.write(reinterpret_cast<const char*>(bytes.data()),std::streamsize(bytes.size()));file.close();
      if(file)std::filesystem::rename(temporary,job.path,ec);
      if(ec)std::filesystem::remove(temporary,ec);
      if(!ec)job.cacheHint=1;
      if(!ec)diskAccounting.Written(job.path.parent_path().parent_path(),job.path,bytes.size(),std::uint64_t(diskBudget.load())<<20);
    }
    ++generated;detailPixels+=reconstructed;
    generationNs+=std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now()-start).count();
    std::fprintf(stderr,"GEKKOAOT_HD_RECONSTRUCT source=%ux%u result=%ux%u method=%s role=%s detail_pixels=%llu bytes=%zu material_kind=%u confidence=%.3f worker_cpu_ms=%.3f generation_wall_ms=%.3f\n",
      job.width,job.height,image->width,image->height,job.material?"material-structure-v2":job.coverage?"coverage-contour-v3":job.graphic?"edge-rule-2d-v1":job.neural?(job.backend==2?"srvgg-native-v4":"espcn-native-v5"):"internal-examples-v2",
      job.background?"sky":"texture",(unsigned long long)reconstructed,bytes.size(),job.materialKind.load(),job.confidence.load(),cpuStart<0?-1.:WorkerCpuMs()-cpuStart,std::chrono::duration<double,std::milli>(Clock::now()-start).count());
    pendingBytes-=job.pending;job.pending=0;
    std::vector<std::uint8_t>().swap(job.source);std::vector<std::uint8_t>().swap(job.palette);
    return true;
  }catch(...){return false;}
}
}
std::uint64_t pending_bytes() noexcept{return pendingBytes.load();}
unsigned reconstruction_scale() noexcept{return scale.load()>1?scale.load():0;}
bool needs_observation(const GXTexObj_& obj) noexcept{
  if(scale.load()<=1)return false;auto it=observations.find(ObservationKey(obj));
  return it!=observations.end() && (it->second.background!=enhanced::background_draw() || (it->second.wait && !it->second.admitted && it->second.frame!=gekkoaot_recording_frame_id()));
}
bool enabled() noexcept{return scale.load()>1 || materialMode.load()!=0;}
bool material_enabled() noexcept{return materialMode.load()!=0;}
std::uint64_t replacement_budget() noexcept{return enabled()?std::uint64_t(budget.load())<<20:4ull<<30;}
bool generated_needs_compute(void* data) noexcept {
  if(!data)return false;auto& job=*static_cast<Job*>(data);
  if(job.material || !job.backend || job.format<=3)return false;
  auto hint=job.cacheHint.load();
  if(hint<0){std::error_code ec;hint=std::filesystem::is_regular_file(job.path,ec)?1:0;job.cacheHint=hint;}
  return hint==0;
}
void configure(const GekkoAOT::GX::GraphicsSettings& s){
  geometry::configure(s.enhanced?s.geometry_refinement:0,s.material_compare!=0,s.enhanced && s.displacement,s.material_relief);
  if(s.texture_scale>1 || s.material_enhancement || s.displacement)diskAccounting.Start(std::filesystem::path(g_config.cachePath)/"hd-textures");
  const unsigned next=s.texture_scale;
  const unsigned nextMaterial=s.enhanced?(s.material_enhancement || s.displacement):0;
  const bool changed=scale.exchange(next)!=next || quality.load()!=s.texture_quality || backendMode.load()!=s.texture_backend || materialMode.load()!=nextMaterial;
  if(comparisonMode.load()!=s.material_compare || normalStrength.load()!=s.material_normal_strength || relief.load()!=s.material_relief || pomQuality.load()!=s.pom_quality || surfaceDetail.load()!=s.surface_detail)
    gx::g_gxState.dirty|=gx::DirtyUniform|gx::DirtyTextures;
  surfaceDetail=s.surface_detail;comparisonMode=s.material_compare;materialMode=nextMaterial;normalStrength=s.material_normal_strength;relief=s.material_relief;pomQuality=s.pom_quality;
  backendMode=s.texture_backend;quality=s.texture_quality;budget=s.hd_budget_mib;diskBudget=s.hd_cache_mib;
  if(changed){gx::g_gxState.dirty|=gx::DirtyPipeline|gx::DirtyTextures|gx::DirtyUniform;gx::clear_static_texture_cache();gx::texture::invalidate_bindings();}
}
static std::optional<texture_replacement::ReplacementResult> Request(
    const GXTexObj_& obj,const texture::TextureSourceKey& key,const GXTlutObj_* tlut,bool material,Job** state=nullptr){
  if((material?!material_enabled():scale.load()<=1) || (material && (!enhanced::eligible() || obj.no_cache() || (tlut&&tlut->no_cache()))))return {};
  switch(obj.format()){
  case GX_TF_I4:case GX_TF_I8:case GX_TF_IA4:case GX_TF_IA8:case GX_TF_RGB565:case GX_TF_RGB5A3:
  case GX_TF_RGBA8:case GX_TF_CMPR:case GX_TF_C4:case GX_TF_C8:case GX_TF_C14X2:break;
  default:return {};
  }
  if(obj.width()>1024 || obj.height()>1024 || obj.width()<(material?16u:1u) || obj.height()<(material?16u:1u))return {};
  const bool background=!material && enhanced::background_draw();
  // Scene surfaces, as opposed to interface art. Only materials are resolved at
  // draw time: color textures can load while another projection is still set.
  const bool world=material && gx::g_gxState.projType==GX_PERSPECTIVE;
  const auto factor=material?1:scale.load(),q=material?0:quality.load();
  const unsigned backend=material?0:backendMode.load();
  // A private derived source key prevents generated resources from overriding
  // user packs, and distinguishes different wrap modes of the same GX image.
  auto hdKey=key;
  std::array<std::uint64_t,8> fields{key.textureHash,key.tlutHash,key.width,key.height,key.format,
      (std::uint64_t(factor)<<32)|q,(std::uint64_t(obj.mip_count())<<32)|unsigned(obj.wrap_s()),
      (std::uint64_t(unsigned(obj.wrap_t()))<<32)|(tlut?unsigned(tlut->format):0)};
  std::array<std::uint8_t,64> identity{};
  for(unsigned i=0;i<fields.size();++i)for(unsigned j=0;j<8;++j)identity[i*8+j]=std::uint8_t(fields[i]>>(j*8));
  const auto model=backend==2?0x90b6ff0db2a28677ull:backend==1?GekkoAOT::GX::TinySR::Fingerprint(factor):0ull;
  hdKey.textureHash=XXH3_64bits_withSeed(identity.data(),identity.size(),model^(material?0x47454b4b4f4d4132ull:0x47454b4b4f484434ull)^(background?0x534b592d5632ull:0ull)^(world?0x574f524c44ull:0ull));
  if(const auto memo=jobMemo.find(hdKey.textureHash);memo!=jobMemo.end() && memo->second.key==hdKey){
    if(!material && !Admit(obj,key,memo->second.job->path))return {};
    if(state)*state=memo->second.job;
    return texture_replacement::find_source_replacement(obj,hdKey);
  }
  auto filename=texture_replacement::build_texture_replacement_name(hdKey);
  if(filename.empty())return {};
  const std::string name=std::string(material?"@gekkoaot-material/":backend==2?"@gekkoaot-srvgg/":backend==1?"@gekkoaot-neural/":"@gekkoaot-hd-v4/")+std::to_string(factor)+(q?"h/":"f/")+filename;
  const auto variant=std::string(background?"sky-v2-":"")+std::string(world?"world-":"")+std::string(material?"material-v3-":backend==2?(std::min(obj.width(),obj.height())<16u?"srvgg-thin-v1-":"srvgg-native-v5-"):backend==1?"espcn-native-v5-":"v4-")+std::to_string(factor)+(q?"h-":"f-")+std::to_string(obj.mip_count())+"-"+
      std::to_string(obj.wrap_s())+std::to_string(obj.wrap_t())+"-"+std::to_string(tlut?unsigned(tlut->format):0);
  const auto cachePath=std::filesystem::path(g_config.cachePath)/"hd-textures"/variant/filename;
  if(!material && !Admit(obj,key,cachePath))return {};
  if(!jobs.contains(name)){
    const auto n=gx::texture::texture_source_size(obj.format(),obj.width(),obj.height(),obj.mip_count());
    const auto pn=tlut?gx::texture::tlut_source_size(tlut->numEntries):0;
    if(jobs.size()>=4096 || n+pn>PendingLimit || pendingBytes.load()+n+pn>PendingLimit){++declined;return {};}
    auto job=std::make_unique<Job>();job->name=name;
    // The persistent identity adds mip count, wrap behavior, palette format and
    // algorithm version to Aurora's content/palette/dimension/format key.
    job->path=cachePath;
    job->width=obj.width();job->height=obj.height();job->mips=obj.mip_count();job->format=obj.format();
    job->scale=factor;job->quality=q;job->backend=backend;job->wrapS=obj.wrap_s();job->wrapT=obj.wrap_t();
    job->material=material;job->background=background;job->world=world;
    job->source.assign(static_cast<const std::uint8_t*>(obj.data),static_cast<const std::uint8_t*>(obj.data)+n);
    if(tlut){job->palette.assign(static_cast<const std::uint8_t*>(tlut->data),static_cast<const std::uint8_t*>(tlut->data)+pn);
      job->paletteFormat=tlut->format;job->paletteEntries=tlut->numEntries;}
    job->pending=n+pn;pendingBytes+=job->pending;
    auto* source=job.get();jobs.emplace(name,std::move(job));++requests;
    if(!material && requests.load()<=16){const auto& observed=observations.at(ObservationKey(obj));
      std::fprintf(stderr,"GEKKOAOT_HD_ADMISSION source=%ux%u stable_observations=%u disk_cached=%u deferred_dynamic=%u\n",obj.width(),obj.height(),observed.stable,unsigned(diskAccounting.Contains(cachePath)),unsigned(observed.wait));}
    texture::register_generated_replacement(name,{Load,source},{.priority=-1000});
  }
  auto* job=jobs.at(name).get();
  jobMemo[hdKey.textureHash]={job,hdKey};
  if(state)*state=job;
  return texture_replacement::find_source_replacement(obj,hdKey);
}
std::optional<texture_replacement::ReplacementResult> request(
    const GXTexObj_& obj,const texture::TextureSourceKey& key,const GXTlutObj_* tlut){
  Job* job=nullptr;auto result=Request(obj,key,tlut,false,&job);
  if(result && result->handle && job && !job->bound){
    job->bound=true;++colorBound;
    const auto& tex=*result->handle;
    colorBytes+=GekkoAOT::GX::HD::MipBytes(tex.size.width,tex.size.height,tex.mipCount);
    if(colorBound<=16 || job->background)std::fprintf(stderr,"GEKKOAOT_HD_TEXTURE_BOUND original=%ux%u gpu=%ux%u mips=%u backend=%u role=%s\n",
      job->width,job->height,tex.size.width,tex.size.height,tex.mipCount,job->backend,job->background?"sky":"texture");
  }
  return result;
}
MaterialBinding request_material(const GXTexObj_& obj,const texture::TextureSourceKey& key,const GXTlutObj_* tlut){
  Job* job=nullptr;auto result=Request(obj,key,tlut,true,&job);
  if(!result || !result->handle || !job)return {};
  if(!job->bound){
    job->bound=true;++materialBound;materialBytes+=GekkoAOT::GX::HD::MipBytes(job->width,job->height,job->mips);
    if(materialBound<=8)std::fprintf(stderr,"GEKKOAOT_MATERIAL_BOUND width=%u height=%u kind=%u confidence=%.3f texture_scale=%u\n",
      job->width,job->height,job->materialKind.load(),job->confidence.load(),scale.load());
  }
  const auto confidence=job->confidence.load();
  const float strength=normalStrength.load()*(.35f+.65f*confidence);
  const float detail=surfaceDetail.load();
  return {result->handle,{comparisonMode.load()?-strength:strength,
    job->materialKind.load()==2 && confidence>.8f?relief.load()*confidence:0.f,float(pomQuality.load()),float(1+unsigned(obj.wrap_s())+4*unsigned(obj.wrap_t()))},
    {detail,detail*.5f,0.f,0.f}};
}
std::shared_ptr<const GekkoAOT::GX::Mesh::HeightField> request_height(const GXTexObj_& obj,const texture::TextureSourceKey& key,const GXTlutObj_* tlut){
  Job* job=nullptr;Request(obj,key,tlut,true,&job);return job?job->heightField.load():nullptr;
}
void bind_material(MaterialBinding binding){boundMaterial=std::move(binding);}
const MaterialBinding& bound_material() noexcept{return boundMaterial;}
void reset() noexcept{compact_gpu_shutdown();boundMaterial={};observations.clear();jobMemo.clear();jobs.clear();pendingBytes=0;}
void report() noexcept{
  std::fprintf(stderr,"GEKKOAOT_HD_DISK_CACHE catalogue_scans=%llu tracked_bytes=%llu maintenance=incremental\n",(unsigned long long)diskAccounting.scans(),(unsigned long long)diskAccounting.bytes());
  std::fprintf(stderr,"GEKKOAOT_HD_GPU published=%llu published_payload_bytes=%llu\n",(unsigned long long)colorBound,(unsigned long long)colorBytes);
  std::fprintf(stderr,"GEKKOAOT_NATIVE_CNN generated=%llu reconstructed_pixels=%llu backend=native-cpp model_bytes_espcn=184144 model_bytes_srvgg=4853660 runtime_dependencies=none\n",(unsigned long long)neuralGenerated.load(),(unsigned long long)neuralPixels.load());
  std::fprintf(stderr,"GEKKOAOT_MATERIAL_RESOURCES published=%llu payload_bytes=%llu generated_identity_count=%zu\n",
    (unsigned long long)materialBound,(unsigned long long)materialBytes,jobs.size());
  std::fprintf(stderr,"GEKKOAOT_HD_TEXTURES requested=%llu cache_hits=%llu generated=%llu declined=%llu generation_ms=%.3f pending_bytes=%llu gpu_budget_mib=%u disk_budget_mib=%u detail_pixels=%llu method=per-resource\n",
    (unsigned long long)requests.load(),(unsigned long long)hits.load(),(unsigned long long)generated.load(),
    (unsigned long long)declined.load(),generationNs.load()/1e6,(unsigned long long)pendingBytes.load(),budget.load(),diskBudget.load(),(unsigned long long)detailPixels.load());
}
}
