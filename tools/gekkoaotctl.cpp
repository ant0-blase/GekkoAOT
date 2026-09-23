// SPDX-License-Identifier: GPL-3.0-or-later
// GekkoAOT native controller: nod -> DolRecomp C/LLVM AOT -> GekkoAOT module -> Aurora GX.
#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <limits>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX 1
#endif
#include <windows.h>
#else
#include <unistd.h>
#include <sys/wait.h>
#endif

namespace fs=std::filesystem;

namespace {
std::string Env(const char* name, std::string fallback={}) {
  if (const char* v = std::getenv(name); v && *v)
    return v;
  return fallback;
}
unsigned Jobs() {
  const auto text=Env("GEKKOAOT_JOBS");
  if(!text.empty()) try { return (std::max)(1u,static_cast<unsigned>(std::stoul(text))); } catch(...) {}
  return (std::max)(1u,std::thread::hardware_concurrency());
}
unsigned EnvUnsigned(const char* name,unsigned fallback,unsigned minimum,unsigned maximum) {
  const auto text=Env(name);
  if(text.empty()) return fallback;
  try {
    const auto value=std::stoul(text);
    if(value<minimum || value>maximum) throw std::out_of_range("range");
    return static_cast<unsigned>(value);
  } catch(...) {
    std::cerr<<"[gekkoaot] warning: "<<name<<" must be "<<minimum<<".."<<maximum
             <<"; using "<<fallback<<"\n";
    return fallback;
  }
}
std::string Quote(const fs::path& p) {
#ifdef _WIN32
  std::string s=p.string(), out="\""; for(char c:s){if(c=='\"')out+="\\\"";else out+=c;} return out+"\"";
#else
  std::string s=p.string(), out="'"; for(char c:s){if(c=='\'')out+="'\\''";else out+=c;} return out+"'";
#endif
}
std::string QuoteText(std::string_view s) {
#ifdef _WIN32
  std::string out="\""; for(char c:s){if(c=='\"')out+="\\\"";else out+=c;} return out+"\"";
#else
  std::string out="'"; for(char c:s){if(c=='\'')out+="'\\''";else out+=c;} return out+"'";
#endif
}
int Run(const std::string& command) {
  std::cout << "$ " << command << '\n' << std::flush;
  const int rc=std::system(command.c_str());
#ifdef _WIN32
  return rc;
#else
  if(rc==-1) return 127;
  if(WIFEXITED(rc)) return WEXITSTATUS(rc);
  if(WIFSIGNALED(rc)) return 128+WTERMSIG(rc);
  return rc;
#endif
}
void Require(int rc, std::string_view what) { if(rc!=0) throw std::runtime_error(std::string(what)+" failed (exit "+std::to_string(rc)+")"); }
std::string ReadLine(const fs::path& p) { std::ifstream in(p); std::string s; std::getline(in,s); return s; }
void WriteText(const fs::path& p,std::string_view s) { fs::create_directories(p.parent_path()); std::ofstream o(p,std::ios::trunc); if(!o)throw std::runtime_error("cannot write "+p.string()); o<<s; }
std::string FileHash64(const fs::path& p) {
  std::ifstream in(p,std::ios::binary);
  if(!in) throw std::runtime_error("cannot hash "+p.string());
  std::uint64_t h=1469598103934665603ull;
  std::array<char,16384> buf{};
  while(in) {
    in.read(buf.data(),static_cast<std::streamsize>(buf.size()));
    const auto n=in.gcount();
    for(std::streamsize i=0;i<n;++i){h^=static_cast<unsigned char>(buf[static_cast<std::size_t>(i)]);h*=1099511628211ull;}
  }
  std::ostringstream out; out<<std::hex<<h; return out.str();
}
std::string TextHash64(std::string_view text) {
  std::uint64_t h=1469598103934665603ull;
  for(const unsigned char c:text){h^=c;h*=1099511628211ull;}
  std::ostringstream out; out<<std::hex<<h; return out.str();
}

fs::path ExecutablePath() {
#ifdef _WIN32
  std::wstring w(32768,L'\0'); DWORD n=GetModuleFileNameW(nullptr,w.data(),static_cast<DWORD>(w.size())); w.resize(n); return fs::path(w);
#else
  std::array<char,4096> b{}; const auto n=readlink("/proc/self/exe",b.data(),b.size()-1); if(n>0)return fs::path(std::string(b.data(),static_cast<std::size_t>(n)));
  return fs::absolute("gekkoaotctl");
#endif
}
fs::path Root() {
  if (const auto explicit_root = Env("GEKKOAOT_RESOURCE_DIR"); !explicit_root.empty()) {
    const fs::path root = fs::absolute(explicit_root);
    if (fs::exists(root/"runtime") && fs::exists(root/"patches")) return root;
    throw std::runtime_error("GEKKOAOT_RESOURCE_DIR does not contain runtime/ and patches/: "+root.string());
  }
  fs::path compiled=GEKKOAOT_SOURCE_DIR;
  if(fs::exists(compiled/"runtime") && fs::exists(compiled/"CMakeLists.txt")) return compiled;
  fs::path cwd=fs::current_path(); if(fs::exists(cwd/"runtime") && fs::exists(cwd/"patches")) return cwd;
  const auto app=ExecutablePath().parent_path();
  const auto installed=fs::weakly_canonical(app/"../share/gekkoaot");
  if(fs::exists(installed/"runtime") && fs::exists(installed/"patches")) return installed;
  throw std::runtime_error("cannot locate GekkoAOT runtime resources");
}

fs::path DefaultStateRoot(const fs::path& root) {
  if (const auto explicit_state = Env("GEKKOAOT_STATE_DIR"); !explicit_state.empty())
    return fs::absolute(explicit_state);

  // Keep existing developer workflows local to the checkout, but never write
  // caches into an installed share/gekkoaot tree from packaged releases.
  if (fs::exists(root / "CMakeLists.txt"))
    return root / ".gekkoaot";

#ifdef _WIN32
  if (const auto local = Env("LOCALAPPDATA"); !local.empty())
    return fs::path(local) / "GekkoAOT";
  if (const auto home = Env("USERPROFILE"); !home.empty())
    return fs::path(home) / "AppData/Local/GekkoAOT";
#else
  if (const auto xdg = Env("XDG_CACHE_HOME"); !xdg.empty())
    return fs::path(xdg) / "gekkoaot";
  if (const auto home = Env("HOME"); !home.empty())
    return fs::path(home) / ".cache/gekkoaot";
#endif
  return fs::temp_directory_path() / "gekkoaot";
}
bool BundledToolchainReady(const fs::path& root) {
  const auto tc=root/"toolchain";
  if(!fs::exists(tc/"engine.ready") ||
     !fs::exists(tc/"src/DolRecomp/src/cpu/cpu.c"))
    return false;
#ifdef _WIN32
  return fs::exists(tc/"bin/dolrecomp.exe") &&
         fs::exists(tc/"bin/gekkoaot-native-gx.dll");
#else
  return fs::exists(tc/"bin/dolrecomp") &&
         fs::exists(tc/"bin/libgekkoaot-native-gx.so");
#endif
}

void SetEnvValue(const char* name,const std::string& value) {
#ifdef _WIN32
  _putenv_s(name,value.c_str());
#else
  setenv(name,value.c_str(),1);
#endif
}

void PrependEnvPath(const char* name,const fs::path& value) {
  if(value.empty() || !fs::exists(value)) return;
  const auto old=Env(name);
#ifdef _WIN32
  constexpr char sep=';';
#else
  constexpr char sep=':';
#endif
  SetEnvValue(name,value.string()+(old.empty()?std::string{}:std::string(1,sep)+old));
}

void ConfigureBundledToolchain(const fs::path& root) {
  if(!BundledToolchainReady(root)) return;
  const auto tc=root/"toolchain";
  PrependEnvPath("PATH",tc/"bin");
  PrependEnvPath("PATH",tc/"cmake/bin");
  PrependEnvPath("PATH",tc/"zig");
#ifndef _WIN32
  PrependEnvPath("LD_LIBRARY_PATH",tc/"lib");
  PrependEnvPath("LD_LIBRARY_PATH",tc/"bin");
#endif
  const auto cmake_toolchain=tc/"gekkoaot-zig.cmake";
  if(fs::exists(cmake_toolchain))
    SetEnvValue("CMAKE_TOOLCHAIN_FILE",cmake_toolchain.string());
#ifdef _WIN32
  SetEnvValue("DOLRECOMP_LLVM_TARGET","x86_64-w64-windows-gnu");
#else
  SetEnvValue("DOLRECOMP_LLVM_TARGET","x86_64-unknown-linux-gnu");
#endif
  fs::path prof=tc/"bin";
#ifdef _WIN32
  prof/="llvm-profdata.exe";
#else
  prof/="llvm-profdata";
#endif
  if(fs::exists(prof) && Env("GEKKOAOT_LLVM_PROFDATA").empty())
    SetEnvValue("GEKKOAOT_LLVM_PROFDATA",prof.string());
  SetEnvValue("GEKKOAOT_BUNDLED_TOOLCHAIN","1");
  std::cout<<"GEKKOAOT_PORTABLE_TOOLCHAIN_V1=1 git=embedded-sources "
              "cmake=bundled ninja=bundled compiler=zig engine=prebuilt\n";
}

fs::path Sibling(std::string name) {
#ifdef _WIN32
  name += ".exe";
#endif
  const auto beside=ExecutablePath().parent_path()/name;
  if(fs::exists(beside)) return beside;

  // Developer/source-tree fallback. build-linux.sh also copies helpers into
  // root/bin, but this keeps the controller usable when only the CMake build
  // tree was refreshed.
  const fs::path source=GEKKOAOT_SOURCE_DIR;
  const auto configured=Env("GEKKOAOT_BUILD_DIR");
  const std::array<fs::path,4> candidates = {
      configured.empty() ? fs::path{} : fs::path(configured)/"bin"/name,
      source/"build/bin"/name,
      source/"build-gui-linux/bin"/name,
      source/"build/Release"/name,
  };
  for(const auto& candidate:candidates)
    if(!candidate.empty() && fs::exists(candidate)) return candidate;
  throw std::runtime_error("required executable missing: "+name+
                           " (build GekkoAOT so the helper tools are installed beside gekkoaotctl)");
}
std::string Head(const fs::path& repo) {
  const fs::path tmp=repo/".gekkoaot-head";
  std::string command="git -C "+Quote(repo)+" rev-parse HEAD > "+Quote(tmp)+" 2>nul";
#ifndef _WIN32
  command="git -C "+Quote(repo)+" rev-parse HEAD > "+Quote(tmp)+" 2>/dev/null";
#endif
  if(Run(command)!=0) return {};
  auto s=ReadLine(tmp); std::error_code ec; fs::remove(tmp,ec); return s;
}
void EnsureCheckout(const fs::path& dir,std::string_view url,std::string_view rev) {
  if(fs::exists(dir/".git") && Head(dir)==rev) { std::cout<<"cache hit: "<<dir<<" @ "<<rev.substr(0,8)<<'\n'; return; }
  fs::create_directories(dir.parent_path());
  if(!fs::exists(dir/".git")) {
    std::error_code ec; fs::remove_all(dir,ec);
    Require(Run("git clone --filter=blob:none --no-checkout "+QuoteText(url)+" "+Quote(dir)),"git clone");
  }
  Require(Run("git -C "+Quote(dir)+" fetch --depth 1 origin "+QuoteText(rev)),"git fetch");
  Require(Run("git -C "+Quote(dir)+" checkout --detach "+QuoteText(rev)),"git checkout");
}
std::string Slug(fs::path p) {
  std::string s=Env("GEKKOAOT_GAME",p.stem().string()); for(char& c:s) if(!std::isalnum(static_cast<unsigned char>(c)))c='-';
  while (!s.empty() && s.front() == '-')
    s.erase(s.begin());
  while (!s.empty() && s.back() == '-')
    s.pop_back();
  return s.empty() ? "game" : s;
}
std::string SharedSuffix() {
#ifdef _WIN32
  return ".dll";
#elif defined(__APPLE__)
  return ".dylib";
#else
  return ".so";
#endif
}
fs::path FindGenerated(const fs::path& parent) {
  if(fs::exists(parent/"generated/generated.h")) return parent/"generated";
  for(const auto& e:fs::directory_iterator(parent)) if(e.is_directory() && fs::exists(e.path()/"generated.h")) return e.path();
  throw std::runtime_error("DolRecomp did not produce generated.h");
}
fs::path FindFileRecursive(const fs::path& root,std::string_view filename) {
  for (const auto& e : fs::recursive_directory_iterator(root)) {
    if (e.is_regular_file() && e.path().filename() == filename)
      return e.path();
  }
  return {};
}
fs::path FindLlvmProfdata() {
  const auto explicit_path=Env("GEKKOAOT_LLVM_PROFDATA");
  if(!explicit_path.empty() && fs::exists(explicit_path)) return fs::absolute(explicit_path);

  const auto llvm_dir=Env("GEKKOAOT_LLVM_DIR");
  if(!llvm_dir.empty()) {
    fs::path cursor=fs::absolute(llvm_dir);
    for(unsigned up=0;up<5;++up) {
      const auto candidate=cursor/"bin"/
#ifdef _WIN32
          "llvm-profdata.exe";
#else
          "llvm-profdata";
#endif
      if(fs::exists(candidate)) return candidate;
      if(!cursor.has_parent_path()) break;
      cursor=cursor.parent_path();
    }
  }

#ifdef _WIN32
  const std::array<const char*,5> names={"llvm-profdata.exe","llvm-profdata-22.exe","llvm-profdata-21.exe","llvm-profdata-20.exe","llvm-profdata-19.exe"};
  for(const auto* name:names) {
    const auto probe=fs::temp_directory_path()/(std::string("gekkoaot-")+name+".txt");
    if(Run("where "+QuoteText(name)+" > "+Quote(probe)+" 2>nul")==0) {
      const auto found=ReadLine(probe); std::error_code ec; fs::remove(probe,ec);
      if(!found.empty() && fs::exists(found)) return fs::path(found);
    }
  }
#else
  const std::array<const char*,5> names={"llvm-profdata","llvm-profdata-22","llvm-profdata-21","llvm-profdata-20","llvm-profdata-19"};
  for(const auto* name:names) {
    const auto probe=fs::temp_directory_path()/(std::string("gekkoaot-")+name+".txt");
    if(Run("command -v "+QuoteText(name)+" > "+Quote(probe)+" 2>/dev/null")==0) {
      const auto found=ReadLine(probe); std::error_code ec; fs::remove(probe,ec);
      if(!found.empty() && fs::exists(found)) return fs::path(found);
    }
  }
#endif
  throw std::runtime_error("llvm-profdata was not found; set GEKKOAOT_LLVM_PROFDATA or install LLVM tools");
}

std::vector<fs::path> FindProfiles(const fs::path& root) {
  std::vector<fs::path> profiles;
  if(!fs::exists(root)) return profiles;
  for(const auto& e:fs::recursive_directory_iterator(root)) {
    if(!e.is_regular_file() || e.path().extension()!=".profraw") continue;
    std::error_code ec;
    const auto bytes=fs::file_size(e.path(),ec);
    if(!ec && bytes>0) profiles.push_back(e.path());
  }
  std::sort(profiles.begin(),profiles.end());
  return profiles;
}

struct Pipeline {
  fs::path root, state, src, build, cache, disc, dolrecomp_src, aurora_src, sdk_manifest;
  fs::path native_run,native_disc,meta_tool,secondary_tool;
  std::string game_id;
  explicit Pipeline(const fs::path& image):root(Root()),state(DefaultStateRoot(root)),src(state/"src"),build(state/"build"),cache(state/"cache"),disc(cache/"discs"/Slug(image)),dolrecomp_src(src/"DolRecomp"),aurora_src(src/"Aurora"),native_run(Sibling("gekkoaot-native-run")),native_disc(Sibling("gekkoaot-native-disc")),meta_tool(Sibling("gekkoaot-module-meta")),secondary_tool(Sibling("gekkoaot-secondary-build")){}
};

void EnsureSources(Pipeline& p) {
  const auto bundled=p.root/"toolchain";
  const auto bundled_dol=bundled/"src/DolRecomp";
  if(BundledToolchainReady(p.root) &&
     fs::exists(bundled_dol/"src/cpu/cpu.c")) {
    p.dolrecomp_src=bundled_dol;
    std::cout<<"GEKKOAOT_BUNDLED_SOURCES_V1=1 dolrecomp=prepatched git=not-required\n";
    return;
  }
  EnsureCheckout(p.dolrecomp_src,"https://github.com/ExpansionPak/DolRecomp.git",GEKKOAOT_DOLRECOMP_REV);
  // The source cache used to be patched in-place by older GekkoAOT revisions.
  // HEAD alone cannot detect those tracked modifications, which can silently
  // resurrect stale LLVM fallbacks while still printing a cache hit.  The
  // checkout is tool-owned, so make the pinned DolRecomp tree reproducible.
  if (Env("GEKKOAOT_ALLOW_DIRTY_DOLRECOMP","0") != "1") {
    Require(Run("git -C "+Quote(p.dolrecomp_src)+" reset --hard "+QuoteText(GEKKOAOT_DOLRECOMP_REV)),
            "DolRecomp source reset");
    Require(Run("git -C "+Quote(p.dolrecomp_src)+" clean -ffd"),
            "DolRecomp source clean");

    // Standalone GekkoAOT deliberately has no ModernGekko/Dolphin interpreter
    // fallback.  Keep the small compiler-side resume patch reproducible after
    // every reset so architectural lazy-FPU RFI targets remain AOT-native.
    const auto native_resume_patch =
        p.root / "patches/dolrecomp/gekkoaot-native-resume-v1.patch";
    if (!fs::exists(native_resume_patch))
      throw std::runtime_error("missing DolRecomp native-resume patch: "+
                               native_resume_patch.string());
    Require(Run("git -C "+Quote(p.dolrecomp_src)+" apply --check "+
                Quote(native_resume_patch)),
            "DolRecomp native-resume patch check");
    Require(Run("git -C "+Quote(p.dolrecomp_src)+" apply "+
                Quote(native_resume_patch)),
            "DolRecomp native-resume patch");
    std::cout << "GEKKOAOT_DOLRECOMP_NATIVE_RESUME_V1=1 "
                 "fp-rfi-reentry=native msr-reentry=upstream\n";

    // The final AOT pass consumes a build-time SDK/HLE manifest and removes
    // ppc_native_region_available() from chunks that provably cannot contain a
    // native hook. This is a universal compiler specialization: no game IDs or
    // hand-authored addresses are embedded in the compiler.
    const auto static_intercepts_patch =
        p.root / "patches/dolrecomp/gekkoaot-static-intercepts-v2.patch";
    if (!fs::exists(static_intercepts_patch))
      throw std::runtime_error("missing DolRecomp static-intercepts patch: "+
                               static_intercepts_patch.string());
    Require(Run("git -C "+Quote(p.dolrecomp_src)+" apply --check "+
                Quote(static_intercepts_patch)),
            "DolRecomp static-intercepts patch check");
    Require(Run("git -C "+Quote(p.dolrecomp_src)+" apply "+
                Quote(static_intercepts_patch)),
            "DolRecomp static-intercepts patch");
    std::cout << "GEKKOAOT_DOLRECOMP_STATIC_INTERCEPTS_V2=1 "
                 "policy=compile-time-manifest safe-regions=no-runtime-probe\n";

    // v70: positive SDK/HLE interceptions are no longer rediscovered through
    // ppc_native_region_available -> module_dispatch -> DispatchHostCall.  The
    // build-time manifest carries a stable service token and DolRecomp emits an
    // exact entry-PC switch straight to CPUState::host_call.  Unresolved/legacy
    // manifests retain the v2 dynamic path as a fail-safe.
    const auto static_direct_patch =
        p.root / "patches/dolrecomp/gekkoaot-static-direct-hle-v3.patch";
    if (!fs::exists(static_direct_patch))
      throw std::runtime_error("missing DolRecomp static-direct HLE patch: "+
                               static_direct_patch.string());
    Require(Run("git -C "+Quote(p.dolrecomp_src)+" apply --check "+
                Quote(static_direct_patch)),
            "DolRecomp static-direct HLE patch check");
    Require(Run("git -C "+Quote(p.dolrecomp_src)+" apply "+
                Quote(static_direct_patch)),
            "DolRecomp static-direct HLE patch");
    std::cout << "GEKKOAOT_DOLRECOMP_STATIC_DIRECT_HLE_V3=1 "
                 "policy=compile-time-positive-lowering fallback=dynamic\n";

    // v72: a direct-token host call is a real AOT/runtime boundary. Native ABI
    // callers can keep guest registers in SSA, so CPUState must be synchronized
    // before NativeOS or another HLE service reads arguments/context from it.
    const auto static_direct_sync_patch =
        p.root / "patches/dolrecomp/gekkoaot-static-direct-hle-state-sync-v4.patch";
    if (!fs::exists(static_direct_sync_patch))
      throw std::runtime_error("missing DolRecomp static-direct state-sync patch: "+
                               static_direct_sync_patch.string());
    Require(Run("git -C "+Quote(p.dolrecomp_src)+" apply --check "+
                Quote(static_direct_sync_patch)),
            "DolRecomp static-direct state-sync patch check");
    Require(Run("git -C "+Quote(p.dolrecomp_src)+" apply "+
                Quote(static_direct_sync_patch)),
            "DolRecomp static-direct state-sync patch");
    std::cout << "GEKKOAOT_DOLRECOMP_STATIC_DIRECT_HLE_STATE_SYNC_V4=1 "
                 "boundary=materialize-native-abi-state before=host-call\n";

    // RecompCore historically returned to the host after only 256 guest cycles.
    // That was useful while the runtime was interpreter-like, but it makes an
    // AOT compatibility layer spend a large fraction of its time in dispatcher
    // and device bookkeeping. Keep timing exact at each boundary while making
    // the quantum a compile-time policy, so normal builds and PGO builds can
    // choose a larger native slice without adding a runtime branch.
    const auto native_budget_patch =
        p.root / "patches/dolrecomp/gekkoaot-native-budget-v1.patch";
    if (!fs::exists(native_budget_patch))
      throw std::runtime_error("missing DolRecomp native-budget patch: "+
                               native_budget_patch.string());
    Require(Run("git -C "+Quote(p.dolrecomp_src)+" apply --check "+
                Quote(native_budget_patch)),
            "DolRecomp native-budget patch check");
    Require(Run("git -C "+Quote(p.dolrecomp_src)+" apply "+
                Quote(native_budget_patch)),
            "DolRecomp native-budget patch");
    std::cout << "GEKKOAOT_DOLRECOMP_NATIVE_BUDGET_V1=1 "
                 "policy=compile-time-quantum\n";

    // v3.3 hot-path compiler specialization: small native leaf calls are
    // eligible for cross-partition inlining and can elide redundant call-edge
    // budget guards, while fixed GameCube memory layouts use a single constant
    // MEM1 bounds check and dead-strip the MEM2 branch.
    const auto superblock_patch =
        p.root / "patches/dolrecomp/gekkoaot-superblock-v3.patch";
    if (!fs::exists(superblock_patch))
      throw std::runtime_error("missing DolRecomp superblock patch: "+
                               superblock_patch.string());
    Require(Run("git -C "+Quote(p.dolrecomp_src)+" apply --check "+
                Quote(superblock_patch)),
            "DolRecomp superblock patch check");
    Require(Run("git -C "+Quote(p.dolrecomp_src)+" apply "+
                Quote(superblock_patch)),
            "DolRecomp superblock patch");
    std::cout << "GEKKOAOT_DOLRECOMP_SUPERBLOCK_V3=1 "
                 "inline-small-native=1 relaxed-call-guard=1 gc-mem1-fastpath=1\n";

    // v3.4 universal hot-path pass: force the inlining policy at call sites so
    // ThinLTO imports compact native helpers across partitions, and feed loop
    // guards branch probabilities matching the overwhelmingly non-expired case.
    const auto universal_hotpath_patch =
        p.root / "patches/dolrecomp/gekkoaot-universal-hotpath-v4.patch";
    if (!fs::exists(universal_hotpath_patch))
      throw std::runtime_error("missing DolRecomp universal-hotpath patch: "+
                               universal_hotpath_patch.string());
    Require(Run("git -C "+Quote(p.dolrecomp_src)+" apply --check "+
                Quote(universal_hotpath_patch)),
            "DolRecomp universal-hotpath patch check");
    Require(Run("git -C "+Quote(p.dolrecomp_src)+" apply "+
                Quote(universal_hotpath_patch)),
            "DolRecomp universal-hotpath patch");
    std::cout << "GEKKOAOT_DOLRECOMP_UNIVERSAL_HOTPATH_V4=1 "
                 "callsite-inline=1 inline-hint=1 loop-guard-likely=1\n";

    // v3.8 PGO trainer: expose LLVM 20's sampled IR instrumentation through a
    // dedicated environment option. This keeps instrumentation identities
    // unchanged while avoiding most expensive profile-counter writes.
    const auto sampled_pgo_patch =
        p.root / "patches/dolrecomp/gekkoaot-pgo-sampled-v5.patch";
    if (!fs::exists(sampled_pgo_patch))
      throw std::runtime_error("missing DolRecomp sampled-PGO patch: "+
                               sampled_pgo_patch.string());
    Require(Run("git -C "+Quote(p.dolrecomp_src)+" apply --check "+
                Quote(sampled_pgo_patch)),
            "DolRecomp sampled-PGO patch check");
    Require(Run("git -C "+Quote(p.dolrecomp_src)+" apply "+
                Quote(sampled_pgo_patch)),
            "DolRecomp sampled-PGO patch");
    std::cout << "GEKKOAOT_DOLRECOMP_PGO_SAMPLED_V5=1 "
                 "backend=llvm-ir instrumentation=sampled\n";

    // v3.9 adaptive AOT: make reservation invalidation cheap on every build,
    // then let PGO select the small set of guest functions that receive LLVM
    // hot attributes before the single IR-PGO/O3 pipeline.  Never run a second
    // module pipeline: LLVM PGO owns metadata such as "CG Profile".
    const auto adaptive_hot_patch =
        p.root / "patches/dolrecomp/gekkoaot-adaptive-hot-v6.patch";
    if (!fs::exists(adaptive_hot_patch))
      throw std::runtime_error("missing DolRecomp adaptive-hot patch: "+
                               adaptive_hot_patch.string());
    Require(Run("git -C "+Quote(p.dolrecomp_src)+" apply --unidiff-zero --check "+
                Quote(adaptive_hot_patch)),
            "DolRecomp adaptive-hot patch check");
    Require(Run("git -C "+Quote(p.dolrecomp_src)+" apply --unidiff-zero "+
                Quote(adaptive_hot_patch)),
            "DolRecomp adaptive-hot patch");
    std::cout << "GEKKOAOT_DOLRECOMP_ADAPTIVE_HOT_V8_1=1 "
                 "reservation=branchless-hot+lazy-cold selector=pgo-manifest state-residency=pgo-hot-full-ssa pipeline=single-ir-pgo-o3\n";

    // v4.0 structural translator pack, independently reimplemented from the
    // strongest WiiCompiled ideas: r1 stack facts / resolved guest-memory
    // ranges and direct known-hardware stores. This is intentionally applied
    // after adaptive residency so the stack base is promotable SSA.
    const auto structural_patch =
        p.root / "patches/dolrecomp/gekkoaot-wiicompiled-structural-v9.patch";
    if (!fs::exists(structural_patch))
      throw std::runtime_error("missing DolRecomp structural translator patch: "+
                               structural_patch.string());
    Require(Run("git -C "+Quote(p.dolrecomp_src)+" apply --unidiff-zero --check "+
                Quote(structural_patch)),
            "DolRecomp structural translator patch check");
    Require(Run("git -C "+Quote(p.dolrecomp_src)+" apply --unidiff-zero "+
                Quote(structural_patch)),
            "DolRecomp structural translator patch");
    std::cout << "GEKKOAOT_DOLRECOMP_STRUCTURAL_V9=1 "
                 "stack-facts=adaptive-r1-mem1 range=4k fifo=direct-known-store inspiration=wiicompiled-cleanroom\n";

    // v4.1: make --targets=host actually use the detected host CPU and LLVM
    // feature set instead of target.cpp's historical generic CPU profile.
    const auto host_native_patch =
        p.root / "patches/dolrecomp/gekkoaot-host-native-v10.patch";
    if (!fs::exists(host_native_patch))
      throw std::runtime_error("missing DolRecomp host-native patch: "+
                               host_native_patch.string());
    Require(Run("git -C "+Quote(p.dolrecomp_src)+" apply --check "+
                Quote(host_native_patch)),
            "DolRecomp host-native patch check");
    Require(Run("git -C "+Quote(p.dolrecomp_src)+" apply "+
                Quote(host_native_patch)),
            "DolRecomp host-native patch");
    std::cout << "GEKKOAOT_DOLRECOMP_HOST_NATIVE_V10=1 "
                 "target=detected-host-cpu features=cpu-default\n";

    // v4.2: PowerPC switch statements commonly use a data-resident jump table
    // followed by mtctr/bctr. The LLVM entry-point scanner cannot infer those
    // data-held interior code addresses, so make only functions containing a
    // computed CTR branch fully re-enterable. This keeps jump-table targets
    // AOT-native without globally fragmenting every function.
    const auto bcctr_reentry_patch =
        p.root / "patches/dolrecomp/gekkoaot-bcctr-reentry-v11.patch";
    if (!fs::exists(bcctr_reentry_patch))
      throw std::runtime_error("missing DolRecomp bcctr-reentry patch: "+
                               bcctr_reentry_patch.string());
    Require(Run("git -C "+Quote(p.dolrecomp_src)+" apply --check "+
                Quote(bcctr_reentry_patch)),
            "DolRecomp bcctr-reentry patch check");
    Require(Run("git -C "+Quote(p.dolrecomp_src)+" apply "+
                Quote(bcctr_reentry_patch)),
            "DolRecomp bcctr-reentry patch");
    std::cout << "GEKKOAOT_DOLRECOMP_BCCTR_REENTRY_V11=1 "
                 "policy=function-local-all-instruction-entrypoints\n";

    // v75: PPC titles frequently synchronize OSThreads with tiny shared-memory
    // polling loops.  Those guest RAM loads must remain observable across
    // runtime scheduler/IRQ/DMA side exits; otherwise LLVM may legally hoist a
    // non-volatile MEM1 load and turn the wait into an infinite native loop.
    // Apply this after all memory/region optimization patches so only proven
    // loop-member guest RAM loads are affected.
    const auto shared_polling_patch =
        p.root / "patches/dolrecomp/gekkoaot-shared-polling-v12.patch";
    if (!fs::exists(shared_polling_patch))
      throw std::runtime_error("missing DolRecomp shared-polling patch: "+
                               shared_polling_patch.string());
    Require(Run("git -C "+Quote(p.dolrecomp_src)+" apply --check "+
                Quote(shared_polling_patch)),
            "DolRecomp shared-polling patch check");
    Require(Run("git -C "+Quote(p.dolrecomp_src)+" apply "+
                Quote(shared_polling_patch)),
            "DolRecomp shared-polling patch");
    std::cout << "GEKKOAOT_DOLRECOMP_SHARED_POLLING_V12=1 "
                 "scope=loop-member-guest-ram-loads semantics=volatile-on-polling-loop\n";


    // v76: direct-token scheduler/wait HLE can capture the caller OSContext.
    // The callee's local PPC liveness is not enough for that boundary: values
    // such as r13-r31 may live only in an ancestor Native ABI frame.  The SDK
    // manifest marks context-capturing hooks and this compiler patch turns that
    // class into propagated Native ABI inputs before code generation.
    const auto context_capture_abi_patch =
        p.root / "patches/dolrecomp/gekkoaot-context-capture-abi-v13.patch";
    if (!fs::exists(context_capture_abi_patch))
      throw std::runtime_error("missing DolRecomp context-capture ABI patch: "+
                               context_capture_abi_patch.string());
    Require(Run("git -C "+Quote(p.dolrecomp_src)+" apply --check "+
                Quote(context_capture_abi_patch)),
            "DolRecomp context-capture ABI patch check");
    Require(Run("git -C "+Quote(p.dolrecomp_src)+" apply "+
                Quote(context_capture_abi_patch)),
            "DolRecomp context-capture ABI patch");
    std::cout << "GEKKOAOT_DOLRECOMP_CONTEXT_CAPTURE_ABI_V13=1 "
                 "policy=manifest-propagated-caller-state boundary=native-os-context\n";

    const auto memory_restart_patch =
        p.root / "patches/dolrecomp/gekkoaot-memory-restart-v14.patch";
    if (!fs::exists(memory_restart_patch))
      throw std::runtime_error("missing DolRecomp memory-restart patch: "+
                               memory_restart_patch.string());
    Require(Run("git -C "+Quote(p.dolrecomp_src)+" apply --check "+
                Quote(memory_restart_patch)), "DolRecomp memory-restart patch check");
    Require(Run("git -C "+Quote(p.dolrecomp_src)+" apply "+
                Quote(memory_restart_patch)), "DolRecomp memory-restart patch");
    std::cout << "GEKKOAOT_DOLRECOMP_MEMORY_RESTART_V14=1 "
                 "resume=faulting-load-store exception-return=native\n";
    const auto memory_capability_patch =
        p.root / "patches/dolrecomp/gekkoaot-memory-capability-v15.patch";
    if (!fs::exists(memory_capability_patch))
      throw std::runtime_error("missing DolRecomp memory capability patch: "+
                               memory_capability_patch.string());
    Require(Run("git -C "+Quote(p.dolrecomp_src)+" apply --check "+
                Quote(memory_capability_patch)), "DolRecomp memory capability patch check");
    Require(Run("git -C "+Quote(p.dolrecomp_src)+" apply "+
                Quote(memory_capability_patch)), "DolRecomp memory capability patch");
    std::cout << "GEKKOAOT_DOLRECOMP_MEMORY_CAPABILITY_V15=1 "
                 "policy=native-module-exports-restart-capability\n";
  }
  // GekkoAOT owns the GameCube-facing runtime and FIFO decoder. Aurora is the
  // renderer/presentation backend only; no ModernGekko or Dolphin execution
  // chassis is fetched or linked by the standalone pipeline.
  EnsureCheckout(p.aurora_src,"https://github.com/encounter/aurora.git",GEKKOAOT_AURORA_REV);
  // Aurora is patched in-place below, so a previous failed/successful run can
  // leave the source cache dirty while HEAD still equals the pinned revision.
  // Reset the tool-owned checkout before reapplying local patches, mirroring
  // the DolRecomp source policy above.
  if (Env("GEKKOAOT_ALLOW_DIRTY_AURORA","0") != "1") {
    Require(Run("git -C "+Quote(p.aurora_src)+" reset --hard "+QuoteText(GEKKOAOT_AURORA_REV)),
            "Aurora source reset");
    Require(Run("git -C "+Quote(p.aurora_src)+" clean -ffd"),
            "Aurora source clean");
  }
  Require(Run("git -C "+Quote(p.aurora_src)+" submodule sync --recursive"),
          "Aurora submodule sync");
  Require(Run("git -C "+Quote(p.aurora_src)+" submodule update --init --recursive --force"),
          "Aurora submodule checkout");
  const auto aurora_raw_draw_patch =
      p.root / "patches/aurora/gekkoaot-raw-draw-v1.patch";
  if (!fs::exists(aurora_raw_draw_patch))
    throw std::runtime_error("missing Aurora raw-draw patch: "+
                             aurora_raw_draw_patch.string());
  Require(Run("git -C "+Quote(p.aurora_src)+" apply --check "+
              Quote(aurora_raw_draw_patch)),
          "Aurora raw-draw patch check");
  Require(Run("git -C "+Quote(p.aurora_src)+" apply "+
              Quote(aurora_raw_draw_patch)),
          "Aurora raw-draw patch");
  std::cout << "GEKKOAOT_AURORA_RAW_DRAW_V1=1 parser=bypass-for-framed-draws merge=retained\n";

  const auto aurora_storage_patch =
      p.root / "patches/aurora/gekkoaot-storage-staging-v4.patch";
  if (!fs::exists(aurora_storage_patch))
    throw std::runtime_error("missing Aurora storage-staging patch: "+
                             aurora_storage_patch.string());
  Require(Run("git -C "+Quote(p.aurora_src)+" apply --check "+
              Quote(aurora_storage_patch)),
          "Aurora storage-staging patch check");
  Require(Run("git -C "+Quote(p.aurora_src)+" apply "+
              Quote(aurora_storage_patch)),
          "Aurora storage-staging patch");
  std::cout << "GEKKOAOT_AURORA_STORAGE_STAGING_V4=1 segment_bytes=67108864 gpu_bytes=67108864 "
               "overflow=pass-boundary-segmented-staging offsets=restart-at-zero single-draw=fail-closed\n";

  // Aurora is a renderer embedded in GekkoAOT, not the owner of the process-wide
  // SDL lifetime. Its upstream window shutdown calls SDL_Quit(), which also
  // tears down GekkoAOT-owned gamepad/haptic/audio subsystems. The host then
  // releases those stale objects in HostRuntime::~HostRuntime(), which glibc
  // reports as double-free/corrupted heap on normal exit. Restrict Aurora to
  // releasing only the VIDEO|EVENTS subsystems that its window layer acquires.
  const auto aurora_sdl_lifetime_patch =
      p.root / "patches/aurora/gekkoaot-sdl-shared-lifetime-v4.patch";
  if (!fs::exists(aurora_sdl_lifetime_patch))
    throw std::runtime_error("missing Aurora shared-SDL-lifetime patch: "+
                             aurora_sdl_lifetime_patch.string());
  Require(Run("git -C "+Quote(p.aurora_src)+" apply --unidiff-zero --check "+
              Quote(aurora_sdl_lifetime_patch)),
          "Aurora shared-SDL-lifetime patch check");
  Require(Run("git -C "+Quote(p.aurora_src)+" apply --unidiff-zero "+
              Quote(aurora_sdl_lifetime_patch)),
          "Aurora shared-SDL-lifetime patch");
  std::cout << "GEKKOAOT_AURORA_SDL_SHARED_LIFETIME_V85=1 "
               "aurora=video+events host=gamepad+haptic+audio global-quit=forbidden\n";

  const auto aurora_xf_zero_stride_patch =
      p.root / "patches/aurora/gekkoaot-xf-zero-stride-v1.patch";
  if (!fs::exists(aurora_xf_zero_stride_patch))
    throw std::runtime_error("missing Aurora zero-stride XF patch: "+
                             aurora_xf_zero_stride_patch.string());
  Require(Run("git -C "+Quote(p.aurora_src)+" apply --check "+
              Quote(aurora_xf_zero_stride_patch)),
          "Aurora zero-stride XF patch check");
  Require(Run("git -C "+Quote(p.aurora_src)+" apply "+
              Quote(aurora_xf_zero_stride_patch)),
          "Aurora zero-stride XF patch");
  std::cout << "GEKKOAOT_AURORA_XF_ZERO_STRIDE_V1=1 policy=base-plus-index-times-stride bounds=retained\n";

  const auto aurora_bind_group_lifetime_patch =
      p.root / "patches/aurora/gekkoaot-bind-group-lifetime-v1.patch";
  if (!fs::exists(aurora_bind_group_lifetime_patch))
    throw std::runtime_error("missing Aurora bind-group lifetime patch: "+
                             aurora_bind_group_lifetime_patch.string());
  Require(Run("git -C "+Quote(p.aurora_src)+" apply --check "+
              Quote(aurora_bind_group_lifetime_patch)),
          "Aurora bind-group lifetime patch check");
  Require(Run("git -C "+Quote(p.aurora_src)+" apply "+
              Quote(aurora_bind_group_lifetime_patch)),
          "Aurora bind-group lifetime patch");
  std::cout << "GEKKOAOT_AURORA_BIND_GROUP_LIFETIME_V1=1 eviction=submitted-frame-age\n";

  const auto aurora_shader_vertex_patch =
      p.root / "patches/aurora/gekkoaot-shader-vertex-v1.patch";
  if (!fs::exists(aurora_shader_vertex_patch))
    throw std::runtime_error("missing Aurora shader-vertex patch: "+
                             aurora_shader_vertex_patch.string());
  Require(Run("git -C "+Quote(p.aurora_src)+" apply --check "+
              Quote(aurora_shader_vertex_patch)),
          "Aurora shader-vertex patch check");
  Require(Run("git -C "+Quote(p.aurora_src)+" apply "+
              Quote(aurora_shader_vertex_patch)),
          "Aurora shader-vertex patch");
  std::cout << "GEKKOAOT_AURORA_SHADER_VERTEX_V1=1 nbt3=independent-indexed-vectors texmtx-identity=handled\n";

  const auto aurora_tev_alpha_compare_patch =
      p.root / "patches/aurora/gekkoaot-tev-alpha-compare-v2.patch";
  if (!fs::exists(aurora_tev_alpha_compare_patch))
    throw std::runtime_error("missing Aurora TEV alpha-compare patch: "+
                             aurora_tev_alpha_compare_patch.string());
  Require(Run("git -C "+Quote(p.aurora_src)+" apply --unidiff-zero --check "+
              Quote(aurora_tev_alpha_compare_patch)),
          "Aurora TEV alpha-compare patch check");
  Require(Run("git -C "+Quote(p.aurora_src)+" apply --unidiff-zero "+
              Quote(aurora_tev_alpha_compare_patch)),
          "Aurora TEV alpha-compare patch");
  std::cout << "GEKKOAOT_AURORA_TEV_ALPHA_COMPARE_V2B=1 compare=prestage-color-ab a8=alpha-ab wgsl=type-safe rebased=post-shader-vertex\n";
}
fs::path BuildDolRecomp(Pipeline& p,std::string_view backend) {
  std::cout << "GEKKOAOT_SUBSTAGE=dolrecomp-build\n" << std::flush;
  fs::path bundled=p.root/"toolchain/bin/dolrecomp";
#ifdef _WIN32
  bundled += ".exe";
#endif
  if(BundledToolchainReady(p.root) && fs::exists(bundled)) {
    std::cout<<"GEKKOAOT_BUNDLED_DOLRECOMP_V1=1 backend="<<backend
             <<" path=\""<<bundled.string()<<"\"\n";
    return bundled;
  }
  const auto b=p.build/"DolRecomp"; fs::create_directories(b);
  const bool llvm_backend=backend=="llvm";
  std::string cfg="cmake -S "+Quote(p.dolrecomp_src)+" -B "+Quote(b)+" -G Ninja -DCMAKE_BUILD_TYPE=Release -DDOLRECOMP_ENABLE_LLVM="+(llvm_backend?"ON":"OFF");
  const auto llvm=Env("GEKKOAOT_LLVM_DIR");
  if(llvm_backend&&!llvm.empty()) cfg += " -DLLVM_DIR="+Quote(fs::path(llvm));
  Require(Run(cfg),"DolRecomp configure"); Require(Run("cmake --build "+Quote(b)+" --target dolrecomp -j"+std::to_string(Jobs())),"DolRecomp build");
  fs::path exe=b/"dolrecomp";
#ifdef _WIN32
  if(!fs::exists(exe))exe=b/"Release/dolrecomp.exe";
#endif
  if (!fs::exists(exe))
    throw std::runtime_error("dolrecomp executable not found");
  return exe;
}
void PrepareCMakeBuildTree(const fs::path& build_dir, const fs::path& source_dir) {
  const auto cache = build_dir / "CMakeCache.txt";
  if (fs::exists(cache)) {
    std::ifstream in(cache);
    std::string line;
    std::string cached_source;
    constexpr std::string_view prefix = "CMAKE_HOME_DIRECTORY:INTERNAL=";
    while (std::getline(in, line)) {
      if (line.rfind(prefix, 0) == 0) {
        cached_source = line.substr(prefix.size());
        break;
      }
    }
    if (!cached_source.empty()) {
      std::error_code ec_a, ec_b;
      const auto wanted = fs::weakly_canonical(source_dir, ec_a);
      const auto cached = fs::weakly_canonical(fs::path(cached_source), ec_b);
      const bool same = !ec_a && !ec_b ? wanted == cached
                                      : fs::path(cached_source).lexically_normal() == source_dir.lexically_normal();
      if (!same) {
        std::cout << "==> stale CMake tree: " << build_dir
                  << " was configured from " << cached_source
                  << "; rebuilding for " << source_dir << '\n';
        std::error_code remove_ec;
        fs::remove_all(build_dir, remove_ec);
        if (remove_ec)
          throw std::runtime_error("cannot remove stale CMake build tree: "+build_dir.string()+
                                   " ("+remove_ec.message()+")");
      }
    }
  }
  fs::create_directories(build_dir);
}

fs::path BuildAuroraBridge(Pipeline& p) {
  std::cout << "GEKKOAOT_SUBSTAGE=native-gx-aurora-build\n" << std::flush;
  fs::path bundled=p.root/"toolchain/bin";
#ifdef _WIN32
  bundled/="gekkoaot-native-gx.dll";
#else
  bundled/="libgekkoaot-native-gx.so";
#endif
  if(BundledToolchainReady(p.root) && fs::exists(bundled)) {
    std::cout<<"GEKKOAOT_BUNDLED_AURORAGX_V1=1 path=\""<<bundled.string()<<"\"\n";
    return bundled;
  }
  if(!fs::exists(p.aurora_src/"CMakeLists.txt"))
    throw std::runtime_error("pinned Aurora source is missing: "+p.aurora_src.string());
  // The old runtime used .gekkoaot/build/NativeGX with runtime/gx/native as
  // its CMake source. Reusing that cache after the direct-Aurora migration
  // makes CMake abort before it can configure the new frontend. Keep a
  // backend-specific tree and also self-heal any stale CMake source binding.
  const auto b=p.build/"NativeGX-Aurora";
  PrepareCMakeBuildTree(b, p.root/"runtime/gx/aurora");
  const std::string cfg="cmake -S "+Quote(p.root/"runtime/gx/aurora")+" -B "+Quote(b)+
      " -G Ninja -DCMAKE_BUILD_TYPE=Release -DGEKKOAOT_AURORA_SOURCE="+Quote(p.aurora_src)+
      " -DCMAKE_POSITION_INDEPENDENT_CODE=ON";
  Require(Run(cfg),"native AuroraGX configure");
  Require(Run("cmake --build "+Quote(b)+" --target gekkoaot-native-gx -j"+std::to_string(Jobs())),
          "native AuroraGX build");
  fs::path lib=b/("libgekkoaot-native-gx"+SharedSuffix());
#ifdef _WIN32
  lib=b/"gekkoaot-native-gx.dll"; if(!fs::exists(lib))lib=b/"Release/gekkoaot-native-gx.dll";
#endif
  if(!fs::exists(lib)){auto x=FindFileRecursive(b,"libgekkoaot-native-gx"+SharedSuffix());if(!x.empty())lib=x;}
  if(!fs::exists(lib))
    throw std::runtime_error("native AuroraGX bridge not produced");
  std::cout << "GEKKOAOT_NATIVE_GX_DIRECT=1 frontend=gekkoaot-native-fifo backend=AuroraGX moderngekko=off dolphin=off\n";
  return lib;
}

void ExtractDisc(Pipeline& p,const fs::path& image) {
  std::cout<<"GEKKOAOT_STAGE=disc\nGEKKOAOT_PROGRESS=8|Reading disc with nod\n";
  // Re-extract if required system files are absent. Existing cache keeps repeated Play fast.
  if(!fs::exists(p.disc/"sys/main.dol")||!fs::exists(p.disc/"meta/disc-id.txt"))
    Require(Run(Quote(p.native_disc)+" "+Quote(image)+" "+Quote(p.disc)),"nod disc extraction");
  p.game_id=ReadLine(p.disc/"meta/disc-id.txt");
  if(p.game_id.size()!=6)throw std::runtime_error("invalid GameCube disc id in nod cache");
  std::cout<<"GEKKOAOT_PROGRESS=30|Disc executable cache ready\n";
}
enum class PgoMode : std::uint8_t { Off, Generate, Use };

struct CompilePlan {
  PgoMode pgo = PgoMode::Off;
  fs::path profile_path;
  fs::path adaptive_hot_manifest;
  unsigned chunk_instructions = 256u;
  unsigned guard_cycles = 2048u;
  unsigned guard_steps = 8192u;
  unsigned inline_bytes = 256u;
  unsigned inline_hint_bytes = 4096u;
  unsigned relaxed_guard_bytes = 4096u;
  bool thinlto = false;
};

const char* PgoModeName(PgoMode mode) {
  switch(mode) {
  case PgoMode::Generate: return "gen";
  case PgoMode::Use: return "use";
  case PgoMode::Off: default: return "off";
  }
}

struct PgoSamplingConfig {
  bool enabled=false;
  unsigned period=65536u;
  unsigned burst=1024u;
  unsigned scale=64u;
};

PgoSamplingConfig PgoSamplingFromEnv() {
  PgoSamplingConfig cfg;
  cfg.enabled=Env("GEKKOAOT_PGO_SAMPLED","1")!="0";
  if(!cfg.enabled) return cfg;
  cfg.period=EnvUnsigned("GEKKOAOT_PGO_SAMPLE_PERIOD",65536u,1024u,1048576u);
  cfg.burst=EnvUnsigned("GEKKOAOT_PGO_SAMPLE_BURST",1024u,1u,cfg.period);
  if(cfg.burst>cfg.period) cfg.burst=cfg.period;
  cfg.scale=(std::max)(1u,(cfg.period+(cfg.burst/2u))/cfg.burst);
  return cfg;
}

CompilePlan NormalCompilePlan() {
  CompilePlan plan;
  plan.inline_bytes=EnvUnsigned("GEKKOAOT_NATIVE_INLINE_BYTES",1024u,64u,4096u);
  plan.inline_hint_bytes=EnvUnsigned("GEKKOAOT_NATIVE_INLINE_HINT_BYTES",4096u,256u,4096u);
  plan.relaxed_guard_bytes=EnvUnsigned("GEKKOAOT_RELAXED_CALL_GUARD_BYTES",4096u,64u,4096u);
  plan.chunk_instructions=EnvUnsigned("GEKKOAOT_AOT_CHUNK_INSTRUCTIONS",1024u,64u,8192u);
  plan.guard_cycles=EnvUnsigned("GEKKOAOT_AOT_GUARD_CYCLES",262144u,256u,1048576u);
  plan.guard_steps=EnvUnsigned("GEKKOAOT_AOT_GUARD_STEPS",1048576u,2048u,1048576u);
  return plan;
}

CompilePlan PgoCompilePlan(PgoMode mode,const fs::path& profile) {
  CompilePlan plan;
  plan.inline_bytes=EnvUnsigned("GEKKOAOT_NATIVE_INLINE_BYTES",1024u,64u,4096u);
  plan.inline_hint_bytes=EnvUnsigned("GEKKOAOT_NATIVE_INLINE_HINT_BYTES",4096u,256u,4096u);
  plan.relaxed_guard_bytes=EnvUnsigned("GEKKOAOT_RELAXED_CALL_GUARD_BYTES",4096u,64u,4096u);
  plan.pgo=mode;
  plan.profile_path=profile;
  plan.chunk_instructions=EnvUnsigned("GEKKOAOT_PGO_CHUNK_INSTRUCTIONS",4096u,64u,4096u);
  plan.guard_cycles=EnvUnsigned("GEKKOAOT_PGO_GUARD_CYCLES",262144u,256u,1048576u);
  plan.guard_steps=EnvUnsigned("GEKKOAOT_PGO_GUARD_STEPS",1048576u,2048u,1048576u);
  plan.thinlto=mode==PgoMode::Use && Env("GEKKOAOT_PGO_THINLTO","1")!="0";
  return plan;
}

fs::path CompileModuleVariant(Pipeline& p,const fs::path& dolrecomp,std::string_view backend,
                              std::string_view policy_tag,const fs::path& static_intercepts,
                              CompilePlan plan=CompilePlan{},bool force=false) {
  const std::string intercept_hash=static_intercepts.empty()?"dynamic":FileHash64(static_intercepts);
  const bool llvm_backend=backend=="llvm";
  if(!llvm_backend && plan.pgo!=PgoMode::Off)
    throw std::runtime_error("PGO requires the LLVM backend");
  if(plan.pgo==PgoMode::Use && (plan.profile_path.empty() || !fs::exists(plan.profile_path)))
    throw std::runtime_error("PGO-use profile is missing: "+plan.profile_path.string());

  const std::string profile_hash=plan.pgo==PgoMode::Use ? FileHash64(plan.profile_path) :
                                 plan.pgo==PgoMode::Generate ? TextHash64(plan.profile_path.string()) : "none";
  const std::string adaptive_hash=!plan.adaptive_hot_manifest.empty() && fs::exists(plan.adaptive_hot_manifest)
      ? FileHash64(plan.adaptive_hot_manifest) : "none";
  const auto sampling=plan.pgo==PgoMode::Generate ? PgoSamplingFromEnv() : PgoSamplingConfig{};
  std::string tuning="c"+std::to_string(plan.chunk_instructions)+
      "-q"+std::to_string(plan.guard_cycles)+"-s"+std::to_string(plan.guard_steps)+
      "-i"+std::to_string(plan.inline_bytes)+"-ih"+std::to_string(plan.inline_hint_bytes)+
      "-rg"+std::to_string(plan.relaxed_guard_bytes)+
      "-pgo"+PgoModeName(plan.pgo)+(plan.thinlto?"-thin":"");
  if(sampling.enabled)
    tuning += "-sampled-p"+std::to_string(sampling.period)+"-b"+
              std::to_string(sampling.burst)+"-x"+std::to_string(sampling.scale);
  if(adaptive_hash!="none")
    tuning += "-adaptive-"+adaptive_hash+"-fullssa2-structural9-hostnative10";
  if(llvm_backend)
    tuning += "-sharedpoll75-ctxabi76-globalindirect87-memrestart95-vmcap97";
  const auto artifact=p.cache/"modules"/p.game_id/(std::string("v")+GEKKOAOT_VERSION+"-"+
      std::string(backend)+"-"+std::string(policy_tag)+"-"+tuning+"-"+profile_hash+"-"+
      intercept_hash+"-"+std::string(GEKKOAOT_DOLRECOMP_REV).substr(0,8)+"-"+
      std::string(GEKKOAOT_AURORA_REV).substr(0,8));
  const auto module=artifact/("g"+p.game_id+"_recomp"+SharedSuffix());
  if(fs::exists(module)&&!force){std::cout<<"cache hit: native module "<<module<<"\nGEKKOAOT_PROGRESS=93|Native module cache hit\n";return module;}
  fs::create_directories(artifact);
  const auto output=artifact/"dolrecomp-output"; if(force){std::error_code ec;fs::remove_all(output,ec);}
  fs::create_directories(output);
  std::cout<<"GEKKOAOT_STAGE=compile\nGEKKOAOT_PROGRESS=38|"<<(llvm_backend?"LLVM AOT":"Portable C AOT")<<" recompilation\n";
  std::string gen;
  if(llvm_backend) {
    const auto native_abi=Env("GEKKOAOT_NATIVE_ABI","unrestricted");
    const auto llvm_cache = p.cache/"dolrecomp-llvm"/(
        std::string("cache19-context-capture-v76-")+intercept_hash+"-"+tuning+"-"+profile_hash+"-"+
        std::string(GEKKOAOT_DOLRECOMP_REV).substr(0,8)+"-"+native_abi+
        "-state-in-memory-host-exact");
    fs::create_directories(llvm_cache);
    gen = "cmake -E env " +
          QuoteText(std::string("DOLRECOMP_LLVM_CACHE=")+llvm_cache.string()) + " " +
          QuoteText("DOLRECOMP_DISPATCH_LOOKUP=indexed") + " " +
          QuoteText(std::string("DOLRECOMP_LLVM_CHUNK_INSTRUCTIONS=")+std::to_string(plan.chunk_instructions)) + " " +
          QuoteText(std::string("DOLRECOMP_GUARD_CYCLES=")+std::to_string(plan.guard_cycles)) + " " +
          QuoteText(std::string("DOLRECOMP_GUARD_STEPS=")+std::to_string(plan.guard_steps)) + " " +
          QuoteText("DOLRECOMP_LLVM_CODEGEN_LEVEL=3") + " " +
          QuoteText(std::string("DOLRECOMP_NATIVE_INLINE_BYTES=")+std::to_string(plan.inline_bytes)) + " " +
          QuoteText(std::string("DOLRECOMP_NATIVE_INLINE_HINT_BYTES=")+std::to_string(plan.inline_hint_bytes)) + " " +
          QuoteText(std::string("DOLRECOMP_RELAXED_CALL_GUARD_BYTES=")+std::to_string(plan.relaxed_guard_bytes));
    if(sampling.enabled) {
      const std::string llvm_options="-enable-sampled-instrumentation -sampled-instr-period="+
          std::to_string(sampling.period)+" -sampled-instr-burst-duration="+
          std::to_string(sampling.burst);
      gen += " " + QuoteText(std::string("DOLRECOMP_LLVM_OPTIONS=")+llvm_options);
    }
    if(!static_intercepts.empty())
      gen += " " + QuoteText(std::string("DOLRECOMP_STATIC_INTERCEPTS=")+static_intercepts.string());
    if(adaptive_hash!="none")
      gen += " " + QuoteText(std::string("DOLRECOMP_ADAPTIVE_HOT_FUNCTIONS=")+plan.adaptive_hot_manifest.string());
    gen += " " + Quote(dolrecomp) +
          " -j"+std::to_string(Jobs())+" --backend="+std::string(backend)+
          " --cpu gekko --gamecube";
    gen += " --state-in-memory --native-abi="+native_abi+" --targets=host --semantics=exact";
    gen += " --partition-instructions "+std::to_string(plan.chunk_instructions);
    if(plan.pgo==PgoMode::Generate)
      gen += " --profile-generate "+Quote(plan.profile_path);
    else if(plan.pgo==PgoMode::Use)
      gen += " --profile-use "+Quote(plan.profile_path);
    std::cout<<"GEKKOAOT_LLVM_TURBO_V12=1 state-in-memory=1 native-abi="<<native_abi
             <<" target=host semantics=exact dispatch=indexed static-intercepts="
             <<(static_intercepts.empty()?"off":intercept_hash)<<" cache="<<llvm_cache<<"\n";
    std::cout<<"GEKKOAOT_NATIVE_AOT_QUANTUM_V12=1 chunk="<<plan.chunk_instructions
             <<" cycles="<<plan.guard_cycles<<" steps="<<plan.guard_steps
             <<" pgo="<<PgoModeName(plan.pgo)<<" thinlto="<<(plan.thinlto?1:0)<<"\n";
    if(sampling.enabled)
      std::cout<<"GEKKOAOT_PGO_TRAINER_V18=1 mode=sampled-ir-instrumentation period="
               <<sampling.period<<" burst="<<sampling.burst<<" scale="<<sampling.scale
               <<" counter-write-ratio=1/"<<sampling.scale<<" prior-guidance=separate-use-pass\n";
    std::cout<<"GEKKOAOT_DISPATCH_INDEX_V8=1 lookup=indexed\n";
    if(!static_intercepts.empty())
      std::cout<<"GEKKOAOT_STATIC_INTERCEPT_AOT_V11=1 manifest=\""<<static_intercepts.string()
               <<"\" hash="<<intercept_hash<<" policy=omit-safe-entry-probes\n";
    if(adaptive_hash!="none")
      std::cout<<"GEKKOAOT_ADAPTIVE_AOT_V21=1 manifest=\""<<plan.adaptive_hot_manifest.string()
               <<"\" hash="<<adaptive_hash
               <<" policy=profile-selected-hot-functions+full-state-ssa+branchless-reservation+single-ir-pgo-o3\n";
  } else {
    gen="cmake -E env "+QuoteText("DOLRECOMP_DISPATCH_LOOKUP=indexed");
    if(!static_intercepts.empty())
      gen += " " + QuoteText(std::string("DOLRECOMP_STATIC_INTERCEPTS=")+static_intercepts.string());
    gen += " "+Quote(dolrecomp)+" -j"+std::to_string(Jobs())+" --backend="+
        std::string(backend)+" --cpu gekko --gamecube";
  }
  gen += " "+Quote(p.disc/"sys/main.dol")+" "+Quote(output);
  Require(Run(gen),llvm_backend?"DolRecomp LLVM AOT":"DolRecomp Portable C AOT");
  const auto generated=FindGenerated(output);
  fs::copy_file(p.disc/"sys/main.dol",generated/"main.dol",fs::copy_options::overwrite_existing);
  fs::path smc=generated/"generated_smc.txt";
  if(!fs::exists(smc)) { for(const auto& e:fs::directory_iterator(generated)) if(e.is_regular_file()&&e.path().filename().string().ends_with("_smc.txt")){fs::copy_file(e.path(),smc,fs::copy_options::overwrite_existing);break;} }
  if(!fs::exists(smc))WriteText(smc,"");
  const auto tables=artifact/"module_tables.inc";
  Require(Run(Quote(p.meta_tool)+" "+Quote(generated/"generated.h")+" "+Quote(smc)+" "+Quote(p.disc/"sys/main.dol")+" "+Quote(tables)),"module metadata");
  const auto mb=artifact/"module-build"; std::error_code ec; fs::remove_all(mb,ec);
  std::string cfg="cmake -S "+Quote(p.root/"runtime/module")+" -B "+Quote(mb)+" -G Ninja -DCMAKE_BUILD_TYPE=Release -DGAME_ID="+QuoteText(p.game_id)+" -DGENERATED_DIR="+Quote(generated)+" -DDOLRECOMP_DIR="+Quote(p.dolrecomp_src)+" -DGEKKOAOT_ROOT="+Quote(p.root)+" -DMODULE_TABLES="+Quote(tables);
  const auto portable_toolchain=p.root/"toolchain/gekkoaot-zig.cmake";
  if(BundledToolchainReady(p.root) && fs::exists(portable_toolchain))
    cfg += " -DCMAKE_TOOLCHAIN_FILE="+Quote(portable_toolchain);
  cfg += " -DGEKKOAOT_PGO_MODE="+QuoteText(PgoModeName(plan.pgo));
  cfg += " -DGEKKOAOT_USE_THINLTO="+std::string(plan.thinlto?"ON":"OFF");
  if(plan.pgo==PgoMode::Use) cfg += " -DGEKKOAOT_PGO_PROFILE="+Quote(plan.profile_path);
  if(plan.pgo!=PgoMode::Off) {
    const auto clang=Env("GEKKOAOT_CLANG","clang");
    cfg += " -DCMAKE_C_COMPILER="+QuoteText(clang);
  }
  Require(Run(cfg),"game module configure"); Require(Run("cmake --build "+Quote(mb)+" -j"+std::to_string(Jobs())),"game module link");
  fs::path built=mb/("g"+p.game_id+"_recomp"+SharedSuffix()); if(!fs::exists(built)){auto x=FindFileRecursive(mb,built.filename().string());if(!x.empty())built=x;}
  if (!fs::exists(built)) throw std::runtime_error("linked game module not found");
  fs::copy_file(built,module,fs::copy_options::overwrite_existing);
  std::cout<<"GEKKOAOT_PROGRESS=98|Native module ready\n"; return module;
}

fs::path EnsureSdkManifest(Pipeline& p,const fs::path&,std::string_view) {
  // The SDK manifest is generated by structural analysis of this exact DOL and
  // by the resolver compiled into gekkoaot-native-run.  v10 keyed only the
  // policy, which meant an old 5-hook manifest could survive resolver fixes
  // forever.  Include the DOL plus an explicit resolver ABI in the cache key;
  // bump the ABI whenever admission/matching semantics change.
  constexpr std::string_view kSdkResolverAbi = "22";
  const auto dol_hash = FileHash64(p.disc/"sys/main.dol");
  const std::string policy = std::string("auto=")+Env("GEKKOAOT_NATIVE_SDK_AUTO","1")+
      ";os="+Env("GEKKOAOT_NATIVE_OS","1")+
      ";admission="+Env("GEKKOAOT_NATIVE_OS_ADMISSION","hot")+
      ";resolver="+std::string(kSdkResolverAbi)+
      ";dol="+dol_hash;
  const auto policy_hash=TextHash64(policy);
  p.sdk_manifest=p.cache/"sdk"/p.game_id/("native-intercepts-v21-"+policy_hash+".txt");
  if(fs::exists(p.sdk_manifest)) {
    std::cout<<"GEKKOAOT_SDK_COMPILETIME_V21=1 cache=hit manifest=\""<<p.sdk_manifest.string()
             <<"\" policy-hash="<<policy_hash<<" dol-hash="<<dol_hash
             <<" resolver-abi="<<kSdkResolverAbi<<"\n";
    return p.sdk_manifest;
  }
  fs::create_directories(p.sdk_manifest.parent_path());
  if(Env("GEKKOAOT_NATIVE_SDK_AUTO","1")=="0") {
    WriteText(p.sdk_manifest,"# GEKKOAOT_NATIVE_SDK_MANIFEST_V2\n# native SDK auto-resolution disabled\n");
    std::cout<<"GEKKOAOT_SDK_COMPILETIME_V21=1 cache=miss hooks=0 reason=auto-disabled manifest=\""
             <<p.sdk_manifest.string()<<"\"\n";
    return p.sdk_manifest;
  }
  std::cout<<"GEKKOAOT_STAGE=sdk-analysis\nGEKKOAOT_PROGRESS=34|Resolving native SDK/HLE entrypoints\n";
  // Pure DOL analysis: no renderer, no disc emulation, no AOT bootstrap module.
  // The same structural resolver used by the runtime scans the original PPC
  // bytes once at build time and emits the exact immutable hook set consumed by
  // both the optimized compiler pass and the runtime.
  std::ostringstream scan;
  scan<<Quote(p.native_run)
      <<" --dol "<<Quote(p.disc/"sys/main.dol")
      <<" --emit-sdk-manifest "<<Quote(p.sdk_manifest);
  Require(Run(scan.str()),"build-time SDK/HLE analysis");
  if(!fs::exists(p.sdk_manifest)) throw std::runtime_error("SDK manifest was not produced");
  std::cout<<"GEKKOAOT_SDK_COMPILETIME_V21=1 cache=miss manifest=\""<<p.sdk_manifest.string()
           <<"\" hash="<<FileHash64(p.sdk_manifest)<<" policy-hash="<<policy_hash<<"\n";
  return p.sdk_manifest;
}

fs::path PgoDirectory(const Pipeline& p) {
  // LLVM instrumentation identities are tied to the generated CFG. Keep old
  // profiles available, but isolate profiles produced by this hot-path ABI so
  // stale counters cannot be silently reused after compiler CFG changes.
  const auto compat=Env("GEKKOAOT_PGO_COMPAT_ID","perf25-context-capture-v76");
  return p.cache/"pgo"/p.game_id/compat;
}
fs::path PgoMergedProfile(const Pipeline& p) { return PgoDirectory(p)/"merged.profdata"; }
fs::path PgoSamplingMarker(const Pipeline& p) { return PgoDirectory(p)/"raw"/".sampling-v18"; }
fs::path PgoAdaptiveManifest(const Pipeline& p) { return PgoDirectory(p)/"adaptive-hot-v21.txt"; }
fs::path AdaptivePgoTool(const Pipeline& p) { return p.root/"tools"/"adaptive_pgo.py"; }

fs::path BuildAdaptivePgoManifest(const Pipeline& p,const fs::path& profile,const fs::path& profdata) {
  const auto output=PgoAdaptiveManifest(p);
  if(Env("GEKKOAOT_ADAPTIVE_PGO","1")=="0") {
    std::error_code ec; fs::remove(output,ec);
    std::cout<<"GEKKOAOT_ADAPTIVE_PGO_V21=0 reason=disabled\n";
    return {};
  }
  const auto tool=AdaptivePgoTool(p);
  if(!fs::exists(tool) || !fs::exists(profile)) {
    std::cerr<<"[gekkoaot] warning: adaptive PGO manifest unavailable"
             <<" tool="<<(fs::exists(tool)?1:0)<<" profile="<<(fs::exists(profile)?1:0)<<"\n";
    return {};
  }
  const unsigned max_functions=EnvUnsigned("GEKKOAOT_ADAPTIVE_PGO_MAX_FUNCTIONS",32u,1u,256u);
  const unsigned divisor=EnvUnsigned("GEKKOAOT_ADAPTIVE_PGO_RELATIVE_DIVISOR",64u,1u,4096u);
  std::ostringstream cmd;
  #ifdef _WIN32
  const auto python=Env("GEKKOAOT_PYTHON","python");
#else
  const auto python=Env("GEKKOAOT_PYTHON","python3");
#endif
  cmd<<QuoteText(python)<<' '<<Quote(tool)
     <<" --llvm-profdata "<<Quote(profdata)
     <<" --profile "<<Quote(profile)
     <<" --output "<<Quote(output)
     <<" --max-functions "<<max_functions
     <<" --relative-divisor "<<divisor;
  const int rc=Run(cmd.str());
  if(rc!=0 || !fs::exists(output)) {
    std::cerr<<"[gekkoaot] warning: adaptive PGO selector failed (exit "<<rc<<")\n";
    return {};
  }
  return output;
}

void WritePgoSamplingMarker(const Pipeline& p,const PgoSamplingConfig& cfg) {
  const auto marker=PgoSamplingMarker(p);
  std::error_code ec;
  if(!cfg.enabled) {
    fs::remove(marker,ec);
    return;
  }
  std::ostringstream text;
  text<<"period="<<cfg.period<<"\n"
      <<"burst="<<cfg.burst<<"\n"
      <<"scale="<<cfg.scale<<"\n";
  WriteText(marker,text.str());
}

std::optional<PgoSamplingConfig> ReadPgoSamplingMarker(const Pipeline& p) {
  const auto marker=PgoSamplingMarker(p);
  if(!fs::exists(marker)) return std::nullopt;
  PgoSamplingConfig cfg;
  cfg.enabled=true;
  cfg.period=0u; cfg.burst=0u; cfg.scale=0u;
  std::ifstream in(marker);
  std::string line;
  while(std::getline(in,line)) {
    const auto pos=line.find('=');
    if(pos==std::string::npos) continue;
    const auto key=line.substr(0,pos);
    try {
      const auto value=static_cast<unsigned>(std::stoul(line.substr(pos+1)));
      if(key=="period") cfg.period=value;
      else if(key=="burst") cfg.burst=value;
      else if(key=="scale") cfg.scale=value;
    } catch(...) {}
  }
  if(cfg.period==0u || cfg.burst==0u || cfg.scale==0u || cfg.burst>cfg.period)
    throw std::runtime_error("invalid PGO sampling marker: "+marker.string());
  return cfg;
}

fs::path CrossGameLocalDb(const Pipeline& p) {
  return p.state/"crossgame"/"local-v1.json";
}
fs::path CrossGameCommunityDb(const Pipeline& p) {
  return p.root/"crossgame-db"/"community-v1.json";
}
fs::path CrossGameTool(const Pipeline& p) {
  return p.root/"tools"/"crossgame_db.py";
}
std::string CrossGamePython() {
#ifdef _WIN32
  return Env("GEKKOAOT_PYTHON","python");
#else
  return Env("GEKKOAOT_PYTHON","python3");
#endif
}

void LearnCrossGameProfile(Pipeline& p,const fs::path& session_profile,
                           const fs::path& merged_profile,const fs::path& profdata,
                           bool recovered) {
  if(Env("GEKKOAOT_CROSSGAME_ENABLE","1")=="0") {
    std::cout<<"GEKKOAOT_CROSSGAME_TRACK_V17=0 reason=disabled\n";
    return;
  }

  const auto tool=CrossGameTool(p);
  const auto community=CrossGameCommunityDb(p);
  const auto local=CrossGameLocalDb(p);
  const auto dol=p.disc/"sys/main.dol";
  if(!fs::exists(tool) || !fs::exists(community) || !fs::exists(dol)) {
    std::cerr<<"[gekkoaot] warning: CrossGameDB tracking unavailable"
             <<" tool="<<(fs::exists(tool)?1:0)
             <<" community="<<(fs::exists(community)?1:0)
             <<" dol="<<(fs::exists(dol)?1:0)<<"\n";
    std::cout<<"GEKKOAOT_CROSSGAME_TRACK_V17=0 reason=missing-input\n";
    return;
  }

  fs::create_directories(local.parent_path());
  const auto python=CrossGamePython();
  const unsigned signature_chunk=EnvUnsigned("GEKKOAOT_CROSSGAME_SIGNATURE_CHUNK",512u,32u,4096u);
  std::ostringstream learn;
  learn<<QuoteText(python)<<' '<<Quote(tool)
       <<" learn --dol "<<Quote(dol)
       <<" --profile "<<Quote(session_profile)
       <<" --llvm-profdata "<<Quote(profdata)
       <<" --local "<<Quote(local)
       <<" --disc-id "<<QuoteText(p.game_id)
       <<" --chunk "<<signature_chunk;
  const int learn_rc=Run(learn.str());
  if(learn_rc!=0 || !fs::exists(local)) {
    std::cerr<<"[gekkoaot] warning: CrossGameDB learning failed (exit "<<learn_rc
             <<"); PGO profile remains valid\n";
    std::cout<<"GEKKOAOT_CROSSGAME_TRACK_V17=0 reason=learn-failed exit="<<learn_rc<<"\n";
    return;
  }

  std::cout<<"GEKKOAOT_CROSSGAME_LOCAL_UPDATED_V17=1 path=\""<<local.string()
           <<"\" hash="<<FileHash64(local)
           <<" source=session-profdata session="<<FileHash64(session_profile)
           <<" merged="<<FileHash64(merged_profile)
           <<" recovered="<<(recovered?1:0)<<"\n";

  if(Env("GEKKOAOT_CROSSGAME_AUTO_EXPORT","1")=="0") {
    std::cout<<"GEKKOAOT_CROSSGAME_COMMUNITY_UPDATED_V17=0 reason=auto-export-disabled\n";
    return;
  }

  const auto tmp=community.parent_path()/"community-v1.new.json";
  std::error_code ec;
  fs::remove(tmp,ec);
  std::ostringstream merge;
  merge<<QuoteText(python)<<' '<<Quote(tool)
       <<" merge --base "<<Quote(community)
       <<" --local "<<Quote(local)
       <<" --output "<<Quote(tmp);
  const int merge_rc=Run(merge.str());
  if(merge_rc!=0 || !fs::exists(tmp)) {
    fs::remove(tmp,ec);
    std::cerr<<"[gekkoaot] warning: CrossGameDB community export failed (exit "
             <<merge_rc<<"); local tracking was kept\n";
    std::cout<<"GEKKOAOT_CROSSGAME_COMMUNITY_UPDATED_V17=0 reason=merge-failed exit="
             <<merge_rc<<"\n";
    return;
  }

  ec.clear();
  fs::rename(tmp,community,ec);
  if(ec) {
    ec.clear();
    fs::copy_file(tmp,community,fs::copy_options::overwrite_existing,ec);
    std::error_code remove_ec; fs::remove(tmp,remove_ec);
  }
  if(ec || !fs::exists(community)) {
    std::cerr<<"[gekkoaot] warning: cannot install updated community CrossGameDB: "
             <<ec.message()<<"\n";
    std::cout<<"GEKKOAOT_CROSSGAME_COMMUNITY_UPDATED_V17=0 reason=install-failed\n";
    return;
  }

  (void)Run(QuoteText(python)+" "+Quote(tool)+" stats --db "+Quote(community));
  std::cout<<"GEKKOAOT_CROSSGAME_COMMUNITY_UPDATED_V17=1 path=\""<<community.string()
           <<"\" hash="<<FileHash64(community)
           <<" local-hash="<<FileHash64(local)<<"\n";
}

fs::path CompileModule(Pipeline& p,const fs::path& dolrecomp,std::string_view backend,bool force=false) {
  const auto explicit_hooks=Env("GEKKOAOT_RUN_ARGS");
  const bool has_manual_hooks=explicit_hooks.find("--os-hook")!=std::string::npos ||
                              explicit_hooks.find("--hle-hook")!=std::string::npos;
  if(has_manual_hooks) {
    std::cerr<<"GEKKOAOT_STATIC_INTERCEPT_AOT_V10=0 reason=manual-runtime-hooks\n";
    p.sdk_manifest.clear();
    return CompileModuleVariant(p,dolrecomp,backend,"perf11-dynamic-hooks",{},
                                NormalCompilePlan(),force);
  }
  const auto manifest=EnsureSdkManifest(p,dolrecomp,backend);
  const auto profile=PgoMergedProfile(p);
  if(backend=="llvm" && Env("GEKKOAOT_PGO_AUTO_USE","1")!="0" && fs::exists(profile)) {
    std::cout<<"GEKKOAOT_PGO_AUTO_USE_V12=1 profile=\""<<profile.string()
             <<"\" hash="<<FileHash64(profile)<<"\n";
    auto plan=PgoCompilePlan(PgoMode::Use,profile);
    auto adaptive=PgoAdaptiveManifest(p);
    if(!fs::exists(adaptive))
      adaptive=BuildAdaptivePgoManifest(p,profile,FindLlvmProfdata());
    if(!adaptive.empty() && fs::exists(adaptive))
      plan.adaptive_hot_manifest=adaptive;
    return CompileModuleVariant(p,dolrecomp,backend,"perf21-adaptive-pgo-use",manifest,
                                plan,force);
  }
  return CompileModuleVariant(p,dolrecomp,backend,"perf22-context-capture-v76",manifest,
                              NormalCompilePlan(),force);
}

std::vector<fs::path> BuildSecondaryModules(Pipeline& p,const fs::path& dolrecomp) {
  const auto out=p.cache/"secondary-aot"/p.game_id;
  const auto manifest=out/"modules.txt";
  fs::create_directories(out);
  std::ostringstream cmd;
  cmd<<"cmake -E env "<<QuoteText("DOLRECOMP_DISPATCH_LOOKUP=indexed")<<" "
     <<Quote(p.secondary_tool)
     <<" --root "<<Quote(p.disc)
     <<" --dolrecomp "<<Quote(dolrecomp)
     <<" --dolrecomp-src "<<Quote(p.dolrecomp_src)
     <<" --gekkoaot-root "<<Quote(p.root)
     <<" --output "<<Quote(out)
     <<" --manifest "<<Quote(manifest)
     <<" --game-id "<<QuoteText(p.game_id)
     <<" --jobs "<<Jobs();
  Require(Run(cmd.str()),"secondary executable AOT build");

  std::vector<fs::path> modules;
  std::ifstream in(manifest);
  std::string line;
  while(std::getline(in,line)) {
    if(line.empty()) continue;
    fs::path m=line;
    if(!fs::exists(m)) throw std::runtime_error("secondary module listed but missing: "+m.string());
    modules.push_back(fs::absolute(m));
  }
  std::cout<<"GEKKOAOT_SECONDARY_MODULES="<<modules.size()<<'\n';
  return modules;
}

void ApplyGraphicsEnv() {
  const auto res=Env("GEKKOAOT_RESOLUTION","1280x720"); const auto x=res.find('x');
  if(x!=std::string::npos){
#ifdef _WIN32
    _putenv_s("GEKKOAOT_NATIVE_GX_WIDTH",res.substr(0,x).c_str()); _putenv_s("GEKKOAOT_NATIVE_GX_HEIGHT",res.substr(x+1).c_str());
#else
    setenv("GEKKOAOT_NATIVE_GX_WIDTH",res.substr(0,x).c_str(),1); setenv("GEKKOAOT_NATIVE_GX_HEIGHT",res.substr(x+1).c_str(),1);
#endif
  }
  const auto full=Env("GEKKOAOT_FULLSCREEN","0");
#ifdef _WIN32
  _putenv_s("GEKKOAOT_NATIVE_GX_FULLSCREEN",full.c_str());
#else
  setenv("GEKKOAOT_NATIVE_GX_FULLSCREEN",full.c_str(),1);
#endif

  // No historical GXRuntime shadow/oracle is enabled. GekkoAOT owns command
  // boundaries and CP state; Aurora only receives already-framed retail GX.
#ifdef _WIN32
  _putenv_s("GEKKOAOT_NATIVE_GX_FIFO_TEXTURE_CACHE","1");
#else
  setenv("GEKKOAOT_NATIVE_GX_FIFO_TEXTURE_CACHE","0",1);
#endif
}
int RunGame(Pipeline& p,const fs::path& image,const fs::path& module,const fs::path& bridge,
            const std::vector<fs::path>& secondary_modules,const fs::path& stop_file={}) {
  ApplyGraphicsEnv();
  std::ostringstream cmd;
  // Compatibility tooling can wrap only the native-run child without putting
  // the controller/build pipeline itself under GDB or perf. Example values:
  //   gdb -q -ex run --args
  //   perf record --call-graph dwarf --
  const auto run_prefix=Env("GEKKOAOT_RUN_PREFIX");
  if(!run_prefix.empty()) cmd<<run_prefix<<' ';
  cmd<<Quote(p.native_run)<<" --module "<<Quote(module)<<" --dol "<<Quote(p.disc/"sys/main.dol")<<" --game-id "<<QuoteText(p.game_id)<<" --disc-image "<<Quote(image)<<" --native-gx-bridge "<<Quote(bridge);
  if(!p.sdk_manifest.empty()) cmd<<" --sdk-manifest "<<Quote(p.sdk_manifest);
  if(fs::exists(p.disc/"sys/boot.bin"))cmd<<" --boot-bin "<<Quote(p.disc/"sys/boot.bin");
  if(fs::exists(p.disc/"sys/fst.bin"))cmd<<" --fst-bin "<<Quote(p.disc/"sys/fst.bin");
  if(fs::exists(p.disc/"sys/bi2.bin"))cmd<<" --bi2-bin "<<Quote(p.disc/"sys/bi2.bin");
  for(const auto& secondary:secondary_modules) cmd<<" --secondary-module "<<Quote(secondary);
  if(!stop_file.empty()) cmd<<" --stop-file "<<Quote(stop_file);
  const auto extra=Env("GEKKOAOT_RUN_ARGS"); if(!extra.empty())cmd<<' '<<extra;
  std::cout<<"GEKKOAOT_STAGE=runtime\nGEKKOAOT_PROGRESS=100|Game running\n"<<std::flush;
  return Run(cmd.str());
}

int FinalizePgoProfiles(Pipeline& p,const fs::path& dolrecomp,
                        const std::vector<fs::path>& raw_profiles,bool recovered,
                        bool build_optimized=true) {
  if(raw_profiles.empty()) throw std::runtime_error("PGO produced no non-empty .profraw files");
  const auto pgo_dir=PgoDirectory(p);
  const auto session_profile=pgo_dir/"session.profdata";
  const auto merged_profile=PgoMergedProfile(p);
  const auto merged_tmp=pgo_dir/"merged.profdata.tmp";
  fs::create_directories(pgo_dir);
  std::error_code ec;
  fs::remove(session_profile,ec); ec.clear(); fs::remove(merged_tmp,ec);

  const auto profdata=FindLlvmProfdata();
  std::cout<<"[3/6] Collecting raw profiles\n"
           <<"GEKKOAOT_PGO_PROFDATA_V12=1 tool=\""<<profdata.string()
           <<"\" raw-count="<<raw_profiles.size()<<" recovered="<<(recovered?1:0)<<"\n";
  std::string merge=Quote(profdata)+" merge -sparse -o "+Quote(session_profile);
  for(const auto& raw:raw_profiles) merge += " "+Quote(raw);
  Require(Run(merge),"PGO raw profile merge");
  if(!fs::exists(session_profile)) throw std::runtime_error("llvm-profdata did not produce session.profdata");

  // Sampled instrumentation keeps the exact same IR-PGO identities but only
  // commits a fraction of counter increments. Rescale this session before it
  // enters the durable A+B+C profile so a sampled C round has the same expected
  // statistical weight as a full-instrumentation C round.
  if(const auto sampled=ReadPgoSamplingMarker(p); sampled && sampled->enabled && sampled->scale>1u) {
    const auto scaled_profile=pgo_dir/"session.scaled.profdata";
    ec.clear(); fs::remove(scaled_profile,ec);
    const std::string scale_cmd=Quote(profdata)+" merge -sparse "+
        QuoteText("-weighted-input="+std::to_string(sampled->scale)+","+session_profile.string())+
        " -o "+Quote(scaled_profile);
    Require(Run(scale_cmd),"PGO sampled-session rescale");
    if(!fs::exists(scaled_profile))
      throw std::runtime_error("llvm-profdata did not produce scaled session profile");
    ec.clear(); fs::rename(scaled_profile,session_profile,ec);
    if(ec) {
      ec.clear(); fs::copy_file(scaled_profile,session_profile,fs::copy_options::overwrite_existing,ec);
      fs::remove(scaled_profile,ec);
    }
    if(ec) throw std::runtime_error("cannot install scaled PGO session profile: "+ec.message());
    std::cout<<"GEKKOAOT_PGO_SAMPLE_SCALE_V18=1 period="<<sampled->period
             <<" burst="<<sampled->burst<<" weight="<<sampled->scale
             <<" policy=expected-count-normalization\n";
  }
  const auto session_hash=FileHash64(session_profile);

  std::cout<<"[4/6] Merging profiles\n"<<std::flush;
  const bool accumulate=Env("GEKKOAOT_PGO_ACCUMULATE","1")!="0" && fs::exists(merged_profile);
  const auto prior_hash=accumulate ? FileHash64(merged_profile) : std::string("none");
  bool merged=false;
  if(accumulate) {
    const unsigned old_weight=EnvUnsigned("GEKKOAOT_PGO_OLD_WEIGHT",1u,1u,32u);
    const unsigned new_weight=EnvUnsigned("GEKKOAOT_PGO_NEW_WEIGHT",1u,1u,64u);
    std::cout<<"GEKKOAOT_PGO_ACCUMULATE_V16=1 old-weight="<<old_weight
             <<" new-weight="<<new_weight
             <<" policy="<<((old_weight==1u && new_weight==1u)?"exact-sum":"weighted")<<"\n";
    const std::string cumulative=Quote(profdata)+" merge -sparse "+
        QuoteText("-weighted-input="+std::to_string(old_weight)+","+merged_profile.string())+" "+
        QuoteText("-weighted-input="+std::to_string(new_weight)+","+session_profile.string())+
        " -o "+Quote(merged_tmp);
    if(Run(cumulative)==0 && fs::exists(merged_tmp)) {
      fs::rename(merged_tmp,merged_profile,ec);
      if(ec) {
        ec.clear(); fs::copy_file(merged_tmp,merged_profile,fs::copy_options::overwrite_existing,ec);
        fs::remove(merged_tmp,ec);
      }
      merged=!ec && fs::exists(merged_profile);
    } else {
      std::cerr<<"[gekkoaot] previous PGO profile is incompatible; starting a fresh cumulative profile\n";
    }
  }
  if(!merged) {
    ec.clear();
    fs::copy_file(session_profile,merged_profile,fs::copy_options::overwrite_existing,ec);
    if(ec) throw std::runtime_error("cannot install merged PGO profile: "+ec.message());
  }

  std::cout<<"[5/6] Validating the merged profile\n"<<std::flush;
  // `--summary-only` is not accepted by llvm-profdata 20 (the LLVM version
  // used by the pinned DolRecomp toolchain on Arch).  A plain `show` still
  // parses the complete instrumentation profile and returns non-zero for a
  // malformed/incompatible file, while remaining compatible with newer LLVM.
  std::cout<<"GEKKOAOT_PGO_VALIDATE_V13=1 mode=portable-show tool=\""
           <<profdata.string()<<"\"\n";
  Require(Run(Quote(profdata)+" show "+Quote(merged_profile)),"PGO profile validation");
  const auto merged_hash=FileHash64(merged_profile);
  std::cout<<"GEKKOAOT_PGO_MERGE_V16=1 prior="<<prior_hash
           <<" session="<<session_hash<<" result="<<merged_hash
           <<" raw-count="<<raw_profiles.size()
           <<" policy="<<(accumulate?"cumulative":"fresh")<<"\n";
  std::cout<<"GEKKOAOT_PGO_PROFILE_V12=1 path=\""<<merged_profile.string()
           <<"\" hash="<<merged_hash<<" accumulate="<<(accumulate?1:0)
           <<" recovered="<<(recovered?1:0)<<"\n";

  // A validated merged.profdata is now the durable transaction boundary.
  // Consume the raw inputs BEFORE building the optimized module so an
  // interrupted ThinLTO build cannot make the next PGO round merge the same
  // counters a second time.  KEEP_RAW archives them outside raw/ instead of
  // leaving them discoverable by automatic orphan recovery.
  const bool keep_raw=Env("GEKKOAOT_PGO_KEEP_RAW","0")!="0";
  const auto consumed_dir=pgo_dir/"consumed";
  if(keep_raw) fs::create_directories(consumed_dir);
  std::size_t consumed_count=0;
  for(const auto& raw:raw_profiles) {
    if(!fs::exists(raw)) continue;
    if(keep_raw) {
      auto archived=consumed_dir/raw.filename();
      if(fs::exists(archived))
        archived += "."+FileHash64(raw);
      ec.clear();
      fs::rename(raw,archived,ec);
      if(ec) {
        ec.clear();
        fs::copy_file(raw,archived,fs::copy_options::overwrite_existing,ec);
        if(ec) throw std::runtime_error("cannot archive consumed PGO raw profile: "+ec.message());
        ec.clear(); fs::remove(raw,ec);
        if(ec) throw std::runtime_error("cannot remove archived PGO raw profile: "+ec.message());
      }
    } else {
      ec.clear(); fs::remove(raw,ec);
      if(ec && fs::exists(raw)) {
        auto tombstone=raw; tombstone += ".consumed";
        std::error_code rename_ec;
        fs::rename(raw,tombstone,rename_ec);
        if(rename_ec)
          throw std::runtime_error("cannot consume PGO raw profile: "+raw.string());
      }
    }
    ++consumed_count;
  }
  std::cout<<"GEKKOAOT_PGO_RAW_CONSUMED_V16=1 count="<<consumed_count
           <<" keep="<<(keep_raw?1:0)
           <<" transaction=merged-profile-validated\n"<<std::flush;
  ec.clear(); fs::remove(PgoSamplingMarker(p),ec);

  const auto adaptive_manifest=BuildAdaptivePgoManifest(p,merged_profile,profdata);

  // CrossGameDB learns from this round's session profile, never from the
  // cumulative profile. This keeps A+B+C training additive: A is learned once,
  // B once and C once, instead of relearning A again when A+B is finalized.
  // Learning happens at the same durable transaction boundary as raw-profile
  // consumption, so an interrupted ThinLTO build cannot lose or duplicate it.
  LearnCrossGameProfile(p,session_profile,merged_profile,profdata,recovered);

  // When Train PGO discovers counters left behind by a genuinely interrupted
  // pre-merge round, merge and validate them first but do not stop the user's
  // new training request.  Already-merged raws can no longer reappear here.
  if(!build_optimized) {
    std::cout<<"GEKKOAOT_PGO_MERGED_V16=1 profile=\""<<merged_profile.string()
             <<"\" hash="<<FileHash64(merged_profile)
             <<" recovered="<<(recovered?1:0)
             <<" build-deferred=1 raw-consumed=1\n"<<std::flush;
    return 0;
  }

  std::cout<<"[6/6] Building the adaptive PGO module\n"<<std::flush;
  auto use_plan=PgoCompilePlan(PgoMode::Use,merged_profile);
  if(!adaptive_manifest.empty() && fs::exists(adaptive_manifest))
    use_plan.adaptive_hot_manifest=adaptive_manifest;
  const auto optimized=CompileModuleVariant(p,dolrecomp,"llvm","perf21-adaptive-pgo-use",p.sdk_manifest,
      use_plan,true);

  std::cout<<"GEKKOAOT_PGO_READY_V12=1 module=\""<<optimized.string()
           <<"\" profile=\""<<merged_profile.string()<<"\" auto-use=1 recovered="
           <<(recovered?1:0)<<"\nGEKKOAOT_PROGRESS=100|PGO optimized module ready\n"<<std::flush;
  return 0;
}

int RecoverPgo(Pipeline& p,const fs::path& dolrecomp) {
  (void)EnsureSdkManifest(p,dolrecomp,"llvm");
  const auto raw_profiles=FindProfiles(PgoDirectory(p)/"raw");
  if(raw_profiles.empty())
    throw std::runtime_error("no orphaned non-empty PGO .profraw files were found");
  std::cout<<"GEKKOAOT_PGO_RECOVER_V12=1 raw-count="<<raw_profiles.size()
           <<" action=merge-build\n";
  return FinalizePgoProfiles(p,dolrecomp,raw_profiles,true);
}

int TrainPgo(Pipeline& p,const fs::path& image,const fs::path& dolrecomp,const fs::path& bridge) {
  if(Env("GEKKOAOT_BACKEND","llvm")!="llvm")
    throw std::runtime_error("PGO training requires GEKKOAOT_BACKEND=llvm");
  const auto explicit_hooks=Env("GEKKOAOT_RUN_ARGS");
  if(explicit_hooks.find("--os-hook")!=std::string::npos ||
     explicit_hooks.find("--hle-hook")!=std::string::npos)
    throw std::runtime_error("PGO with manual runtime hooks is disabled; use the universal SDK manifest path");

  const auto manifest=EnsureSdkManifest(p,dolrecomp,"llvm");
  (void)manifest;
  const auto pgo_dir=PgoDirectory(p);
  const auto raw_dir=pgo_dir/"raw";
  fs::create_directories(raw_dir);

  // Never destroy flushed counters from a previous interrupted shutdown.
  // Recover them into the cumulative profile, then CONTINUE into a new
  // instrumented round. Train PGO must always mean "launch a training run";
  // recovery is housekeeping, not a terminal action.
  const auto orphan_profiles=FindProfiles(raw_dir);
  if(!orphan_profiles.empty() && Env("GEKKOAOT_PGO_AUTO_RECOVER","1")!="0") {
    std::cout<<"[0/6] Recovering orphaned PGO counters from an interrupted shutdown\n"
             <<"GEKKOAOT_PGO_RECOVER_V15=1 raw-count="<<orphan_profiles.size()
             <<" action=merge-then-train\n";
    FinalizePgoProfiles(p,dolrecomp,orphan_profiles,true,false);
  }

  const auto prior_profile=PgoMergedProfile(p);
  const auto sampling=PgoSamplingFromEnv();
  WritePgoSamplingMarker(p,sampling);
  std::cout<<"GEKKOAOT_PGO_ITERATIVE_V18=1 prior-profile="
           <<(fs::exists(prior_profile)?1:0)
           <<" policy="<<(sampling.enabled?"sampled-instrumentation":"full-instrumentation")
           <<"+exact-cumulative-sum+consume-after-merge+rebuild-each-round";
  if(sampling.enabled)
    std::cout<<" period="<<sampling.period<<" burst="<<sampling.burst
             <<" scale="<<sampling.scale;
  std::cout<<"\n";

  const auto raw_template=raw_dir/"gekkoaot-%m-%p.profraw";
  const auto stop_root=Env("GEKKOAOT_USER_DIR").empty() ? pgo_dir : fs::path(Env("GEKKOAOT_USER_DIR"));
  fs::create_directories(stop_root);
  const auto stop_file=stop_root/"pgo-stop.request";
  auto stop_ack=stop_file;
  stop_ack += ".consumed";
  std::error_code ec;
  fs::remove(stop_file,ec);
  ec.clear();
  fs::remove(stop_ack,ec);

  std::cout<<"[1/6] Building the PGO instrumentation module\n"<<std::flush;
  const auto training=CompileModuleVariant(p,dolrecomp,"llvm","perf18-sampled-pgo-gen",p.sdk_manifest,
      PgoCompilePlan(PgoMode::Generate,raw_template),true);
  const auto secondary_modules=BuildSecondaryModules(p,dolrecomp);

  std::cout<<"[2/6] PGO training has started\n"
             "GEKKOAOT_PGO_V18=1 mode=training stop-file=\""<<stop_file.string()
           <<"\" profile-template=\""<<raw_template.string()<<"\" collector="
           <<(sampling.enabled?"sampled-ir":"full-ir")
           <<" prior-profile-applied=0 reason=llvm-irinstr-and-iruse-are-exclusive\n"<<std::flush;
  const int run_rc=RunGame(p,image,training,bridge,secondary_modules,stop_file);

  // The profile filename contains the runtime PID, so every training round
  // produces new files. If GEKKOAOT_PGO_KEEP_RAW=1 kept recovered counters in
  // place, do not count them a second time in this round.
  const auto all_raw_profiles=FindProfiles(raw_dir);
  std::vector<fs::path> raw_profiles;
  raw_profiles.reserve(all_raw_profiles.size());
  for(const auto& raw:all_raw_profiles) {
    if(std::find(orphan_profiles.begin(),orphan_profiles.end(),raw)==orphan_profiles.end())
      raw_profiles.push_back(raw);
  }
  // native-run atomically renames the request to this acknowledgement when it
  // actually observes a GUI Stop.  This is stronger than !exists(stop_file),
  // which was true even before a Stop was ever requested.
  const bool stop_consumed=fs::exists(stop_ack);
  fs::remove(stop_file,ec);
  ec.clear();
  fs::remove(stop_ack,ec);
  if(run_rc!=0) {
    // Aurora/Dawn teardown has historically failed after a graceful Stop as
    // either SIGSEGV (139) or SIGABRT/free() validation (134).  If native-run
    // positively acknowledged the Stop and compiler-rt left a non-empty raw
    // profile, the training data is valid enough to merge and rebuild.
    const bool renderer_teardown_failure=(run_rc==139 || run_rc==134);
    if(renderer_teardown_failure && stop_consumed && !raw_profiles.empty()) {
      const char* signal_name=run_rc==139 ? "SIGSEGV" : "SIGABRT";
      std::cerr<<"[gekkoaot] warning: renderer teardown exited "<<run_rc
               <<" ("<<signal_name<<") after acknowledged PGO Stop; recovering the flushed profile\n";
      std::cout<<"GEKKOAOT_PGO_SHUTDOWN_SALVAGE_V19=1 exit="<<run_rc
               <<" signal="<<signal_name<<" raw-count="<<raw_profiles.size()
               <<" stop-ack=1 policy=acknowledged-stop-only\n";
    } else {
      throw std::runtime_error("PGO training runtime did not stop cleanly (exit "+std::to_string(run_rc)+")");
    }
  }

  if(raw_profiles.empty())
    throw std::runtime_error("PGO produced no non-empty .profraw files; use Stop in the GUI so the runtime exits normally");
  return FinalizePgoProfiles(p,dolrecomp,raw_profiles,run_rc!=0);
}

}

