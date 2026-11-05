// SPDX-License-Identifier: GPL-3.0-or-later
#include "native/runtime.h"
#include "sdk_fixture_words.h"
#include "sdk_fixture_words.h"
#include "native/sdk_autoresolver.h"
#include "sdk/gx_normal_service.h"
#include "sdk/gx_array_service.h"
#include <array>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <memory>
#define CHECK(x) do{if(!(x)){std::cerr<<__LINE__<<": " #x "\n";std::exit(1);}}while(false)
using namespace GekkoAOT::Native;
static const std::vector<std::uint32_t>& normalBody(){static const auto w=RequireSdkFixture("admission_gx_normal");return w;}
static const std::vector<std::uint32_t>& packedBody(){static const auto w=RequireSdkFixture("admission_gx_normal_packed");return w;}
static const std::vector<std::uint32_t>& dvdBody(){static const auto w=RequireSdkFixture("admission_dvd_fast_open");return w;}
static bool Enabled(const char* name){const auto* flag=std::getenv(name);return !flag||flag[0]!='0';}
static std::string Analyze(bool changed,bool leaf=false,bool sda2=false) {
  std::vector<unsigned char> dol(0x600);
  auto word=[&](unsigned at,unsigned value){for(unsigned i=0;i<4;++i)dol[at+i]=value>>(24-i*8);};
  word(0,0x100);word(0x48,0x80010000);word(0x90,0x500);word(0xe0,0x80010000);
  word(0x100,0x48000101);word(0x104,0x480001fd);word(0x108,0x480002f9);word(0x10c,0x480003f5);word(0x110,0x4e800020);
  for(unsigned i=0;i<dvdBody().size();++i)word(0x200+i*4,dvdBody()[i]);
  for(unsigned i=0;i<normalBody().size();++i)word(0x300+i*4,normalBody()[i]);
  for(unsigned i=0;i<packedBody().size();++i)word(0x400+i*4,packedBody()[i]);
  if(leaf) {
    for(unsigned i=0;i<GxSetArrayLeafFixture().size();++i)word(0x500+i*4,GxSetArrayLeafFixture()[i]);
  } else {
    for(unsigned i=0;i<GxSetArrayFixture().size();++i)word(0x500+i*4,GxSetArrayFixture()[i]);
    if(sda2){word(0x500+15*4,0x8062a778);word(0x500+30*4,0x8062a778);}
  }
  if(changed){word(0x200+14*4,0x41820010);word(0x300+7*4,0xe0a30004);word(0x500+(leaf?1:12)*4,0x41800020);}
  const auto path=std::filesystem::temp_directory_path()/("gekkoaot-sdk-admission-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+".txt");
  struct Cleanup {std::filesystem::path p;~Cleanup(){std::error_code ec;std::filesystem::remove(p,ec);std::filesystem::remove(p.string()+".static.txt",ec);}}cleanup{path};
  auto runtime=std::make_unique<HostRuntime>();CHECK(runtime->LoadDolImage(dol).entry_point==0x80010000);
  const auto stats=AutoRegisterNativeSdk(*runtime,path);
  std::ifstream f(path);std::string manifest((std::istreambuf_iterator<char>(f)),{});CHECK(!manifest.empty());
  CHECK(RegisterNativeSdkManifest(*runtime,path));(void)stats;
  return manifest;
}
int main() {
  const auto manifest=Analyze(false);
  CHECK((manifest.find(" DVDFastOpen")!=std::string::npos)==Enabled("GEKKOAOT_NATIVE_DVD_FST"));
  CHECK((manifest.find(" GXLoadNrmMtxImm ")!=std::string::npos)==Enabled("GEKKOAOT_NATIVE_GX_NORMAL_MATRIX"));
  CHECK((manifest.find(" GXLoadNrmMtxImm3x3")!=std::string::npos)==Enabled("GEKKOAOT_NATIVE_GX_NORMAL_MATRIX"));
  CHECK((manifest.find(" GXSetArray")!=std::string::npos)==Enabled("GEKKOAOT_NATIVE_GX_ARRAY_SERVICE"));
  CHECK((Analyze(false,true).find(" GXSetArray")!=std::string::npos)==Enabled("GEKKOAOT_NATIVE_GX_ARRAY_SERVICE"));
  CHECK((Analyze(false,false,true).find(" GXSetArray")!=std::string::npos)==Enabled("GEKKOAOT_NATIVE_GX_ARRAY_SERVICE"));
  CHECK(Analyze(true,true).find(" GXSetArray")==std::string::npos);
  const auto changed=Analyze(true);
  CHECK(changed.find(" DVDFastOpen")==std::string::npos);CHECK(changed.find(" GXLoadNrmMtxImm ")==std::string::npos);
  CHECK(changed.find(" GXSetArray")==std::string::npos);
  CHECK((changed.find(" GXLoadNrmMtxImm3x3")!=std::string::npos)==Enabled("GEKKOAOT_NATIVE_GX_NORMAL_MATRIX"));
  std::cout<<"Automatic DOL SDK hooks: leaf semantics, relocated SDA, branch/load mutations, manifest and independent flags passed\n";
}
