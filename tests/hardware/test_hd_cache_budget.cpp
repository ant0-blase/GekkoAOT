#include "../../runtime/gx/hd_cache_budget.h"
#include <fstream>
#include <cstdio>
#include <cstdlib>
#define CHECK(v) do{if(!(v)){std::fprintf(stderr,"line %d: %s\n",__LINE__,#v);std::exit(1);}}while(0)
int main(){
  namespace fs=std::filesystem;
  const auto root=fs::temp_directory_path()/("gekkoaot-cache-budget-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));fs::create_directories(root);
  GekkoAOT::GX::HD::DiskBudget cache;
  const auto write=[&](unsigned i,unsigned size,unsigned limit){auto p=root/(std::to_string(i)+".dds");std::ofstream f(p,std::ios::binary);std::string bytes(size,'x');f.write(bytes.data(),size);f.close();cache.Written(root,p,size,limit);return p;};
  for(unsigned i=0;i<200;++i)write(i,128,100000);
  CHECK(cache.scans()==1 && cache.bytes()==25600);
  auto same=write(199,256,100000);CHECK(cache.bytes()==25728 && cache.Contains(same));
  auto retained=write(200,500,1000);CHECK(cache.bytes()<=900 && cache.Contains(retained));
  CHECK(cache.scans()==2);CHECK(fs::file_size(retained)==500);
  auto ignored=root/"game.iso";std::ofstream(ignored)<<"original game";
  write(201,600,1000);CHECK(fs::exists(ignored));
  std::printf("200 writes: one catalogue scan; overwrite accounting and bounded eviction passed\n");
  fs::remove_all(root);
}