int main(int argc,char** argv) {
  const std::string action=argc>1?argv[1]:"status";
  try {
    if(action=="status") {
      std::cout<<"GekkoAOT v"<<GEKKOAOT_VERSION
               <<"\npython=not-required\ndolrecomp="<<GEKKOAOT_DOLRECOMP_REV
               <<"\nrenderer=gekkoaot-native-fifo/aurora\n";
      return 0;
    }

    const auto root=Root();
    ConfigureBundledToolchain(root);
    if(action=="clean") {
      std::error_code ec;
      fs::remove_all(root/".gekkoaot/cache",ec);
      std::cout<<"GEKKOAOT_PROGRESS=100|Cache cleaned\n";
      return ec?1:0;
    }

    if(action=="tools") {
      Pipeline p(root/".gekkoaot/toolchain-placeholder.rvz");
      const std::string backend=Env("GEKKOAOT_BACKEND","llvm");
      if(backend!="c"&&backend!="llvm") throw std::runtime_error("GEKKOAOT_BACKEND must be c or llvm");
      std::cout<<"GEKKOAOT_CONTROLLER_VERSION="<<GEKKOAOT_VERSION
               <<"\nGEKKOAOT_BACKEND="<<backend
               <<"\nGEKKOAOT_PROGRESS=2|Preparing native toolchain\n";
      EnsureSources(p);
      (void)BuildDolRecomp(p,backend);
      (void)BuildAuroraBridge(p);
      std::cout<<"GEKKOAOT_PROGRESS=100|Native engine ready\n";
      return 0;
    }

    const auto iso=Env("GEKKOAOT_ISO");
    if(iso.empty()) throw std::runtime_error("GEKKOAOT_ISO is empty");
    const fs::path image=fs::absolute(iso);
    if(!fs::exists(image)) throw std::runtime_error("game source does not exist: "+image.string());

    Pipeline p(image);
    const std::string backend=Env("GEKKOAOT_BACKEND","llvm");
    if(backend!="c"&&backend!="llvm") throw std::runtime_error("GEKKOAOT_BACKEND must be c or llvm");
    std::cout<<"GEKKOAOT_CONTROLLER_VERSION="<<GEKKOAOT_VERSION
             <<"\nGEKKOAOT_BACKEND="<<backend
             <<"\nGEKKOAOT_PROGRESS=2|Preparing native toolchain\n";
    EnsureSources(p);
    const auto dolrecomp=BuildDolRecomp(p,backend);
    const auto bridge=BuildAuroraBridge(p);
    std::cout<<"GEKKOAOT_PROGRESS=20|Native SDK and AuroraGX ready\n";
    ExtractDisc(p,image);

    if(action=="inspect") {
      std::cout<<"Disc ID: "<<p.game_id<<"\nGEKKOAOT_PROGRESS=100|Inspection complete\n";
      return 0;
    }

    if(action=="pgo-recover") {
      if(backend!="llvm") throw std::runtime_error("PGO recovery requires GEKKOAOT_BACKEND=llvm");
      return RecoverPgo(p,dolrecomp);
    }
    if(action=="pgo") return TrainPgo(p,image,dolrecomp,bridge);

    const auto module=CompileModule(p,dolrecomp,backend,false);
    const auto secondary_modules=BuildSecondaryModules(p,dolrecomp);
    if(action=="compile") return 0;
    if(action=="run") return RunGame(p,image,module,bridge,secondary_modules);
    throw std::runtime_error("unknown action: "+action);
  } catch(const std::exception& e) {
    std::cerr<<"[gekkoaot] error: "<<e.what()<<'\n';
    return 1;
  }
}
