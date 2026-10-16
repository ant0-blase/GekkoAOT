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
#include <iterator>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>
#include "source_timestamps.h"
#include "pipeline_lock.h"
#include "native_hot_profile.h"

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

// v323: the main AOT thread is frontend bound (HPPOA: 51% of slots, ~25 L1i
// misses per 1000 instructions). LLVM's hot/cold splitting outlines the
// branch-weighted slow paths (external memory services, state spills, FP
// fixups) into a separate cold section so the hot code stays dense.
// GEKKOAOT_LLVM_HOT_COLD_SPLIT=1 enables it; DolRecomp forwards
// DOLRECOMP_LLVM_OPTIONS to LLVM's command-line parser.
std::string HotColdSplitLlvmOptions() {
  std::string options = "-hot-cold-split -enable-cold-section -hotcoldsplit-max-params=" +
                        Env("GEKKOAOT_LLVM_HOT_COLD_SPLIT_PARAMS", "32");
  const std::string extra = Env("DOLRECOMP_LLVM_OPTIONS");
  return extra.empty() ? options : extra + " " + options;
}
bool FPRegionCoalesceEnabled() {
  return Env("GEKKOAOT_FP_REGION_COALESCE", "1") != "0";
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
#ifdef _WIN32
  // Protect a quoted executable and its quoted arguments from cmd /c's
  // first/last-quote stripping. Native helpers use RunProcess instead.
  const auto first=command.find_first_not_of(" \t\r\n");
  const auto shell=first!=std::string::npos && command[first]=='"'
      ? std::string("\"")+command.substr(first)+"\"" : command;
  const int rc=std::system(shell.c_str());
  return rc;
#else
  const int rc=std::system(command.c_str());
  if(rc==-1) return 127;
  if(WIFEXITED(rc)) return WEXITSTATUS(rc);
  if(WIFSIGNALED(rc)) return 128+WTERMSIG(rc);
  return rc;
#endif
}

#ifdef _WIN32
std::wstring QuoteWindowsArgument(std::wstring_view arg) {
  // CreateProcessW receives one mutable command-line string. Quote arguments
  // using the MSVC/CommandLineToArgvW backslash rules so spaces, parentheses,
  // quotes and trailing backslashes survive exactly.
  std::wstring out;
  out.push_back(L'"');
  std::size_t slashes=0;
  for(const wchar_t ch:arg) {
    if(ch==L'\\') {
      ++slashes;
      continue;
    }
    if(ch==L'"') {
      out.append(slashes*2u+1u,L'\\');
      out.push_back(L'"');
      slashes=0;
      continue;
    }
    out.append(slashes,L'\\');
    slashes=0;
    out.push_back(ch);
  }
  out.append(slashes*2u,L'\\');
  out.push_back(L'"');
  return out;
}
#endif

int RunProcess(const fs::path& executable,const std::vector<fs::path>& args) {
#ifdef _WIN32
  // Do not route native helper invocations through std::system()/cmd.exe.
  // cmd.exe reparses quoting and can reject otherwise-valid Windows paths.
  std::wstring command=QuoteWindowsArgument(executable.wstring());
  for(const auto& arg:args) {
    command.push_back(L' ');
    command+=QuoteWindowsArgument(arg.wstring());
  }

  std::cout<<"$ [direct] "<<Quote(executable);
  for(const auto& arg:args) std::cout<<' '<<Quote(arg);
  std::cout<<'\n'<<std::flush;

  std::vector<wchar_t> mutable_command(command.begin(),command.end());
  mutable_command.push_back(L'\0');

  STARTUPINFOW startup{};
  startup.cb=sizeof(startup);
  PROCESS_INFORMATION process{};

  const BOOL created=CreateProcessW(
      executable.c_str(),
      mutable_command.data(),
      nullptr,
      nullptr,
      TRUE,
      0,
      nullptr,
      nullptr,
      &startup,
      &process);
  if(!created) {
    const DWORD error=GetLastError();
    std::cerr<<"[gekkoaot] CreateProcessW failed for "<<executable.string()
             <<" (Win32 error "<<error<<")\n";
    return error ? static_cast<int>(error) : 1;
  }

  const DWORD wait=WaitForSingleObject(process.hProcess,INFINITE);
  DWORD exit_code=1;
  if(wait==WAIT_OBJECT_0) {
    if(!GetExitCodeProcess(process.hProcess,&exit_code)) {
      const DWORD error=GetLastError();
      std::cerr<<"[gekkoaot] GetExitCodeProcess failed (Win32 error "<<error<<")\n";
      exit_code=error ? error : 1;
    }
  } else {
    const DWORD error=GetLastError();
    std::cerr<<"[gekkoaot] WaitForSingleObject failed (Win32 error "<<error<<")\n";
    exit_code=error ? error : 1;
  }

  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  return static_cast<int>(exit_code);
#else
  std::string command=Quote(executable);
  for(const auto& arg:args) command+=' '+Quote(arg);
  return Run(command);
#endif
}

void Require(int rc, std::string_view what) { if(rc!=0) throw std::runtime_error(std::string(what)+" failed (exit "+std::to_string(rc)+")"); }
std::string ReadLine(const fs::path& p) { std::ifstream in(p); std::string s; std::getline(in,s); return s; }
void WriteText(const fs::path& p,std::string_view s) { fs::create_directories(p.parent_path()); std::ofstream o(p,std::ios::trunc); if(!o)throw std::runtime_error("cannot write "+p.string()); o<<s; }
std::string ReadText(const fs::path& p) {
  std::ifstream in(p,std::ios::binary);
  if(!in) throw std::runtime_error("cannot read "+p.string());
  return std::string(std::istreambuf_iterator<char>(in),std::istreambuf_iterator<char>());
}
void ReplaceOnceInFile(const fs::path& p,std::string_view needle,std::string_view replacement,std::string_view tag) {
  // Git for Windows may check third-party sources out with CRLF while all
  // embedded patch anchors in GekkoAOT are authored with LF. Match against a
  // normalized representation so exact source transforms are platform-neutral.
  const auto normalizeLf=[](std::string_view input) {
    std::string out;
    out.reserve(input.size());
    for(std::size_t i=0;i<input.size();++i) {
      if(input[i]=='\r') {
        if(i+1<input.size() && input[i+1]=='\n') continue;
        out.push_back('\n');
      } else {
        out.push_back(input[i]);
      }
    }
    return out;
  };

  auto text=normalizeLf(ReadText(p));
  const auto normalizedNeedle=normalizeLf(needle);
  const auto normalizedReplacement=normalizeLf(replacement);

  const auto first=text.find(normalizedNeedle);
  if(first==std::string::npos) throw std::runtime_error(std::string(tag)+": anchor not found in "+p.string());
  if(text.find(normalizedNeedle,first+normalizedNeedle.size())!=std::string::npos)
    throw std::runtime_error(std::string(tag)+": anchor is not unique in "+p.string());
  text.replace(first,normalizedNeedle.size(),normalizedReplacement);
  WriteText(p,text);
}
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

std::string CacheComponent(std::string_view full_key) {
#ifdef _WIN32
  // Windows portable installs can easily hit the legacy MAX_PATH boundary once
  // the descriptive AOT tuning/intercept key is nested below LOCALAPPDATA.
  // Keep the existing always-compact policy there.
  constexpr std::size_t kReadablePrefix=28;
#else
  // POSIX filesystems such as ext4 commonly cap one path component at NAME_MAX
  // (255 bytes).  The descriptive LLVM/AOT cache key can exceed that as new
  // ABI compatibility tags are appended (v167 reached 261 bytes).  Preserve
  // existing cache paths while they are safely below the component limit, and
  // compact only oversized keys.  The hash covers the complete key, so no
  // compiler/runtime compatibility information is lost.
  constexpr std::size_t kMaxReadableComponent=180;
  if(full_key.size()<=kMaxReadableComponent) return std::string(full_key);
  constexpr std::size_t kReadablePrefix=96;
#endif
  std::string compact(full_key.substr(0,(std::min)(kReadablePrefix,full_key.size())));
  while(!compact.empty() && (compact.back()=='-' || compact.back()=='.'))
    compact.pop_back();
  compact += "-h" + TextHash64(full_key);
  return compact;
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
  const bool hasRepo=fs::exists(dir/".git");
  if(hasRepo && Head(dir)==rev) { std::cout<<"cache hit: "<<dir<<" @ "<<rev.substr(0,8)<<'\n'; return; }
  fs::create_directories(dir.parent_path());
  if(!hasRepo) {
    std::error_code ec; fs::remove_all(dir,ec);
    Require(Run("git clone --filter=blob:none --no-checkout "+QuoteText(url)+" "+Quote(dir)),"git clone");
  } else {
    // Source caches are owned by GekkoAOT and patched in-place after checkout.
    // When the pin changes, discard those generated/local patch changes before
    // checkout; otherwise git correctly refuses to overwrite the dirty tree.
    Require(Run("git -C "+Quote(dir)+" reset --hard"),"git source cache reset before revision switch");
    Require(Run("git -C "+Quote(dir)+" clean -ffd"),"git source cache clean before revision switch");
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
  std::optional<GekkoAOT::Build::SourceTimestamps> aurora_source_times;
  fs::path native_run,native_disc,meta_tool,secondary_tool;
  std::string game_id;
  explicit Pipeline(const fs::path& image):root(Root()),state(DefaultStateRoot(root)),src(state/"src"),build(state/"build"),cache(state/"cache"),disc(cache/"discs"/Slug(image)),dolrecomp_src(src/"DolRecomp"),aurora_src(src/"Aurora"),native_run(Sibling("gekkoaot-native-run")),native_disc(Sibling("gekkoaot-native-disc")),meta_tool(Sibling("gekkoaot-module-meta")),secondary_tool(Sibling("gekkoaot-secondary-build")){}
};

struct ConcurrentToolchainSnapshot {
  fs::path dolrecomp;
  fs::path bridge;
  fs::path dolrecomp_src;
};

ConcurrentToolchainSnapshot StageConcurrentToolchain(Pipeline& p,
                                                     const fs::path& dolrecomp,
                                                     const fs::path& bridge) {
  // The mutable source/build trees are reset and patched on every controller
  // startup.  Copy the finished compiler plus the C helper source consumed by
  // game-module/REL linking into a content-addressed immutable snapshot before
  // dropping pipeline.lock.  Multiple games can then run LLVM AOT concurrently
  // without one process cleaning files beneath another process.
  const auto cpu=p.dolrecomp_src/"src/cpu/cpu.c";
  const auto cpu_h=p.dolrecomp_src/"src/cpu/cpu.h";
  const auto types=p.dolrecomp_src/"src/common/types.h";
  if(!fs::exists(dolrecomp)||!fs::exists(bridge)||!fs::exists(cpu)||
     !fs::exists(cpu_h)||!fs::exists(types))
    throw std::runtime_error("cannot snapshot incomplete native toolchain");
  const auto key=TextHash64(FileHash64(dolrecomp)+FileHash64(bridge)+
                            FileHash64(cpu)+FileHash64(cpu_h)+FileHash64(types));
  const auto base=p.state/"toolchain-snapshots"/CacheComponent("concurrent-v196-"+key);
  const auto source=base/"DolRecomp";
  const auto compiler=base/dolrecomp.filename();
  const auto gx=base/bridge.filename();
  const auto marker=base/"ready.txt";
  if(!fs::exists(marker)||!fs::exists(compiler)||!fs::exists(gx)||
     !fs::exists(source/"src/cpu/cpu.c")) {
    const auto temp=fs::path(base.string()+".tmp");
    std::error_code ec;
    fs::remove_all(temp,ec);
    fs::create_directories(temp/"DolRecomp");
    fs::copy(p.dolrecomp_src/"src",temp/"DolRecomp/src",
             fs::copy_options::recursive|fs::copy_options::overwrite_existing);
    fs::copy_file(dolrecomp,temp/dolrecomp.filename(),fs::copy_options::overwrite_existing);
    fs::copy_file(bridge,temp/bridge.filename(),fs::copy_options::overwrite_existing);
#ifndef _WIN32
    fs::permissions(temp/dolrecomp.filename(),fs::status(dolrecomp).permissions(),
                    fs::perm_options::replace,ec);
#endif
    WriteText(temp/"ready.txt",key+"\n");
    fs::remove_all(base,ec);
    fs::rename(temp,base);
  }
  std::cout<<"GEKKOAOT_PIPELINE_CONCURRENT_V196=1 snapshot=\""<<base.string()
           <<"\" policy=lock-toolchain-prep-only game-aot=parallel\n";
  return {compiler,gx,source};
}

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
    const GekkoAOT::Build::SourceTimestamps source_times(p.dolrecomp_src);
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

    // Runtime helpers such as MTSPR can unwind generated code and resume at
    // the architectural next instruction.  Exception prologues use SPRG0-3
    // heavily, so make those post-SPR boundaries native dispatcher entries.
    const auto spr_reentry_patch =
        p.root / "patches/dolrecomp/gekkoaot-spr-boundary-reentry-v23.patch";
    if (!fs::exists(spr_reentry_patch))
      throw std::runtime_error("missing DolRecomp SPR re-entry patch: "+
                               spr_reentry_patch.string());
    Require(Run("git -C "+Quote(p.dolrecomp_src)+" apply --check "+
                Quote(spr_reentry_patch)),
            "DolRecomp SPR boundary re-entry patch check");
    Require(Run("git -C "+Quote(p.dolrecomp_src)+" apply "+
                Quote(spr_reentry_patch)),
            "DolRecomp SPR boundary re-entry patch");
    std::cout << "GEKKOAOT_DOLRECOMP_SPR_REENTRY_V167=1 "
                 "helper=mtspr continuation=region-leader policy=native-no-interpreter\n";

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

    // v159: a configured runtime chain budget of zero is the strongest host-
    // boundary mode.  DolRecomp emits guards before charging a loop-header
    // block, so a literal zero budget can side-exit forever at the same guest
    // PC (notably SDK SelectThread idle loops) without executing OSEnableInterrupts.
    // Clamp only the generated guard threshold to one guest cycle: zero keeps
    // its no-chaining intent while guaranteeing one block of forward progress.
    const auto zero_budget_progress_patch =
        p.root / "patches/dolrecomp/gekkoaot-zero-budget-progress-v18.patch";
    if (!fs::exists(zero_budget_progress_patch))
      throw std::runtime_error("missing DolRecomp zero-budget progress patch: "+
                               zero_budget_progress_patch.string());
    Require(Run("git -C "+Quote(p.dolrecomp_src)+" apply --check "+
                Quote(zero_budget_progress_patch)),
            "DolRecomp zero-budget progress patch check");
    Require(Run("git -C "+Quote(p.dolrecomp_src)+" apply "+
                Quote(zero_budget_progress_patch)),
            "DolRecomp zero-budget progress patch");
    std::cout << "GEKKOAOT_DOLRECOMP_ZERO_BUDGET_PROGRESS_V18=1 "
                 "zero=one-block-progress host-boundary=retained scheduler-idle=safe\n";

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
    std::cout << "GEKKOAOT_DOLRECOMP_REL_ADDRESS_PROVENANCE_V165=1 "
                 "translate=proven-constant-mem1 dynamic-pointers=identity\n";

    // v171: v165 intentionally stopped translating arbitrary dynamic REL EAs,
    // but indexed table accesses (static canonical base + runtime index) also
    // became invisible to the OSLink bridge. Preserve that specific provenance
    // without reintroducing heap-pointer aliasing.
    const auto rel_indexed_provenance_patch =
        p.root / "patches/dolrecomp/gekkoaot-rel-indexed-provenance-v24.patch";
    if (!fs::exists(rel_indexed_provenance_patch))
      throw std::runtime_error("missing DolRecomp REL indexed-provenance patch: "+
                               rel_indexed_provenance_patch.string());
    Require(Run("git -C "+Quote(p.dolrecomp_src)+" apply --unidiff-zero --check "+
                Quote(rel_indexed_provenance_patch)),
            "DolRecomp REL indexed-provenance patch check");
    Require(Run("git -C "+Quote(p.dolrecomp_src)+" apply --unidiff-zero "+
                Quote(rel_indexed_provenance_patch)),
            "DolRecomp REL indexed-provenance patch");
    std::cout << "GEKKOAOT_DOLRECOMP_REL_INDEXED_PROVENANCE_V171=1 "
                 "translate=static-base+dynamic-index loaded-pointers=identity\n";

    // v195: canonical REL pointers can also escape through guest stores.  A
    // module prolog may publish a relocated data/interface pointer into a
    // main-DOL global; translate the proven stored value before it escapes.
    const auto rel_stored_pointer_patch =
        p.root / "patches/dolrecomp/gekkoaot-rel-stored-pointer-v48.patch";
    if (!fs::exists(rel_stored_pointer_patch))
      throw std::runtime_error("missing DolRecomp REL stored-pointer patch: " +
                               rel_stored_pointer_patch.string());
    Require(Run("git -C " + Quote(p.dolrecomp_src) +
                " apply --recount --check " + Quote(rel_stored_pointer_patch)),
            "DolRecomp REL stored-pointer patch check");
    Require(Run("git -C " + Quote(p.dolrecomp_src) +
                " apply --recount " + Quote(rel_stored_pointer_patch)),
            "DolRecomp REL stored-pointer patch");
    std::cout << "GEKKOAOT_DOLRECOMP_REL_STORED_POINTER_V199=1 "
                 "candidate=known-mem1+relocatable guard=own-rel-data+cross-module "
                 "store=canonical-to-oslink-runtime v195-broad-rewrite=off\n";

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

    const auto crossgame_prior_patch =
        p.root / "patches/dolrecomp/gekkoaot-crossgame-prior-v193.patch";
    Require(Run("git -C "+Quote(p.dolrecomp_src)+" apply --unidiff-zero --check "+
                Quote(crossgame_prior_patch)), "DolRecomp CrossGameDB prior patch check");
    Require(Run("git -C "+Quote(p.dolrecomp_src)+" apply --unidiff-zero "+
                Quote(crossgame_prior_patch)), "DolRecomp CrossGameDB prior patch");

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

    // v157 / v22: RELs are relocated at runtime and can be entered through
    // pointers or jump-table slots that are invisible to static entry-point
    // discovery.  Keep every REL guest instruction natively re-enterable while
    // leaving the main DOL's coalesced region layout unchanged.
    const auto rel_all_entrypoints_patch =
        p.root / "patches/dolrecomp/gekkoaot-rel-all-entrypoints-v22.patch";
    if (!fs::exists(rel_all_entrypoints_patch))
      throw std::runtime_error("missing DolRecomp REL all-entrypoints patch: "+
                               rel_all_entrypoints_patch.string());
    Require(Run("git -C "+Quote(p.dolrecomp_src)+" apply --check "+
                Quote(rel_all_entrypoints_patch)),
            "DolRecomp REL all-entrypoints patch check");
    Require(Run("git -C "+Quote(p.dolrecomp_src)+" apply "+
                Quote(rel_all_entrypoints_patch)),
            "DolRecomp REL all-entrypoints patch");
    std::cout << "GEKKOAOT_DOLRECOMP_REL_ALL_ENTRYPOINTS_V22=1 "
                 "scope=secondary-rel-only policy=native-no-interpreter\n";

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

    // v129: a computed CTR transfer (bctr/bctrl) has no statically known
    // destination ABI.  Keeping its containing function on the Native ABI
    // path can leave pass-through PPC registers resident only in LLVM SSA,
    // while the global indirect dispatcher starts the dynamic target from
    // CPUState.  Fence just those functions back to the architectural-state
    // path; ordinary BLR/BCLR returns stay Native ABI.
    const auto computed_ctr_abi_fence_patch =
        p.root / "patches/dolrecomp/gekkoaot-computed-ctr-abi-fence-v16.patch";
    if (!fs::exists(computed_ctr_abi_fence_patch))
      throw std::runtime_error("missing DolRecomp computed-CTR ABI fence patch: "+
                               computed_ctr_abi_fence_patch.string());
    Require(Run("git -C "+Quote(p.dolrecomp_src)+" apply --check "+
                Quote(computed_ctr_abi_fence_patch)),
            "DolRecomp computed-CTR ABI fence patch check");
    Require(Run("git -C "+Quote(p.dolrecomp_src)+" apply "+
                Quote(computed_ctr_abi_fence_patch)),
            "DolRecomp computed-CTR ABI fence patch");
    std::cout << "GEKKOAOT_DOLRECOMP_COMPUTED_CTR_ABI_FENCE_V16=1 "
                 "policy=bcctr-architectural-state-boundary\n";

    // v151: keep the retail OSDisable/Enable/RestoreInterrupts bodies in guest
    // AOT, but make both directions of MSR[EE] changes visible to the standalone
    // dispatcher. NativeOS host leaves already created such a dispatch boundary
    // implicitly; without it some titles can leave PI/VI pending indefinitely.
    const auto msr_ee_fence_patch =
        p.root / "patches/dolrecomp/gekkoaot-msr-ee-transition-fence-v17.patch";
    if (!fs::exists(msr_ee_fence_patch))
      throw std::runtime_error("missing DolRecomp MSR[EE] transition fence patch: "+
                               msr_ee_fence_patch.string());
    Require(Run("git -C "+Quote(p.dolrecomp_src)+" apply --check "+
                Quote(msr_ee_fence_patch)),
            "DolRecomp MSR[EE] transition fence patch check");
    Require(Run("git -C "+Quote(p.dolrecomp_src)+" apply "+
                Quote(msr_ee_fence_patch)),
            "DolRecomp MSR[EE] transition fence patch");
    std::cout << "GEKKOAOT_DOLRECOMP_MSR_EE_FENCE_V17=1 "
                 "policy=side-exit-on-ee-transition guest-interrupt-leaves=ppc-aot\n";
    const auto coalescing_patch = p.root / "patches/dolrecomp/gekkoaot-bounded-coalescing-v19.patch";
    Require(Run("git -C " + Quote(p.dolrecomp_src) + " apply --check " + Quote(coalescing_patch)),
            "DolRecomp bounded coalescing patch check");
    Require(Run("git -C " + Quote(p.dolrecomp_src) + " apply " + Quote(coalescing_patch)),
            "DolRecomp bounded coalescing patch");
    const auto progress_patch = p.root / "patches/dolrecomp/gekkoaot-build-progress-v20.patch";
    Require(Run("git -C " + Quote(p.dolrecomp_src) + " apply --check " + Quote(progress_patch)),
            "DolRecomp parallel progress patch check");
    Require(Run("git -C " + Quote(p.dolrecomp_src) + " apply " + Quote(progress_patch)),
            "DolRecomp parallel progress patch");
    const auto slp_patch = p.root / "patches/dolrecomp/gekkoaot-bounded-slp-v21.patch";
    Require(Run("git -C " + Quote(p.dolrecomp_src) + " apply --check " + Quote(slp_patch)),
            "DolRecomp bounded SLP patch check");
    Require(Run("git -C " + Quote(p.dolrecomp_src) + " apply " + Quote(slp_patch)),
            "DolRecomp bounded SLP patch");
    const auto call_chain_patch = p.root / "patches/dolrecomp/gekkoaot-native-call-chain-state-v24.patch";
    Require(Run("git -C " + Quote(p.dolrecomp_src) + " apply --check " + Quote(call_chain_patch)),
            "DolRecomp native call-chain state patch check");
    Require(Run("git -C " + Quote(p.dolrecomp_src) + " apply " + Quote(call_chain_patch)),
            "DolRecomp native call-chain state patch");

    // v207: preserve the architectural PowerPC non-volatile GPR bank across
    // Native ABI chains. Cross-module REL/DOL calls can use r14-r31 as hidden
    // live-ins even when intermediate AOT ranges never reference them.
    const auto cross_module_gpr_patch =
        p.root / "patches/dolrecomp/gekkoaot-cross-module-preserved-gpr-v207.patch";
    if (!fs::exists(cross_module_gpr_patch))
      throw std::runtime_error("missing DolRecomp cross-module GPR patch: " +
                               cross_module_gpr_patch.string());
    Require(Run("git -C " + Quote(p.dolrecomp_src) + " apply --check " +
                Quote(cross_module_gpr_patch)),
            "DolRecomp cross-module preserved GPR patch check");
    Require(Run("git -C " + Quote(p.dolrecomp_src) + " apply " +
                Quote(cross_module_gpr_patch)),
            "DolRecomp cross-module preserved GPR patch");
    std::cout << "GEKKOAOT_DOLRECOMP_CROSS_MODULE_GPR_V207=1 "
                 "abi=r14-r31-native-chain materialize=module-boundary\n";

    // v211: REL->DOL interior/static entrypoints are host-dispatchable ABI
    // boundaries. Import PPC callee-saved GPRs from CPUState at exactly those
    // entrypoints so hidden live-ins survive module transitions.
    const auto rel_to_dol_livein_patch =
        p.root / "patches/dolrecomp/gekkoaot-rel-to-dol-livein-v211.patch";
    if (!fs::exists(rel_to_dol_livein_patch))
      throw std::runtime_error(
          "missing DolRecomp REL->DOL live-in patch: " +
          rel_to_dol_livein_patch.string());

    Require(
        Run("git -C " + Quote(p.dolrecomp_src) +
            " apply --recount --check " +
            Quote(rel_to_dol_livein_patch)),
        "DolRecomp REL->DOL live-in patch check");

    Require(
        Run("git -C " + Quote(p.dolrecomp_src) +
            " apply --recount " +
            Quote(rel_to_dol_livein_patch)),
        "DolRecomp REL->DOL live-in patch");

    std::cout
        << "GEKKOAOT_DOLRECOMP_REL_TO_DOL_LIVEIN_V211=1 "
           "scope=main-dol manifest=v206 preserved-gpr=r14-r31\n";


    // v28b: optimize the common scalar-single path inside the native cpu.c
    // helpers instead of injecting per-instruction LLVM CFG.  The exact helper
    // fallback remains intact for special values/control modes, while generated
    // guest chunks stay the same size and shape as v24.
    const bool scalar_fp_fastpath =
        Env("GEKKOAOT_SCALAR_FP_FASTPATH", "1") != "0";
    if (scalar_fp_fastpath) {
      const auto scalar_fp_patch =
          p.root / "patches/dolrecomp/gekkoaot-scalar-fp-helper-fastpath-v28.patch";
      if (!fs::exists(scalar_fp_patch))
        throw std::runtime_error("missing DolRecomp scalar FP fast-path patch: " +
                                 scalar_fp_patch.string());
      Require(Run("git -C " + Quote(p.dolrecomp_src) + " apply --unidiff-zero --recount --check " +
                  Quote(scalar_fp_patch)),
              "DolRecomp scalar FP fast-path patch check");
      Require(Run("git -C " + Quote(p.dolrecomp_src) + " apply --unidiff-zero --recount " +
                  Quote(scalar_fp_patch)),
              "DolRecomp scalar FP fast-path patch");
      std::cout << "GEKKOAOT_DOLRECOMP_SCALAR_FP_FASTPATH_V28B=1 "
                   "location=native-helper ops=fmuls,scalar-fma "
                   "guard=rn0+ni0+normal-or-zero fallback=exact-helper\n";
    } else {
      std::cout << "GEKKOAOT_DOLRECOMP_SCALAR_FP_FASTPATH_V28B=0 "
                   "fallback=exact-helper\n";
    }

    // v34b: paired-single raw-f32 is a universal GameCube hot path.  Standard
    // GX matrix upload helpers in both SFA and SMB use qr0 and paired stores to
    // WGPIPE.  Collapse contiguous raw-f32 pairs inside the exact helper and
    // batch a WGPIPE pair into one 64-bit host boundary.  Quantized GQR modes
    // and exceptional paths remain byte-for-byte on the old helper path.
    const bool psq_raw_f32_fastpath =
        Env("GEKKOAOT_PSQ_RAW_F32_FASTPATH", "1") != "0";
    if (psq_raw_f32_fastpath) {
      const auto psq_patch =
          p.root / "patches/dolrecomp/gekkoaot-psq-raw-f32-fastpath-v34.patch";
      if (!fs::exists(psq_patch))
        throw std::runtime_error("missing DolRecomp PSQ raw-f32 fast-path patch: " +
                                 psq_patch.string());
      Require(Run("git -C " + Quote(p.dolrecomp_src) +
                  " apply --unidiff-zero --recount --check " + Quote(psq_patch)),
              "DolRecomp PSQ raw-f32 v34b patch check");
      Require(Run("git -C " + Quote(p.dolrecomp_src) +
                  " apply --unidiff-zero --recount " + Quote(psq_patch)),
              "DolRecomp PSQ raw-f32 v34b patch");
      std::cout << "GEKKOAOT_DOLRECOMP_PSQ_RAW_F32_V34B=1 "
                   "scope=universal pair-load=64bit pair-store=64bit wgpipe=one-boundary "
                   "fallback=exact-quantized-helper\n";
    } else {
      std::cout << "GEKKOAOT_DOLRECOMP_PSQ_RAW_F32_V34B=0 "
                   "fallback=exact-helper\n";
    }

    // v40: reintroduce only the measured 32-bit Portable-C memory hot path.
    // Unlike v35-v38, this does not touch 8/16/64-bit helpers, PSQ, paired
    // singles or force-inline policy.  The helpers live in cpu.h after CPUState
    // and common type declarations, so every split C chunk sees the same ABI.
    // MMIO/unmapped accesses fall back to the original mem_read32/mem_write32.
    const bool portable_c_mem32_fast =
        Env("GEKKOAOT_BACKEND", "llvm") == "c" &&
        Env("GEKKOAOT_PORTABLE_C_MEM32_FASTPATH", "1") != "0";
    if (portable_c_mem32_fast) {
      const auto mem32_patch =
          p.root / "patches/dolrecomp/gekkoaot-portable-c-mem32-fastpath-v40.patch";
      if (!fs::exists(mem32_patch))
        throw std::runtime_error("missing DolRecomp Portable-C mem32 v40 patch: " +
                                 mem32_patch.string());
      Require(Run("git -C " + Quote(p.dolrecomp_src) +
                  " apply --unidiff-zero --recount --check " + Quote(mem32_patch)),
              "DolRecomp Portable-C mem32 v40 patch check");
      Require(Run("git -C " + Quote(p.dolrecomp_src) +
                  " apply --unidiff-zero --recount " + Quote(mem32_patch)),
              "DolRecomp Portable-C mem32 v40 patch");
      std::cout << "GEKKOAOT_DOLRECOMP_PORTABLE_C_MEM32_V40=1 "
                   "ops=lwz,lwzx,lfs,stw,stwx,stfs,lwarx,stwcx,lmw,stmw,dcbz "
                   "ram=mem1+mem2 inline=cost-based fallback=exact-mmio "
                   "hardcode=none\n";
    } else {
      std::cout << "GEKKOAOT_DOLRECOMP_PORTABLE_C_MEM32_V40=0 "
                   "base=v34 fallback=exact-memory-abi\n";
    }

    std::cout << "GEKKOAOT_DOLRECOMP_PORTABLE_C_SAFE_V39=1 "
                 "forceinline-v36=off psq-scale-v37=off paired-single-v38=off "
                 "policy=compile-first\n";

    const bool direct_scalar_fp =
        Env("GEKKOAOT_DIRECT_SCALAR_FP", "1") != "0";
    if (direct_scalar_fp) {
      const auto direct_fp_patch =
          p.root / "patches/dolrecomp/gekkoaot-universal-direct-fp-v31.patch";
      if (!fs::exists(direct_fp_patch))
        throw std::runtime_error("missing DolRecomp direct scalar FP patch: " +
                                 direct_fp_patch.string());
      Require(Run("git -C " + Quote(p.dolrecomp_src) +
                  " apply --unidiff-zero --recount --check " + Quote(direct_fp_patch)),
              "DolRecomp universal direct FP v31 patch check");
      Require(Run("git -C " + Quote(p.dolrecomp_src) +
                  " apply --unidiff-zero --recount " + Quote(direct_fp_patch)),
              "DolRecomp universal direct FP v31 patch");
      std::cout << "GEKKOAOT_DOLRECOMP_DIRECT_SCALAR_FP_V186=1 "
                   "scope=universal provenance=exact-f32 "
                   "ops=fadds,fsubs,fmuls,fdivs,frsp,scalar-fma-f32+f64,fcmp "
                   "fallback=v28b-exact-helper\n";
    } else {
      std::cout << "GEKKOAOT_DOLRECOMP_DIRECT_SCALAR_FP_V186=0 "
                   "fallback=v28b-exact-helper\n";
    }

    // v41: detect short, side-effect-free RAM flag polling loops in DolIR and
    // give only those loops a small AOT budget. On expiry the generated module
    // leaves a host-scratch marker so GekkoAOT can park on the next real device
    // deadline instead of burning a core until VI/AI/DSP/DI changes the flag.
    // The classifier is structural (load/compare/backedge), never game/address
    // specific, and fails closed to the ordinary guard for ambiguous loops.
    const auto poll_park_patch =
        p.root / "patches/dolrecomp/gekkoaot-poll-park-v41.patch";
    if (!fs::exists(poll_park_patch))
      throw std::runtime_error("missing DolRecomp poll-park patch: " +
                               poll_park_patch.string());
    Require(Run("git -C " + Quote(p.dolrecomp_src) +
                " apply --recount --check " + Quote(poll_park_patch)),
            "DolRecomp poll-park v41 patch check");
    Require(Run("git -C " + Quote(p.dolrecomp_src) +
                " apply --recount " + Quote(poll_park_patch)),
            "DolRecomp poll-park v41 patch");
    std::cout << "GEKKOAOT_DOLRECOMP_POLL_PARK_V41=1 "
                 "classifier=pure-single-load-ram-poll guard=128 "
                 "marker=external-rid-0xfd hardcode=none\n";

    // v42 closes a false-negative exposed by MOH Frontline without knowing
    // anything about MOH: canonical load/compare/conditional-backedge flag
    // waits are recognized from raw PPC shape before the generic DolIR proof.
    const auto poll_park_v42_patch =
        p.root / "patches/dolrecomp/gekkoaot-poll-park-v42.patch";
    if (!fs::exists(poll_park_v42_patch))
      throw std::runtime_error("missing DolRecomp poll-park v42 patch: " +
                               poll_park_v42_patch.string());
    Require(Run("git -C " + Quote(p.dolrecomp_src) +
                " apply --recount --check " + Quote(poll_park_v42_patch)),
            "DolRecomp poll-park v42 patch check");
    Require(Run("git -C " + Quote(p.dolrecomp_src) +
                " apply --recount " + Quote(poll_park_v42_patch)),
            "DolRecomp poll-park v42 patch");
    std::cout << "GEKKOAOT_DOLRECOMP_POLL_PARK_V42=1 "
                 "classifier=raw-flag-poll+dolir guard=128 marker=external-rid-0xfd "
                 "hardcode=none cache=bumped\n";

    // v43: Portable-C now gets the same structural poll parking as LLVM and
    // uses the already-generated static intercept manifest to avoid crossing
    // CPUState::host_call for ordinary compiled PCs. This is backend-neutral
    // policy, not a game/address list.
    const bool portable_c_backend = Env("GEKKOAOT_BACKEND", "llvm") == "c";
    if (portable_c_backend) {
      const auto c_hot_patch =
          p.root / "patches/dolrecomp/gekkoaot-portable-c-hotpath-v43.patch";
      if (!fs::exists(c_hot_patch))
        throw std::runtime_error("missing DolRecomp Portable-C hotpath v43 patch: " +
                                 c_hot_patch.string());
      Require(Run("git -C " + Quote(p.dolrecomp_src) +
                  " apply --unidiff-zero --recount --check " + Quote(c_hot_patch)),
              "DolRecomp Portable-C hotpath v43 patch check");
      Require(Run("git -C " + Quote(p.dolrecomp_src) +
                  " apply --unidiff-zero --recount " + Quote(c_hot_patch)),
              "DolRecomp Portable-C hotpath v43 patch");
      std::cout << "GEKKOAOT_DOLRECOMP_PORTABLE_C_HOTPATH_V43=1 "
                   "poll=structural-flag-park hostcall=static-positive-only "
                   "guard=128 marker=external-rid-0xfd hardcode=none\n";

      if (portable_c_mem32_fast) {
        const auto c_mem32_inline_patch =
            p.root / "patches/dolrecomp/gekkoaot-portable-c-mem32-inline-v43.patch";
        if (!fs::exists(c_mem32_inline_patch))
          throw std::runtime_error("missing DolRecomp Portable-C mem32 inline v43 patch: " +
                                   c_mem32_inline_patch.string());
        Require(Run("git -C " + Quote(p.dolrecomp_src) +
                    " apply --unidiff-zero --recount --check " +
                    Quote(c_mem32_inline_patch)),
                "DolRecomp Portable-C mem32 inline v43 patch check");
        Require(Run("git -C " + Quote(p.dolrecomp_src) +
                    " apply --unidiff-zero --recount " + Quote(c_mem32_inline_patch)),
                "DolRecomp Portable-C mem32 inline v43 patch");
        std::cout << "GEKKOAOT_DOLRECOMP_PORTABLE_C_MEM32_INLINE_V43=1 "
                     "scope=read32+write32 policy=targeted-always-inline global-forceinline=off\n";
      }
    }

    // v44: the static intercept manifest is authoritative compiler input for
    // modern-runtime LLVM too. Previous v2/v3 logic still forced every modern
    // region through ppc_native_region_available(), defeating compile-time
    // pruning and showing up prominently in perf.
    const bool llvm_backend_now = Env("GEKKOAOT_BACKEND", "llvm") == "llvm";
    if (llvm_backend_now) {
      if (direct_scalar_fp) {
        const auto fprf_lazy_patch =
            p.root / "patches/dolrecomp/gekkoaot-fprf-lazy-v187.patch";
        if (!fs::exists(fprf_lazy_patch))
          throw std::runtime_error("missing DolRecomp FPRF-lazy v187 patch: " +
                                   fprf_lazy_patch.string());
        Require(Run("git -C " + Quote(p.dolrecomp_src) +
                    " apply --unidiff-zero --recount --check " + Quote(fprf_lazy_patch)),
                "DolRecomp FPRF-lazy v187 patch check");
        Require(Run("git -C " + Quote(p.dolrecomp_src) +
                    " apply --unidiff-zero --recount " + Quote(fprf_lazy_patch)),
                "DolRecomp FPRF-lazy v187 patch");
        std::cout << "GEKKOAOT_DOLRECOMP_FPRF_LAZY_V187=1 "
                     "scope=scalar-fma helper=deferred llvm=pending-ssa "
                     "flush=fpscr-read+runtime-boundary fallback=exact\n";
        const auto dynamic_fma_patch =
            p.root / "patches/dolrecomp/gekkoaot-dynamic-scalar-fma-v189.patch";
        if (!fs::exists(dynamic_fma_patch))
          throw std::runtime_error("missing DolRecomp dynamic scalar FMA v189 patch: " +
                                   dynamic_fma_patch.string());
        Require(Run("git -C " + Quote(p.dolrecomp_src) +
                    " apply --check " + Quote(dynamic_fma_patch)),
                "DolRecomp dynamic scalar FMA v189 patch check");
        Require(Run("git -C " + Quote(p.dolrecomp_src) +
                    " apply " + Quote(dynamic_fma_patch)),
                "DolRecomp dynamic scalar FMA v189 patch");
        std::cout << "GEKKOAOT_DOLRECOMP_DYNAMIC_SCALAR_FMA_V189=1 "
                     "scope=universal guard=exact-f32-bits+rn-nearest+ni-nontiny "
                     "ops=fmadds,fmsubs,fnmadds,fnmsubs fallback=exact "
                     "fprf=preserved-at-slow-join hardcode=none\n";
      }

      const auto admission_patch =
          p.root / "patches/dolrecomp/gekkoaot-modern-static-admission-v44.patch";
      if (!fs::exists(admission_patch))
        throw std::runtime_error("missing DolRecomp modern static admission v44 patch: " +
                                 admission_patch.string());
      Require(Run("git -C " + Quote(p.dolrecomp_src) +
                  " apply --recount --check " + Quote(admission_patch)),
              "DolRecomp modern static admission v44 patch check");
      Require(Run("git -C " + Quote(p.dolrecomp_src) +
                  " apply --recount " + Quote(admission_patch)),
              "DolRecomp modern static admission v44 patch");
      std::cout << "GEKKOAOT_DOLRECOMP_STATIC_ADMISSION_V44=1 "
                   "modern-runtime=compile-time-manifest dynamic=fallback-only\n";
    }

    const auto byte_reverse_patch =
        p.root / "patches/dolrecomp/gekkoaot-halfword-byte-reverse-v45.patch";
    if (!fs::exists(byte_reverse_patch))
      throw std::runtime_error("missing DolRecomp halfword byte-reverse v45 patch: " +
                               byte_reverse_patch.string());
    Require(Run("git -C " + Quote(p.dolrecomp_src) +
                " apply --check " + Quote(byte_reverse_patch)),
            "DolRecomp halfword byte-reverse v45 patch check");
    Require(Run("git -C " + Quote(p.dolrecomp_src) +
                " apply " + Quote(byte_reverse_patch)),
            "DolRecomp halfword byte-reverse v45 patch");
    std::cout << "GEKKOAOT_DOLRECOMP_HALFWORD_REVERSE_V45=1 "
                 "ops=lhbrx,sthbrx width=16 upper-bits=ignored\n";

    // Apply the already-proven v46 compiler turbo first.  v47 is deliberately
    // a small incremental scheduler patch on top of v46; keeping these two
    // stages separate makes the patch resilient to the other DolRecomp patches
    // applied above (memory restart/capability, REL provenance, etc.).
    const auto compiler_turbo_v46_patch =
        p.root / "patches/dolrecomp/gekkoaot-compiler-turbo-v46.patch";
    if (!fs::exists(compiler_turbo_v46_patch))
      throw std::runtime_error("missing DolRecomp compiler turbo v46 patch: " +
                               compiler_turbo_v46_patch.string());
    Require(Run("git -C " + Quote(p.dolrecomp_src) +
                " apply --recount --check " + Quote(compiler_turbo_v46_patch)),
            "DolRecomp compiler turbo v46 patch check");
    Require(Run("git -C " + Quote(p.dolrecomp_src) +
                " apply --recount " + Quote(compiler_turbo_v46_patch)),
            "DolRecomp compiler turbo v46 patch");
    std::cout << "GEKKOAOT_DOLRECOMP_COMPILER_TURBO_V46=1 "
                 "worker-batch=env target-machine-cache=off "
                 "fallback-audit=optional thinlto=on-demand ir-level=env\n";

    const auto compiler_turbo_v47_patch =
        p.root / "patches/dolrecomp/gekkoaot-compiler-turbo-v47-adaptive.patch";
    if (!fs::exists(compiler_turbo_v47_patch))
      throw std::runtime_error("missing DolRecomp compiler turbo v47 adaptive patch: " +
                               compiler_turbo_v47_patch.string());
    Require(Run("git -C " + Quote(p.dolrecomp_src) +
                " apply --recount --check " + Quote(compiler_turbo_v47_patch)),
            "DolRecomp compiler turbo v47 adaptive patch check");
    Require(Run("git -C " + Quote(p.dolrecomp_src) +
                " apply --recount " + Quote(compiler_turbo_v47_patch)),
            "DolRecomp compiler turbo v47 adaptive patch");
    std::cout << "GEKKOAOT_DOLRECOMP_COMPILER_TURBO_V47=1 "
                 "base=v46 rel-scheduler=adaptive-global-cpu-budget\n";

    // v215 ordering fix: keep the v212 tlbie hook after all legacy cpu.c
    // zero-context patches.  The scalar-FP v28b and FPRF-lazy v187 patches
    // intentionally target the pinned DolRecomp layout by line number; adding
    // the six-line tlbie hook before them shifted those insertion points into
    // single_fma_value() and produced an invalid nested static function when
    // game modules recompiled cpu.c.  Applying v212 here preserves both sets
    // of changes without weakening either optimization or DTLB semantics.
    const auto dtlb_tlbie_patch =
        p.root / "patches/dolrecomp/gekkoaot-dtlb-tlbie-v212.patch";
    if (!fs::exists(dtlb_tlbie_patch))
      throw std::runtime_error("missing DolRecomp DTLB/tlbie patch: " +
                               dtlb_tlbie_patch.string());
    Require(Run("git -C " + Quote(p.dolrecomp_src) + " apply --check " +
                Quote(dtlb_tlbie_patch)),
            "DolRecomp DTLB/tlbie patch check");
    Require(Run("git -C " + Quote(p.dolrecomp_src) + " apply " +
                Quote(dtlb_tlbie_patch)),
            "DolRecomp DTLB/tlbie patch");
    std::cout << "GEKKOAOT_DOLRECOMP_DTLB_TLBIE_V212=1 "
                 "ordering=v215-after-zero-context-cpu-patches "
                 "callback=cache-control-op4 invalidate=both-ways-per-set\n";

    // v217: some retail DOLs put real PPC routines in nominal data sections.
    // DolRecomp historically admits only the seven DOL text sections, so a
    // perfectly valid direct branch into one of those routines reaches the
    // runtime as an uncompiled address.  Promote only data tails proven
    // executable by a direct B/BC from text; no game IDs or guessed opcodes.
    const auto dol_data_code_patch =
        p.root / "patches/dolrecomp/gekkoaot-dol-data-code-v217.patch";
    if (!fs::exists(dol_data_code_patch))
      throw std::runtime_error("missing DolRecomp executable-data v217 patch: " +
                               dol_data_code_patch.string());
    Require(Run("git -C " + Quote(p.dolrecomp_src) + " apply --check " +
                Quote(dol_data_code_patch)),
            "DolRecomp executable-data v217 patch check");
    Require(Run("git -C " + Quote(p.dolrecomp_src) + " apply " +
                Quote(dol_data_code_patch)),
            "DolRecomp executable-data v217 patch");
    std::cout << "GEKKOAOT_DOLRECOMP_DATA_CODE_V217=1 "
                 "proof=direct-b-or-bc-from-text admission=data-tail-from-first-target "
                 "embedded-data=retained\n";

    const auto rel_cfg_address_patch =
        p.root / "patches/dolrecomp/gekkoaot-rel-cfg-address-v221.patch";
    if (!fs::exists(rel_cfg_address_patch))
      throw std::runtime_error("missing DolRecomp REL CFG address patch: " +
                               rel_cfg_address_patch.string());
    Require(Run("git -C " + Quote(p.dolrecomp_src) + " apply --check " +
                Quote(rel_cfg_address_patch)), "DolRecomp REL CFG address patch check");
    Require(Run("git -C " + Quote(p.dolrecomp_src) + " apply " +
                Quote(rel_cfg_address_patch)), "DolRecomp REL CFG address patch");
    std::cout << "GEKKOAOT_DOLRECOMP_REL_CFG_ADDRESS_V221=1 "
                 "proof=all-predecessors-agree loops=preserve-static-base calls=forget\n";

    const auto rel_pointer_patch =
        p.root / "patches/dolrecomp/gekkoaot-rel-pointer-materialization-v222.patch";
    if (!fs::exists(rel_pointer_patch))
      throw std::runtime_error("missing DolRecomp REL pointer materialization patch: " +
                               rel_pointer_patch.string());
    Require(Run("git -C " + Quote(p.dolrecomp_src) + " apply --check " +
                Quote(rel_pointer_patch)), "DolRecomp REL pointer materialization patch check");
    Require(Run("git -C " + Quote(p.dolrecomp_src) + " apply " +
                Quote(rel_pointer_patch)), "DolRecomp REL pointer materialization patch");
    std::cout << "GEKKOAOT_DOLRECOMP_REL_POINTER_V222=1 "
                 "proof=self-relocation materialize=architectural-gpr memory=relocated-ea\n";

    const auto rel_live_gpr_patch =
        p.root / "patches/dolrecomp/gekkoaot-rel-live-gpr-cap-v224.patch";
    if (!fs::exists(rel_live_gpr_patch))
      throw std::runtime_error("missing DolRecomp REL live GPR capability patch: " +
                               rel_live_gpr_patch.string());
    Require(Run("git -C " + Quote(p.dolrecomp_src) + " apply --check " +
                Quote(rel_live_gpr_patch)), "DolRecomp REL live GPR capability patch check");
    Require(Run("git -C " + Quote(p.dolrecomp_src) + " apply " +
                Quote(rel_live_gpr_patch)), "DolRecomp REL live GPR capability patch");

    const auto dispatch_cache_patch =
        p.root / "patches/dolrecomp/gekkoaot-dispatch-lookup-cache-v227.patch";
    Require(Run("git -C "+Quote(p.dolrecomp_src)+" apply --check "+Quote(dispatch_cache_patch)),
            "DolRecomp dispatch lookup cache patch check");
    Require(Run("git -C "+Quote(p.dolrecomp_src)+" apply "+Quote(dispatch_cache_patch)),
            "DolRecomp dispatch lookup cache patch");
    std::cout << "GEKKOAOT_DOLRECOMP_DISPATCH_CACHE_V277=1 lookup=pure-exact-pc slots=512 ownership=module-static-cpu-thread tls=off host-hooks=preserved\n";

    // v278: LLVM/main and LLVM/secondary builds already carry a complete
    // compile-time SDK interception manifest. Do not call CPUState::host_call
    // for every ordinary AOT PC merely to get a negative answer. Emit an exact
    // positive filter: known SDK entries retain host-call precedence, while all
    // other PCs go straight to native dispatch. Portable-C already gets the
    // identical transform from v43, so do not apply it twice there.
    if (!portable_c_backend) {
      const auto dispatch_hostfilter_patch =
          p.root / "patches/dolrecomp/gekkoaot-dispatch-static-hostcall-v278.patch";
      if (!fs::exists(dispatch_hostfilter_patch))
        throw std::runtime_error("missing DolRecomp static hostcall filter v278 patch: " +
                                 dispatch_hostfilter_patch.string());
      Require(Run("git -C " + Quote(p.dolrecomp_src) +
                  " apply --check " + Quote(dispatch_hostfilter_patch)),
              "DolRecomp static hostcall filter v278 patch check");
      Require(Run("git -C " + Quote(p.dolrecomp_src) +
                  " apply " + Quote(dispatch_hostfilter_patch)),
              "DolRecomp static hostcall filter v278 patch");
      std::cout << "GEKKOAOT_DOLRECOMP_STATIC_HOSTCALL_FILTER_V278=1 "
                   "policy=compile-time-positive-only fallback=runtime-on-native-miss "
                   "safe-chain=unchanged\n";
    }

    const auto fpr_ssa_patch = p.root / "patches/dolrecomp/gekkoaot-fpr-state-ssa-v228.patch";
    Require(Run("git -C "+Quote(p.dolrecomp_src)+" apply --check "+Quote(fpr_ssa_patch)),
            "DolRecomp FPR SSA patch check");
    Require(Run("git -C "+Quote(p.dolrecomp_src)+" apply "+Quote(fpr_ssa_patch)),
            "DolRecomp FPR SSA patch");

    const auto side_exit_patch = p.root / "patches/dolrecomp/gekkoaot-side-exit-analysis-v232.patch";
    Require(Run("git -C "+Quote(p.dolrecomp_src)+" apply --check "+Quote(side_exit_patch)),
            "DolRecomp side-exit analysis patch check");
    Require(Run("git -C "+Quote(p.dolrecomp_src)+" apply "+Quote(side_exit_patch)),
            "DolRecomp side-exit analysis patch");

    const auto clock_patch = p.root / "patches/dolrecomp/gekkoaot-native-sdk-clock-lowering-v238.patch";
    Require(Run("git -C "+Quote(p.dolrecomp_src)+" apply --check "+Quote(clock_patch)),
            "DolRecomp native SDK clock lowering patch check");
    Require(Run("git -C "+Quote(p.dolrecomp_src)+" apply "+Quote(clock_patch)),
            "DolRecomp native SDK clock lowering patch");

    const auto hardware_patch = p.root / "patches/dolrecomp/gekkoaot-native-hardware-lowering-v243.patch";
    Require(Run("git -C "+Quote(p.dolrecomp_src)+" apply --check "+Quote(hardware_patch)),
            "DolRecomp native hardware lowering patch check");
    Require(Run("git -C "+Quote(p.dolrecomp_src)+" apply "+Quote(hardware_patch)),
            "DolRecomp native hardware lowering patch");

    const auto wgpipe_try_patch = p.root / "patches/dolrecomp/gekkoaot-native-wgpipe-try-v244.patch";
    Require(Run("git -C "+Quote(p.dolrecomp_src)+" apply --check "+Quote(wgpipe_try_patch)),
            "DolRecomp native WGPIPE partial write patch check");
    Require(Run("git -C "+Quote(p.dolrecomp_src)+" apply "+Quote(wgpipe_try_patch)),
            "DolRecomp native WGPIPE partial write patch");

    const auto stable_mmio_patch = p.root / "patches/dolrecomp/gekkoaot-stable-mmio-v280.patch";
    if (ReadText(p.dolrecomp_src / "src/backend/llvm/memory_service.cpp").find(
            "GEKKOAOT_STABLE_MMIO_V280") == std::string::npos) {
      Require(Run("git -C " + Quote(p.dolrecomp_src) + " apply --check " + Quote(stable_mmio_patch)),
              "DolRecomp stable MMIO v280 patch check");
      Require(Run("git -C " + Quote(p.dolrecomp_src) + " apply " + Quote(stable_mmio_patch)),
              "DolRecomp stable MMIO v280 patch");
    }

    const auto dynamic_mmio_patch = p.root / "patches/dolrecomp/gekkoaot-dynamic-stable-mmio-v281.patch";
    if (ReadText(p.dolrecomp_src / "src/backend/llvm/memory_service.cpp").find(
            "GEKKOAOT_DYNAMIC_STABLE_MMIO_V281") == std::string::npos) {
      Require(Run("git -C " + Quote(p.dolrecomp_src) + " apply --check " + Quote(dynamic_mmio_patch)),
              "DolRecomp dynamic stable MMIO v281 patch check");
      Require(Run("git -C " + Quote(p.dolrecomp_src) + " apply " + Quote(dynamic_mmio_patch)),
              "DolRecomp dynamic stable MMIO v281 patch");
    }

    const auto residency_patch = p.root / "patches/dolrecomp/gekkoaot-profile-fp-residency-v285.patch";
    if (!fs::exists(residency_patch)) throw std::runtime_error("missing profiled FP residency patch");
    Require(Run("git -C " + Quote(p.dolrecomp_src) + " apply --check " + Quote(residency_patch)),
            "DolRecomp profiled FP residency patch check");
    Require(Run("git -C " + Quote(p.dolrecomp_src) + " apply " + Quote(residency_patch)),
            "DolRecomp profiled FP residency patch");
    const auto address_patch = p.root / "patches/dolrecomp/gekkoaot-native-address-lowering-v287.patch";
    if (!fs::exists(address_patch)) throw std::runtime_error("missing native address lowering patch");
    Require(Run("git -C " + Quote(p.dolrecomp_src) + " apply --check " + Quote(address_patch)),
            "DolRecomp native address lowering patch check");
    Require(Run("git -C " + Quote(p.dolrecomp_src) + " apply " + Quote(address_patch)),
            "DolRecomp native address lowering patch");

    // v274: MSR[EE] transitions no longer force a host boundary. Clearing EE
    // stays in AOT code; setting it asks the runtime (host_call token
    // 0xFFFFFFF4) and exits only when an interrupt is already pending.
    // GEKKOAOT_EE_PENDING_QUERY_CODEGEN=0 keeps the v17 boundary on every
    // transition.
    const auto ee_query_patch =
        p.root / "patches/dolrecomp/gekkoaot-ee-pending-query-v274.patch";
    if (Env("GEKKOAOT_EE_PENDING_QUERY_CODEGEN","1") != "0") {
      if (!fs::exists(ee_query_patch))
        throw std::runtime_error("missing DolRecomp EE pending query patch: "+ee_query_patch.string());
      Require(Run("git -C "+Quote(p.dolrecomp_src)+" apply --check "+Quote(ee_query_patch)),
              "DolRecomp EE pending query patch check");
      Require(Run("git -C "+Quote(p.dolrecomp_src)+" apply "+Quote(ee_query_patch)),
              "DolRecomp EE pending query patch");
      std::cout << "GEKKOAOT_DOLRECOMP_EE_PENDING_QUERY_V274=1 policy=exit-only-when-irq-pending\n";
    }

    // v284: RFI is also an EE-enable scheduling boundary. Both AOT backends
    // share this CPU helper. The runtime query leaves IRQ delivery to the
    // outer dispatcher and only breaks a chain when an event needs service.
    const auto rfi_query_patch =
        p.root / "patches/dolrecomp/gekkoaot-rfi-pending-query-v284.patch";
    if (Env("GEKKOAOT_RFI_PENDING_QUERY_CODEGEN", "1") != "0") {
      if (!fs::exists(rfi_query_patch))
        throw std::runtime_error("missing DolRecomp RFI pending query patch: " + rfi_query_patch.string());
      Require(Run("git -C " + Quote(p.dolrecomp_src) + " apply --check " + Quote(rfi_query_patch)),
              "DolRecomp RFI pending query patch check");
      Require(Run("git -C " + Quote(p.dolrecomp_src) + " apply " + Quote(rfi_query_patch)),
              "DolRecomp RFI pending query patch");
      std::cout << "GEKKOAOT_DOLRECOMP_RFI_PENDING_QUERY_V284=1\n";
    }

    // v274: an MSR[FP]=0 check leaves AOT code instead of rejoining the fast
    // path after ppc_fp_available(), so FP code no longer reloads every guest
    // register from CPUState after each check. GEKKOAOT_FP_COLD_EXIT_CODEGEN=0
    // keeps the previous rejoin.
    const auto fp_cold_patch =
        p.root / "patches/dolrecomp/gekkoaot-fp-available-cold-exit-v274.patch";
    if (Env("GEKKOAOT_FP_COLD_EXIT_CODEGEN","1") != "0") {
      if (!fs::exists(fp_cold_patch))
        throw std::runtime_error("missing DolRecomp FP cold exit patch: "+fp_cold_patch.string());
      Require(Run("git -C "+Quote(p.dolrecomp_src)+" apply --check "+Quote(fp_cold_patch)),
              "DolRecomp FP cold exit patch check");
      Require(Run("git -C "+Quote(p.dolrecomp_src)+" apply "+Quote(fp_cold_patch)),
              "DolRecomp FP cold exit patch");
      std::cout << "GEKKOAOT_DOLRECOMP_FP_COLD_EXIT_V274=1 policy=fp-unavailable-always-exits\n";
    }

    // v292 keeps consecutive paired-single ALU operations in one bounded
    // region. The first lazy-FPU restart and independent entry points remain
    // native. Always register the patch; its codegen switch and fingerprint
    // permit independent on/off builds without changing source preparation.
    const auto fp_region_patch =
        p.root / "patches/dolrecomp/gekkoaot-fp-region-coalesce-v292.patch";
    if (!fs::exists(fp_region_patch))
      throw std::runtime_error("missing DolRecomp FP region patch: " + fp_region_patch.string());
    Require(Run("git -C " + Quote(p.dolrecomp_src) + " apply --check " + Quote(fp_region_patch)),
            "DolRecomp FP region coalescing patch check");
    Require(Run("git -C " + Quote(p.dolrecomp_src) + " apply " + Quote(fp_region_patch)),
            "DolRecomp FP region coalescing patch");
    std::cout << "GEKKOAOT_DOLRECOMP_FP_REGION_COALESCE_V292=1 max-paired-run=32\n";

    const auto flat_vmem_patch = p.root / "patches/dolrecomp/gekkoaot-native-flat-vmem-v304.patch";
    Require(Run("git -C " + Quote(p.dolrecomp_src) + " apply --check " + Quote(flat_vmem_patch)),
            "DolRecomp revocable flat VM patch check");
    Require(Run("git -C " + Quote(p.dolrecomp_src) + " apply " + Quote(flat_vmem_patch)),
            "DolRecomp revocable flat VM patch");
    std::cout << "GEKKOAOT_DOLRECOMP_NATIVE_FLAT_VMEM_V304=1 policy=owner-view+per-access-revocation\n";
    const auto locked_cache_patch = p.root / "patches/dolrecomp/gekkoaot-native-locked-cache-v308.patch";
    Require(Run("git -C " + Quote(p.dolrecomp_src) + " apply --check " + Quote(locked_cache_patch)),
            "DolRecomp direct locked-cache patch check");
    Require(Run("git -C " + Quote(p.dolrecomp_src) + " apply " + Quote(locked_cache_patch)),
            "DolRecomp direct locked-cache patch");
    std::cout << "GEKKOAOT_DOLRECOMP_NATIVE_LOCKED_CACHE_V308=1 policy=owner-storage+shared-sdk-dma\n";

    if (llvm_backend_now && direct_scalar_fp) {
      const auto scalar_math_patch = p.root / "patches/dolrecomp/gekkoaot-native-scalar-math-v316.patch";
      Require(Run("git -C " + Quote(p.dolrecomp_src) + " apply --check " + Quote(scalar_math_patch)),
              "DolRecomp exact native scalar math patch check");
      Require(Run("git -C " + Quote(p.dolrecomp_src) + " apply " + Quote(scalar_math_patch)),
              "DolRecomp exact native scalar math patch");
      std::cout << "GEKKOAOT_DOLRECOMP_NATIVE_SCALAR_MATH_V316=1 policy=finite-exact+architectural-status fallback=original\n";
    }


    // v275: the instruction after an early-return blr is an internal label,
    // not a new LLVM function, when a branch of the same function reaches it
    // (loops/if-else continuing past an early return no longer cross host
    // boundaries). Opt-in (GEKKOAOT_PARTITION_INTERNAL_LABELS=1) until the
    // measured A/B across the game corpus is in.
    const auto partition_patch =
        p.root / "patches/dolrecomp/gekkoaot-partition-internal-labels-v275.patch";
    if (Env("GEKKOAOT_PARTITION_INTERNAL_LABELS","0") == "1") {
      if (!fs::exists(partition_patch))
        throw std::runtime_error("missing DolRecomp partition patch: "+partition_patch.string());
      Require(Run("git -C "+Quote(p.dolrecomp_src)+" apply --check "+Quote(partition_patch)),
              "DolRecomp internal-label partition patch check");
      Require(Run("git -C "+Quote(p.dolrecomp_src)+" apply "+Quote(partition_patch)),
              "DolRecomp internal-label partition patch");
      std::cout << "GEKKOAOT_DOLRECOMP_PARTITION_INTERNAL_LABELS_V275=1 policy=after-blr-split-only-when-unreached-from-region\n";
    }

    // v320: an unknown GQR inlined all five psq quantization types with the
    // complete guest-memory ladder per lane (HPPOA PSMTX leaf: 160 load sites,
    // ~13k phis, 600 KiB). Inline the float type only; quantized psq use the
    // existing exact helper. GEKKOAOT_PSQ_FLOAT_SPECULATION=0 restores it.
    const auto psq_float_patch =
        p.root / "patches/dolrecomp/gekkoaot-psq-float-speculation-v320.patch";
    if (Env("GEKKOAOT_PSQ_FLOAT_SPECULATION","1") != "0") {
      if (!fs::exists(psq_float_patch))
        throw std::runtime_error("missing DolRecomp psq float speculation patch: "+psq_float_patch.string());
      Require(Run("git -C "+Quote(p.dolrecomp_src)+" apply --check "+Quote(psq_float_patch)),
              "DolRecomp psq float speculation patch check");
      Require(Run("git -C "+Quote(p.dolrecomp_src)+" apply "+Quote(psq_float_patch)),
              "DolRecomp psq float speculation patch");
      std::cout << "GEKKOAOT_DOLRECOMP_PSQ_FLOAT_SPECULATION_V320=1 inline=f32 quantized=exact-helper\n";
    }

    // v321: the exact psq helpers ignored a DSI raised by an external lane
    // access (paged VMEM miss) and reported success, so generated code kept
    // running with the exception latch pending and the vector never entered.
    const auto psq_dsi_patch =
        p.root / "patches/dolrecomp/gekkoaot-psq-helper-dsi-v321.patch";
    // Its context is the v34 raw-f32 helper body, which is applied by default.
    if (Env("GEKKOAOT_PSQ_RAW_F32_FASTPATH", "1") != "0") {
      if (!fs::exists(psq_dsi_patch))
        throw std::runtime_error("missing DolRecomp psq helper DSI patch: "+psq_dsi_patch.string());
      Require(Run("git -C "+Quote(p.dolrecomp_src)+" apply --check "+Quote(psq_dsi_patch)),
              "DolRecomp psq helper DSI patch check");
      Require(Run("git -C "+Quote(p.dolrecomp_src)+" apply "+Quote(psq_dsi_patch)),
              "DolRecomp psq helper DSI patch");
      std::cout << "GEKKOAOT_DOLRECOMP_PSQ_HELPER_DSI_V321=1 lane-fault=unwind\n";
    }

    // v322: inline paged-VM accesses through the runtime's exact DTLB mirror
    // (HPPOA/CoD issue ~3M external VM accesses/s). Codegen policy is selected
    // by DOLRECOMP_NATIVE_PAGED_VMEM (GEKKOAOT_NATIVE_PAGED_VMEM=0 disables).
    const auto paged_vmem_patch =
        p.root / "patches/dolrecomp/gekkoaot-native-paged-vmem-v322.patch";
    if (!fs::exists(paged_vmem_patch))
      throw std::runtime_error("missing DolRecomp paged VM patch: "+paged_vmem_patch.string());
    Require(Run("git -C "+Quote(p.dolrecomp_src)+" apply --check "+Quote(paged_vmem_patch)),
            "DolRecomp paged VM patch check");
    Require(Run("git -C "+Quote(p.dolrecomp_src)+" apply "+Quote(paged_vmem_patch)),
            "DolRecomp paged VM patch");
    std::cout << "GEKKOAOT_DOLRECOMP_NATIVE_PAGED_VMEM_V322="
              << (Env("GEKKOAOT_NATIVE_PAGED_VMEM","1") != "0" ? 1 : 0)
              << " source=runtime-dtlb-mirror miss=external-service\n";

    // v325: v320 left quantized psq to the C helper, whose reads only know
    // MEM1 (HPCOS: ~8.6M external locked-cache accesses/s). Resolve the pair
    // window once in generated code and decode every GQR type from it.
    // Built on v320's psq emitters; GEKKOAOT_PSQ_RESOLVED=0 restores v320.
    if (Env("GEKKOAOT_PSQ_FLOAT_SPECULATION","1") != "0") {
      const auto psq_resolved_patch =
          p.root / "patches/dolrecomp/gekkoaot-psq-resolved-v325.patch";
      if (!fs::exists(psq_resolved_patch))
        throw std::runtime_error("missing DolRecomp psq resolved patch: "+psq_resolved_patch.string());
      Require(Run("git -C "+Quote(p.dolrecomp_src)+" apply --check "+Quote(psq_resolved_patch)),
              "DolRecomp psq resolved patch check");
      Require(Run("git -C "+Quote(p.dolrecomp_src)+" apply "+Quote(psq_resolved_patch)),
              "DolRecomp psq resolved patch");
      std::cout << "GEKKOAOT_DOLRECOMP_PSQ_RESOLVED_V325="
                << (Env("GEKKOAOT_PSQ_RESOLVED","1") != "0" ? 1 : 0)
                << " window=8 regions=mem1+locked-cache+flat-vm+paged-vm miss=exact-helper\n";

      // v330: dynamic-GQR psq_st into WGPIPE (CoD vertex stores) left v325's
      // window unresolved and paid the C helper per lane. Offer the packed
      // quantized pair to the native gather once; GEKKOAOT_PSQ_WGPIPE=0 disables.
      const auto psq_wgpipe_patch =
          p.root / "patches/dolrecomp/gekkoaot-psq-wgpipe-v330.patch";
      if (!fs::exists(psq_wgpipe_patch))
        throw std::runtime_error("missing DolRecomp psq WGPIPE patch: "+psq_wgpipe_patch.string());
      Require(Run("git -C "+Quote(p.dolrecomp_src)+" apply --check "+Quote(psq_wgpipe_patch)),
              "DolRecomp psq WGPIPE patch check");
      Require(Run("git -C "+Quote(p.dolrecomp_src)+" apply "+Quote(psq_wgpipe_patch)),
              "DolRecomp psq WGPIPE patch");
      std::cout << "GEKKOAOT_DOLRECOMP_PSQ_WGPIPE_V330="
                << (Env("GEKKOAOT_PSQ_WGPIPE","1") != "0" ? 1 : 0)
                << " pack=quantized-pair try=native-gather miss=exact-lanes\n";
    }

    // v331: every psq_l f32 lane ran the exact integer single->double
    // expansion (~30 host instructions, if-converted). Zero/normal/infinite
    // lanes widen natively; subnormal and NaN lanes call the exact expansion
    // out of line. GEKKOAOT_PSQ_EXTEND_FAST=0 restores the inline expansion.
    const auto psq_extend_patch =
        p.root / "patches/dolrecomp/gekkoaot-psq-extend-fast-v331.patch";
    if (!fs::exists(psq_extend_patch))
      throw std::runtime_error("missing DolRecomp psq extend patch: "+psq_extend_patch.string());
    Require(Run("git -C "+Quote(p.dolrecomp_src)+" apply --check "+Quote(psq_extend_patch)),
            "DolRecomp psq extend patch check");
    Require(Run("git -C "+Quote(p.dolrecomp_src)+" apply "+Quote(psq_extend_patch)),
            "DolRecomp psq extend patch");
    std::cout << "GEKKOAOT_DOLRECOMP_PSQ_EXTEND_FAST_V331="
              << (Env("GEKKOAOT_PSQ_EXTEND_FAST","1") != "0" ? 1 : 0)
              << " common=host-widen exact=subnormal+nan-out-of-line\n";

    // v337: owner services are memory(none) (one call per function after
    // inlining instead of one per inlined callee) and native finite scalar
    // math covers fdivs. Neither changes the CFG seen by PGO instrumentation
    // outside fdivs sites, so trained profiles stay valid.
    const auto pure_patch = p.root / "patches/dolrecomp/gekkoaot-pure-services-fdivs-v337.patch";
    if (!fs::exists(pure_patch))
      throw std::runtime_error("missing DolRecomp pure services patch: "+pure_patch.string());
    Require(Run("git -C "+Quote(p.dolrecomp_src)+" apply --check "+Quote(pure_patch)),
            "DolRecomp pure services patch check");
    Require(Run("git -C "+Quote(p.dolrecomp_src)+" apply "+Quote(pure_patch)),
            "DolRecomp pure services patch");
    std::cout << "GEKKOAOT_DOLRECOMP_PURE_SERVICES_V337=1 services=address+locked-cache+flat-vm fdivs=native-finite\n";

    // v338: dynamic-address stores that hit WGPIPE try the native gather before
    // publishing guest state (CoD ~1M vertex stores/s). Main module only:
    // DOLRECOMP_DYNAMIC_WGPIPE_TRY is passed to the main build alone.
    const auto dynwg_patch = p.root / "patches/dolrecomp/gekkoaot-dynamic-wgpipe-try-v338.patch";
    if (!fs::exists(dynwg_patch))
      throw std::runtime_error("missing DolRecomp dynamic WGPIPE patch: "+dynwg_patch.string());
    Require(Run("git -C "+Quote(p.dolrecomp_src)+" apply --check "+Quote(dynwg_patch)),
            "DolRecomp dynamic WGPIPE patch check");
    Require(Run("git -C "+Quote(p.dolrecomp_src)+" apply "+Quote(dynwg_patch)),
            "DolRecomp dynamic WGPIPE patch");
    std::cout << "GEKKOAOT_DOLRECOMP_DYNAMIC_WGPIPE_TRY_V338="
              << (Env("GEKKOAOT_DYNAMIC_WGPIPE_TRY","1") != "0" ? 1 : 0)
              << " scope=external-store-path try=native-gather miss=exact-publication\n";

    // v339: TBAA classes for CPUState vs guest/host memory accesses, so state
    // values survive guest memory traffic in state-in-memory code.
    // GEKKOAOT_STATE_TBAA=0 disables (DOLRECOMP_STATE_TBAA=0).
    const auto tbaa_patch = p.root / "patches/dolrecomp/gekkoaot-state-tbaa-v339.patch";
    if (!fs::exists(tbaa_patch))
      throw std::runtime_error("missing DolRecomp state TBAA patch: "+tbaa_patch.string());
    Require(Run("git -C "+Quote(p.dolrecomp_src)+" apply --check "+Quote(tbaa_patch)),
            "DolRecomp state TBAA patch check");
    Require(Run("git -C "+Quote(p.dolrecomp_src)+" apply "+Quote(tbaa_patch)),
            "DolRecomp state TBAA patch");
    std::cout << "GEKKOAOT_DOLRECOMP_STATE_TBAA_V339="
              << (Env("GEKKOAOT_STATE_TBAA","1") != "0" ? 1 : 0)
              << " classes=cpu-state+guest-or-host-memory untagged=conservative\n";

    // v340: native finite scalar math under any FPSCR rounding mode and in
    // non-IEEE mode (MXCSR mirrors RN/NI); tiny NI results keep the helper.
    const auto controls_patch = p.root / "patches/dolrecomp/gekkoaot-native-math-any-controls-v340.patch";
    if (!fs::exists(controls_patch))
      throw std::runtime_error("missing DolRecomp native math controls patch: "+controls_patch.string());
    Require(Run("git -C "+Quote(p.dolrecomp_src)+" apply --check "+Quote(controls_patch)),
            "DolRecomp native math controls patch check");
    Require(Run("git -C "+Quote(p.dolrecomp_src)+" apply "+Quote(controls_patch)),
            "DolRecomp native math controls patch");
    std::cout << "GEKKOAOT_DOLRECOMP_NATIVE_MATH_ANY_CONTROLS_V340="
              << (Env("GEKKOAOT_NATIVE_MATH_ANY_CONTROLS","1") != "0" ? 1 : 0)
              << " controls=mxcsr-mirrored ni-tiny=helper\n";

    // v341: paired FMA keeps the strict f64 path in non-IEEE mode only when an
    // input lane is an f32 denormal (the single path is otherwise identical).
    const auto pfma_patch = p.root / "patches/dolrecomp/gekkoaot-paired-fma-ni-denormal-v341.patch";
    if (!fs::exists(pfma_patch))
      throw std::runtime_error("missing DolRecomp paired FMA NI patch: "+pfma_patch.string());
    Require(Run("git -C "+Quote(p.dolrecomp_src)+" apply --check "+Quote(pfma_patch)),
            "DolRecomp paired FMA NI patch check");
    Require(Run("git -C "+Quote(p.dolrecomp_src)+" apply "+Quote(pfma_patch)),
            "DolRecomp paired FMA NI patch");
    std::cout << "GEKKOAOT_DOLRECOMP_PAIRED_FMA_NI_DENORMAL_V341="
              << (Env("GEKKOAOT_PAIRED_FMA_NI_DENORMAL","1") != "0" ? 1 : 0)
              << " strict=ni+f32-denormal-input\n";

    // v342: ps_add/ps_sub/ps_mul/ps_muls with exact-single operands pick the
    // f32 path at run time unless NI zeroes an f32-denormal input.
    const auto pni_patch = p.root / "patches/dolrecomp/gekkoaot-paired-ni-denormal-v342.patch";
    if (!fs::exists(pni_patch))
      throw std::runtime_error("missing DolRecomp paired NI patch: "+pni_patch.string());
    Require(Run("git -C "+Quote(p.dolrecomp_src)+" apply --check "+Quote(pni_patch)),
            "DolRecomp paired NI patch check");
    Require(Run("git -C "+Quote(p.dolrecomp_src)+" apply "+Quote(pni_patch)),
            "DolRecomp paired NI patch");
    std::cout << "GEKKOAOT_DOLRECOMP_PAIRED_NI_DENORMAL_V342="
              << (Env("GEKKOAOT_PAIRED_NI_DENORMAL","1") != "0" ? 1 : 0)
              << " ops=add+sub+mul+muls strict=ni+f32-denormal-input\n";

    // v351: a REL import from a module that is not on the disc (RE4 disc 1:
    // st1_1.rel -> module 16) stays unpatched, as OSLink leaves it until that
    // module is linked, instead of failing the whole secondary REL batch.
    const auto rel_imports_patch = p.root / "patches/dolrecomp/gekkoaot-rel-unloaded-imports-v351.patch";
    if (!fs::exists(rel_imports_patch))
      throw std::runtime_error("missing DolRecomp REL unloaded-imports patch: "+rel_imports_patch.string());
    Require(Run("git -C "+Quote(p.dolrecomp_src)+" apply --check "+Quote(rel_imports_patch)),
            "DolRecomp REL unloaded-imports patch check");
    Require(Run("git -C "+Quote(p.dolrecomp_src)+" apply "+Quote(rel_imports_patch)),
            "DolRecomp REL unloaded-imports patch");
    std::cout << "GEKKOAOT_DOLRECOMP_REL_UNLOADED_IMPORTS_V351=1 policy=oslink-leave-unlinked\n";

    // v364: DOLRECOMP_REL_ALIGN lets the secondary build's dense canonical
    // REL layout (titles whose 64 KiB slots overflow REL24 range of the DOL)
    // reach the REL batch. Without it the batch keeps its 64 KiB slots.
    const auto rel_dense_patch = p.root / "patches/dolrecomp/gekkoaot-rel-dense-layout-v364.patch";
    if (!fs::exists(rel_dense_patch))
      throw std::runtime_error("missing DolRecomp REL dense-layout patch: "+rel_dense_patch.string());
    Require(Run("git -C "+Quote(p.dolrecomp_src)+" apply --check "+Quote(rel_dense_patch)),
            "DolRecomp REL dense-layout patch check");
    Require(Run("git -C "+Quote(p.dolrecomp_src)+" apply "+Quote(rel_dense_patch)),
            "DolRecomp REL dense-layout patch");
    std::cout << "GEKKOAOT_DOLRECOMP_REL_DENSE_LAYOUT_V364=1 env=DOLRECOMP_REL_ALIGN\n";

    std::cout << "GEKKOAOT_SOURCE_REUSE backend=dolrecomp unchanged="
              << source_times.RestoreUnchanged() << '\n';
  }
  // GekkoAOT owns the GameCube-facing runtime and FIFO decoder. Aurora is the
  // renderer/presentation backend only; no ModernGekko or Dolphin execution
  // chassis is fetched or linked by the standalone pipeline.
  EnsureCheckout(p.aurora_src,"https://github.com/encounter/aurora.git",GEKKOAOT_AURORA_REV);
  p.aurora_source_times.emplace(p.aurora_src);
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

  // v159: Aurora's ByteBuffer::append() performs a capacity check and a
  // memcpy for every u16 emitted by prepare_idx_buffer(). Retail triangle
  // strips/quads can execute this tens of thousands of times per frame. Expose
  // one append-uninitialized primitive so the GX decoder can reserve once and
  // fill the final index span directly.
  const auto aurora_internal_v159 = p.aurora_src / "lib/internal.hpp";
  ReplaceOnceInFile(aurora_internal_v159,
R"GEKKO(  void append_zeroes(size_t size) {
    resize(m_length + size, true);
    m_length += size;
  }

  void release() {
)GEKKO",
R"GEKKO(  void append_zeroes(size_t size) {
    resize(m_length + size, true);
    m_length += size;
  }

  uint8_t* append_uninitialized(size_t size) {
    const size_t offset = m_length;
    resize(m_length + size, false);
    m_length += size;
    return m_data + offset;
  }

  void release() {
)GEKKO", "Aurora ByteBuffer direct append v159");

  const auto aurora_cp_v159 = p.aurora_src / "lib/gx/command_processor.cpp";
  ReplaceOnceInFile(aurora_cp_v159,
R"GEKKO(u16 prepare_idx_buffer(ByteBuffer& buf, GXPrimitive prim, u16 vtxStart, u16 vtxCount) noexcept {
  u16 numIndices = 0;
  if (prim == GX_QUADS) {
    buf.reserve_extra((vtxCount / 4) * 6 * sizeof(u16));

    for (u16 v = 0; v < vtxCount; v += 4) {
      u16 idx0 = vtxStart + v;
      u16 idx1 = vtxStart + v + 1;
      u16 idx2 = vtxStart + v + 2;
      u16 idx3 = vtxStart + v + 3;

      buf.append(idx0);
      buf.append(idx1);
      buf.append(idx2);
      numIndices += 3;

      buf.append(idx2);
      buf.append(idx3);
      buf.append(idx0);
      numIndices += 3;
    }
  } else if (prim == GX_TRIANGLES) {
    buf.reserve_extra(vtxCount * sizeof(u16));
    for (u16 v = 0; v < vtxCount; ++v) {
      const u16 idx = vtxStart + v;
      buf.append(idx);
      ++numIndices;
    }
  } else if (prim == GX_TRIANGLEFAN) {
    buf.reserve_extra(((u32(vtxCount) - 3) * 3 + 3) * sizeof(u16));
    for (u16 v = 0; v < vtxCount; ++v) {
      const u16 idx = vtxStart + v;
      if (v < 3) {
        buf.append(idx);
        ++numIndices;
        continue;
      }
      buf.append(std::array{vtxStart, static_cast<u16>(idx - 1), idx});
      numIndices += 3;
    }
  } else if (prim == GX_TRIANGLESTRIP) {
    buf.reserve_extra(((static_cast<u32>(vtxCount) - 3) * 3 + 3) * sizeof(u16));
    for (u16 v = 0; v < vtxCount; ++v) {
      const u16 idx = vtxStart + v;
      if (v < 3) {
        buf.append(idx);
        ++numIndices;
        continue;
      }
      if ((v & 1) == 0) {
        buf.append(std::array{static_cast<u16>(idx - 2), static_cast<u16>(idx - 1), idx});
      } else {
        buf.append(std::array{static_cast<u16>(idx - 1), static_cast<u16>(idx - 2), idx});
      }
      numIndices += 3;
    }
  } else if (prim == GX_LINES || prim == GX_LINESTRIP || prim == GX_POINTS) {
    buf.reserve_extra(6 * sizeof(u16));
    buf.append<u16>(0);
    buf.append<u16>(1);
    buf.append<u16>(3);
    buf.append<u16>(3);
    buf.append<u16>(2);
    buf.append<u16>(0);
    numIndices = 6;
  } else
    UNLIKELY FATAL("unsupported primitive type {}", static_cast<u32>(prim));
  return numIndices;
}
)GEKKO",
R"GEKKO(u16 prepare_idx_buffer(ByteBuffer& buf, GXPrimitive prim, u16 vtxStart, u16 vtxCount) noexcept {
  u32 indexCount = 0;
  if (prim == GX_QUADS) {
    indexCount = ((u32(vtxCount) + 3u) / 4u) * 6u;
  } else if (prim == GX_TRIANGLES) {
    indexCount = vtxCount;
  } else if (prim == GX_TRIANGLEFAN || prim == GX_TRIANGLESTRIP) {
    indexCount = vtxCount <= 3u ? vtxCount : 3u + (u32(vtxCount) - 3u) * 3u;
  } else if (prim == GX_LINES || prim == GX_LINESTRIP || prim == GX_POINTS) {
    indexCount = 6u;
  } else {
    UNLIKELY FATAL("unsupported primitive type {}", static_cast<u32>(prim));
  }

  auto* out = reinterpret_cast<u16*>(buf.append_uninitialized(indexCount * sizeof(u16)));
  u32 n = 0;
  if (prim == GX_QUADS) {
    for (u32 v = 0; v < vtxCount; v += 4u) {
      const u16 i0 = static_cast<u16>(vtxStart + v);
      const u16 i1 = static_cast<u16>(i0 + 1u);
      const u16 i2 = static_cast<u16>(i0 + 2u);
      const u16 i3 = static_cast<u16>(i0 + 3u);
      out[n++] = i0; out[n++] = i1; out[n++] = i2;
      out[n++] = i2; out[n++] = i3; out[n++] = i0;
    }
  } else if (prim == GX_TRIANGLES) {
    for (u32 v = 0; v < vtxCount; ++v)
      out[n++] = static_cast<u16>(vtxStart + v);
  } else if (prim == GX_TRIANGLEFAN) {
    for (u32 v = 0; v < vtxCount; ++v) {
      const u16 idx = static_cast<u16>(vtxStart + v);
      if (v < 3u) out[n++] = idx;
      else {
        out[n++] = vtxStart;
        out[n++] = static_cast<u16>(idx - 1u);
        out[n++] = idx;
      }
    }
  } else if (prim == GX_TRIANGLESTRIP) {
    for (u32 v = 0; v < vtxCount; ++v) {
      const u16 idx = static_cast<u16>(vtxStart + v);
      if (v < 3u) out[n++] = idx;
      else if ((v & 1u) == 0u) {
        out[n++] = static_cast<u16>(idx - 2u);
        out[n++] = static_cast<u16>(idx - 1u);
        out[n++] = idx;
      } else {
        out[n++] = static_cast<u16>(idx - 1u);
        out[n++] = static_cast<u16>(idx - 2u);
        out[n++] = idx;
      }
    }
  } else {
    out[0] = 0u; out[1] = 1u; out[2] = 3u;
    out[3] = 3u; out[4] = 2u; out[5] = 0u;
    n = 6u;
  }
  return static_cast<u16>(n);
}
)GEKKO", "Aurora direct index fill v159");
  std::cout << "GEKKOAOT_AURORA_INDEX_FASTPATH_V159=1 reserve=once fill=direct append-calls=eliminated\n";

  const auto aurora_storage_patch =
      p.root / "patches/aurora/gekkoaot-storage-staging-v4.patch";
  if (!fs::exists(aurora_storage_patch))
    throw std::runtime_error("missing Aurora storage-staging patch: "+
                             aurora_storage_patch.string());
  Require(Run("git -C "+Quote(p.aurora_src)+" apply --recount --check "+
              Quote(aurora_storage_patch)),
          "Aurora storage-staging patch check");
  Require(Run("git -C "+Quote(p.aurora_src)+" apply --recount "+
              Quote(aurora_storage_patch)),
          "Aurora storage-staging patch");
  std::cout << "GEKKOAOT_AURORA_STORAGE_STAGING_V4=1 segment_bytes=67108864 gpu_bytes=67108864 "
               "overflow=pass-boundary-segmented-staging offsets=restart-at-zero single-draw=fail-closed\n";

  // v141: restore retail CP/XF matrix-index coherency. Dolphin keeps CP MATINDEX
  // A/B and XF MatrixIndexA/B as one logical state; Aurora 9c decodes only the
  // PN selector from CP 0x30 and ignores CP 0x40. That leaves texture/transform
  // selectors stale when games alternate CP and XF programming paths.
  const auto aurora_regs_v141 = p.aurora_src / "lib/gx/regs.cpp";
  ReplaceOnceInFile(aurora_regs_v141,
R"GEKKO(// Matrix index A (0x30)
void cp_mtx_index(u8, u32 value) noexcept {
  g_gxState.currentPnMtx = reg_get(value, 6, 0) / 3;
  g_gxState.xfRegValid.reset(0x18);
}
)GEKKO",
R"GEKKO(// Matrix index A (0x30), mirrors XF 0x18.
void cp_mtx_index(u8, u32 value) noexcept {
  g_gxState.currentPnMtx = reg_get(value, 6, 0) / 3;
  g_gxState.xfRegValid.reset(0x18);
  for (u32 i = 0; i < 4; ++i) {
    const auto texMtx = static_cast<GXTexMtx>(reg_get(value, 6, 6 + i * 6));
    if (g_gxState.tcgs[i].mtx != texMtx) {
      g_gxState.tcgs[i].mtx = texMtx;
      g_gxState.dirty |= DirtyPipeline;
    }
  }
}

// Matrix index B (0x40), mirrors XF 0x19.
void cp_mtx_index_b(u8, u32 value) noexcept {
  g_gxState.xfRegValid.reset(0x19);
  for (u32 i = 0; i < 4 && (i + 4) < MaxTexCoord; ++i) {
    const auto texMtx = static_cast<GXTexMtx>(reg_get(value, 6, i * 6));
    if (g_gxState.tcgs[i + 4].mtx != texMtx) {
      g_gxState.tcgs[i + 4].mtx = texMtx;
      g_gxState.dirty |= DirtyPipeline;
    }
  }
}
)GEKKO", "Aurora CP/XF matrix-index decode v141");

  ReplaceOnceInFile(aurora_regs_v141,
R"GEKKO(  regs[0x30] = {cp_mtx_index, DirtyImmediates};
  regs[0x40] = {}; // Matrix index B; mirrors XF 0x19
)GEKKO",
R"GEKKO(  regs[0x30] = {cp_mtx_index, DirtyImmediates};
  regs[0x40] = {cp_mtx_index_b};
)GEKKO", "Aurora CP matrix-index table v141");

  ReplaceOnceInFile(aurora_regs_v141,
R"GEKKO(// Matrix index B (0x19)
void xf_mtx_index_b(u8, u32 value) noexcept {
  for (u32 i = 0; i < 4 && (i + 4) < MaxTexCoord; i++) {
    g_gxState.tcgs[i + 4].mtx = static_cast<GXTexMtx>(reg_get(value, 6, i * 6));
  }
}
)GEKKO",
R"GEKKO(// Matrix index B (0x19)
void xf_mtx_index_b(u8, u32 value) noexcept {
  // A later CP 0x40 must not be deduplicated against state from before this XF
  // write. XF 0x18 already performs the symmetric CP 0x30 invalidation.
  g_gxState.cpRegValid.reset(0x40);
  for (u32 i = 0; i < 4 && (i + 4) < MaxTexCoord; i++) {
    g_gxState.tcgs[i + 4].mtx = static_cast<GXTexMtx>(reg_get(value, 6, i * 6));
  }
}
)GEKKO", "Aurora XF-B/CP-B shadow coherency v141");

  // Retail GX commands must not disappear because a host pipeline is still
  // compiling. Block only on a first-use GX pipeline cache miss; already-built
  // pipelines remain the same O(1) lookup path.
  const auto aurora_pipeline_cache_v141 = p.aurora_src / "lib/gfx/pipeline_cache.cpp";
  ReplaceOnceInFile(aurora_pipeline_cache_v141,
R"GEKKO(PipelineRef find_pipeline(const gx::PipelineConfig& config, const RenderTargetLayout& layout) {
  remember_pipeline_config(ShaderType::GX, config, current_frame(), true);
  return resolve_pipeline(ShaderType::GX, config, layout, PipelinePriority::Normal);
}
)GEKKO",
R"GEKKO(PipelineRef find_pipeline(const gx::PipelineConfig& config, const RenderTargetLayout& layout) {
  remember_pipeline_config(ShaderType::GX, config, current_frame(), true);
  // GekkoAOT v141: a retail GX draw is ordered work, not optional UI work.
  // Wait only when this exact pipeline has not been compiled yet.
  return resolve_pipeline(ShaderType::GX, config, layout, PipelinePriority::Blocking);
}
)GEKKO", "Aurora no-dropped-first-use GX pipeline v141");

  std::cout << "GEKKOAOT_AURORA_MATRIX_COHERENCY_V141=1 "
               "cp30=pn+tex0-3 cp40=tex4-7 xf19-invalidates-cp40=1 dirty=precise model=dolphin-style\\n";
  std::cout << "GEKKOAOT_AURORA_GX_PIPELINE_V141=1 "
               "first-use=blocking cached=fast policy=no-retail-draw-drop\\n";

  const auto aurora_vertex_index_capacity_patch =
      p.root / "patches/aurora/gekkoaot-vertex-index-capacity-v1.patch";
  if (!fs::exists(aurora_vertex_index_capacity_patch))
    throw std::runtime_error("missing Aurora vertex/index capacity patch: "+
                             aurora_vertex_index_capacity_patch.string());
  Require(Run("git -C "+Quote(p.aurora_src)+" apply --check "+
              Quote(aurora_vertex_index_capacity_patch)),
          "Aurora vertex/index capacity patch check");
  Require(Run("git -C "+Quote(p.aurora_src)+" apply "+
              Quote(aurora_vertex_index_capacity_patch)),
          "Aurora vertex/index capacity patch");
  std::cout << "GEKKOAOT_AURORA_VERTEX_INDEX_CAPACITY_V1=1 vertex_mib=32 index_mib=8 "
               "policy=single-frame-bounded-headroom\n";

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

  const auto aurora_viewport_patch =
      p.root / "patches/aurora/gekkoaot-retail-viewport-v1.patch";
  if (!fs::exists(aurora_viewport_patch))
    throw std::runtime_error("missing Aurora retail viewport patch: " + aurora_viewport_patch.string());
  Require(Run("git -C " + Quote(p.aurora_src) + " apply --check " + Quote(aurora_viewport_patch)),
          "Aurora retail viewport patch check");
  Require(Run("git -C " + Quote(p.aurora_src) + " apply " + Quote(aurora_viewport_patch)),
          "Aurora retail viewport patch");
  std::cout << "GEKKOAOT_AURORA_RETAIL_VIEWPORT_V1=1 origin=342\n";

  const auto aurora_texture_descriptor_patch =
      p.root / "patches/aurora/gekkoaot-texture-descriptor-cache-v1.patch";
  if (!fs::exists(aurora_texture_descriptor_patch))
    throw std::runtime_error("missing Aurora texture descriptor patch: " + aurora_texture_descriptor_patch.string());
  Require(Run("git -C " + Quote(p.aurora_src) + " apply --check " + Quote(aurora_texture_descriptor_patch)),
          "Aurora texture descriptor patch check");
  Require(Run("git -C " + Quote(p.aurora_src) + " apply " + Quote(aurora_texture_descriptor_patch)),
          "Aurora texture descriptor patch");
  std::cout << "GEKKOAOT_AURORA_TEXTURE_DESCRIPTOR_CACHE_V1=1 source=address+dimensions+format+mips palette=format+entries+version\n";

  // v159: raw retail FIFO reconstruction has stable source pointers and
  // explicit texDataVersion/tlutDataVersion mutation counters. Memoize the
  // expensive XXH3 content/source hash tuple on that versioned identity. This
  // preserves content-cache/replacement semantics while making repeated binds
  // O(1), including texObjId==0 clients where the object cache cannot help.
  const auto aurora_texture_v159 = p.aurora_src / "lib/gx/texture.cpp";
  ReplaceOnceInFile(aurora_texture_v159,
R"GEKKO(void apply_pending_invalidations() noexcept {
  const uint64_t pendingCacheClears = s_pendingCacheClears.exchange(0, std::memory_order_acq_rel);
  const uint64_t pendingInvalidations = s_pendingInvalidations.exchange(0, std::memory_order_acq_rel);
)GEKKO",
R"GEKKO(void apply_pending_invalidations() noexcept {
  // v159: current_bind_generation() is queried on every draw. Avoid two locked
  // atomic exchanges when no producer published an invalidation since the last
  // draw; a racing publication remains pending for the next boundary exactly
  // as it can when racing the original exchange itself.
  if (s_pendingCacheClears.load(std::memory_order_acquire) == 0 &&
      s_pendingInvalidations.load(std::memory_order_acquire) == 0)
    return;
  const uint64_t pendingCacheClears = s_pendingCacheClears.exchange(0, std::memory_order_acq_rel);
  const uint64_t pendingInvalidations = s_pendingInvalidations.exchange(0, std::memory_order_acq_rel);
)GEKKO", "Aurora bind-generation atomic fast path v159");

  ReplaceOnceInFile(aurora_texture_v159,
R"GEKKO(struct TextureKeys {
  TextureContentKey contentKey;
  std::optional<aurora::texture::TextureSourceKey> sourceKey;
};
)GEKKO",
R"GEKKO(struct TextureKeys {
  TextureContentKey contentKey;
  std::optional<aurora::texture::TextureSourceKey> sourceKey;
};

struct TextureHashMemoKey {
  const void* data = nullptr;
  size_t bytes = 0;
  u32 width = 0;
  u32 height = 0;
  u32 format = 0;
  u32 mipCount = 0;
  u32 texDataVersion = 0;
  const void* tlutData = nullptr;
  u32 tlutDataVersion = 0;
  u32 tlutFormat = 0;
  u16 tlutEntries = 0;
  bool buildSourceKey = false;

  bool operator==(const TextureHashMemoKey&) const = default;
  template <typename H>
  friend H AbslHashValue(H h, const TextureHashMemoKey& key) {
    return H::combine(std::move(h), key.data, key.bytes, key.width, key.height,
                      key.format, key.mipCount, key.texDataVersion, key.tlutData,
                      key.tlutDataVersion, key.tlutFormat, key.tlutEntries,
                      key.buildSourceKey);
  }
};

constexpr size_t TextureHashMemoMaxEntries = 8192;
absl::flat_hash_map<TextureHashMemoKey, TextureKeys> s_textureHashMemo;
)GEKKO", "Aurora texture hash memo types v159");

  ReplaceOnceInFile(aurora_texture_v159,
R"GEKKO(TextureKeys hash_texture_source(const GXTexObj_& obj, const GXTlutObj_* tlut, bool buildSourceKey) {
  ZoneScoped;
  const size_t textureBytes = texture::texture_source_size(obj.format(), obj.width(), obj.height(), obj.mip_count());
  CHECK(obj.has_data() && textureBytes != 0, "invalid texture source for content hash");

  TextureKeys keys;
)GEKKO",
R"GEKKO(TextureKeys hash_texture_source(const GXTexObj_& obj, const GXTlutObj_* tlut, bool buildSourceKey) {
  ZoneScoped;
  const size_t textureBytes = texture::texture_source_size(obj.format(), obj.width(), obj.height(), obj.mip_count());
  CHECK(obj.has_data() && textureBytes != 0, "invalid texture source for content hash");

  const TextureHashMemoKey memoKey{
      .data = obj.data,
      .bytes = textureBytes,
      .width = obj.width(),
      .height = obj.height(),
      .format = obj.format(),
      .mipCount = obj.mip_count(),
      .texDataVersion = obj.texDataVersion,
      .tlutData = tlut ? tlut->data : nullptr,
      .tlutDataVersion = tlut ? tlut->tlutDataVersion : 0u,
      .tlutFormat = tlut ? static_cast<u32>(tlut->format) : 0u,
      .tlutEntries = static_cast<u16>(tlut ? tlut->numEntries : 0u),
      .buildSourceKey = buildSourceKey,
  };
  if (const auto memo = s_textureHashMemo.find(memoKey); memo != s_textureHashMemo.end()) {
    return memo->second;
  }

  TextureKeys keys;
)GEKKO", "Aurora texture hash memo lookup v159");

  ReplaceOnceInFile(aurora_texture_v159,
R"GEKKO(  }
  return keys;
}

gfx::TextureHandle get_tlut_texture(const GXTlutObj_& tlut) {
)GEKKO",
R"GEKKO(  }
  if (s_textureHashMemo.size() >= TextureHashMemoMaxEntries)
    s_textureHashMemo.clear();
  s_textureHashMemo.insert_or_assign(memoKey, keys);
  return keys;
}

gfx::TextureHandle get_tlut_texture(const GXTlutObj_& tlut) {
)GEKKO", "Aurora texture hash memo publish v159");

  ReplaceOnceInFile(aurora_texture_v159,
R"GEKKO(  s_sourceKeyCache.clear();
  s_contentCacheBytes = 0;
)GEKKO",
R"GEKKO(  s_sourceKeyCache.clear();
  s_textureHashMemo.clear();
  s_contentCacheBytes = 0;
)GEKKO", "Aurora texture hash memo shutdown v159");
  std::cout << "GEKKOAOT_AURORA_TEXTURE_HASH_MEMO_V159=1 key=pointer+descriptor+versions max=8192 content-semantics=retained bind-atomics=load-before-exchange\n";


  // v160: Aurora persists every seen GX pipeline and normally recompiles the
  // complete historical cache in the background on startup. Large retail
  // caches can keep Dawn/driver compiler threads saturated for seconds and,
  // on some drivers, temporarily serialize presentation. Keep the disk cache,
  // but cap/throttle background warmup while preserving immediate priority for
  // pipelines the running game actually requests.
  const auto aurora_pipeline_cache_v160 = p.aurora_src / "lib/gfx/pipeline_cache.cpp";
  ReplaceOnceInFile(aurora_pipeline_cache_v160,
R"GEKKO(#include <algorithm>
#include <atomic>
)GEKKO",
R"GEKKO(#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <atomic>
)GEKKO", "Aurora pipeline anti-hitch includes v160");
  ReplaceOnceInFile(aurora_pipeline_cache_v160,
R"GEKKO(#include <SDL3/SDL_iostream.h>
)GEKKO",
R"GEKKO(#include <SDL3/SDL_iostream.h>
#include <SDL3/SDL_thread.h>
)GEKKO", "Aurora pipeline worker priority include v160");
  ReplaceOnceInFile(aurora_pipeline_cache_v160,
R"GEKKO(#ifdef NDEBUG
constexpr size_t BuildPipelinesPerFrame = 5;
#else
constexpr size_t BuildPipelinesPerFrame = 1;
#endif
static std::thread g_pipelineThread;
)GEKKO",
R"GEKKO(#ifdef NDEBUG
constexpr size_t BuildPipelinesPerFrame = 5;
#else
constexpr size_t BuildPipelinesPerFrame = 1;
#endif

static size_t gekkoaot_env_size(const char* name, size_t fallback, size_t maximum) {
  const char* value = std::getenv(name);
  if (value == nullptr || value[0] == '\0') return fallback;
  char* end = nullptr;
  const unsigned long long parsed = std::strtoull(value, &end, 10);
  if (end == value || (end != nullptr && *end != '\0')) return fallback;
  return std::min<size_t>(static_cast<size_t>(parsed), maximum);
}

static size_t gekkoaot_pipeline_warmup_limit() {
  static const size_t value = gekkoaot_env_size("GEKKOAOT_AURORA_PIPELINE_WARMUP_LIMIT", 32u, 4096u);
  return value;
}

static size_t gekkoaot_pipeline_background_sleep_us() {
  static const size_t value = gekkoaot_env_size("GEKKOAOT_AURORA_PIPELINE_BG_SLEEP_US", 2000u, 50000u);
  return value;
}

static std::thread g_pipelineThread;
)GEKKO", "Aurora pipeline anti-hitch policy v160");
  ReplaceOnceInFile(aurora_pipeline_cache_v160,
R"GEKKO(static void pipeline_worker() {
#ifdef TRACY_ENABLE
  tracy::SetThreadName("Pipeline compilation thread");
#endif

  bool hasMore = false;
)GEKKO",
R"GEKKO(static void pipeline_worker() {
#ifdef TRACY_ENABLE
  tracy::SetThreadName("Pipeline compilation thread");
#endif
  (void)SDL_SetCurrentThreadPriority(SDL_THREAD_PRIORITY_LOW);

  bool hasMore = false;
)GEKKO", "Aurora pipeline worker low priority v160");
  ReplaceOnceInFile(aurora_pipeline_cache_v160,
R"GEKKO(  while (g_hasPipelineThread || g_pipelinesPerFrame < BuildPipelinesPerFrame) {
    PendingPipeline pending;
    {
)GEKKO",
R"GEKKO(  while (g_hasPipelineThread || g_pipelinesPerFrame < BuildPipelinesPerFrame) {
    PendingPipeline pending;
    bool backgroundCompile = false;
    {
)GEKKO", "Aurora pipeline worker background marker v160");
  ReplaceOnceInFile(aurora_pipeline_cache_v160,
R"GEKKO(      auto& source = !g_pipelineQueue.empty() ? g_pipelineQueue : g_backgroundPipelineQueue;
)GEKKO",
R"GEKKO(      backgroundCompile = g_pipelineQueue.empty();
      auto& source = backgroundCompile ? g_backgroundPipelineQueue : g_pipelineQueue;
)GEKKO", "Aurora pipeline live queue priority v160");
  ReplaceOnceInFile(aurora_pipeline_cache_v160,
R"GEKKO(    notify_pipeline_ready(true, pipelineCount);
  }
}
)GEKKO",
R"GEKKO(    notify_pipeline_ready(true, pipelineCount);
    if (backgroundCompile && g_hasPipelineThread) {
      const size_t sleepUs = gekkoaot_pipeline_background_sleep_us();
      if (sleepUs != 0u)
        std::this_thread::sleep_for(std::chrono::microseconds(sleepUs));
    }
  }
}
)GEKKO", "Aurora pipeline background throttle v160");
  ReplaceOnceInFile(aurora_pipeline_cache_v160,
R"GEKKO(  std::ranges::sort(known, {}, &KnownPipeline::firstFrameUsed);
  for (const auto& pipeline : known) {
    std::visit(
        [&](const auto& config) {
          resolve_pipeline(pipeline.type, config, scene, PipelinePriority::Background);
          if (offscreen.key != scene.key) {
            resolve_pipeline(pipeline.type, config, offscreen, PipelinePriority::Background);
          }
        },
        pipeline.config);
  }
}
)GEKKO",
R"GEKKO(  std::ranges::sort(known, {}, &KnownPipeline::firstFrameUsed);
  const size_t warmupLimit = std::min(known.size(), gekkoaot_pipeline_warmup_limit());
  size_t warmupQueued = 0;
  for (const auto& pipeline : known) {
    if (warmupQueued >= warmupLimit) break;
    std::visit(
        [&](const auto& config) {
          resolve_pipeline(pipeline.type, config, scene, PipelinePriority::Background);
          ++warmupQueued;
        },
        pipeline.config);
  }
  Log.info("GEKKOAOT_AURORA_PIPELINE_ANTIHITCH_V160=1 cached={} warmup={} background_sleep_us={} "
           "offscreen=on-demand live-priority=normal thread-priority=low",
           known.size(), warmupQueued, gekkoaot_pipeline_background_sleep_us());
}
)GEKKO", "Aurora bounded pipeline warmup v160");
  std::cout << "GEKKOAOT_AURORA_PIPELINE_ANTIHITCH_V160=1 warmup-default=32 bg-sleep-us-default=2000 live-priority=normal thread-priority=low disk-cache=retained\\n";

  // v142: GameCube vertex matrix indices are six-bit values. Dolphin masks
  // both PNMTXIDX and TEXMTXIDX with 0x3f while translating the FIFO vertex
  // stream. Aurora 9c consumed the full host byte instead, so stale/high bits
  // in a recycled skinning buffer could select a completely different matrix
  // (or index outside the packed 10 PN + 10 texture-matrix uniform array).
  // Keep Aurora's packed-matrix representation, but apply the hardware mask
  // before converting the raw XF-row selector to a packed matrix index.
  const auto aurora_matrix_index_shader_v142 = p.aurora_src / "lib/gx/shader.cpp";
  ReplaceOnceInFile(aurora_matrix_index_shader_v142,
R"GEKKO(  case GX_VA_PNMTXIDX:
    return fmt::format("(raw_fetch_u8_1(&{}, {}) / 3u)", buf, offs);
  case GX_VA_TEX0MTXIDX:
  case GX_VA_TEX1MTXIDX:
  case GX_VA_TEX2MTXIDX:
  case GX_VA_TEX3MTXIDX:
  case GX_VA_TEX4MTXIDX:
  case GX_VA_TEX5MTXIDX:
  case GX_VA_TEX6MTXIDX:
  case GX_VA_TEX7MTXIDX:
    return fmt::format("raw_fetch_u8_1(&{}, {})", buf, offs);
)GEKKO",
R"GEKKO(  case GX_VA_PNMTXIDX:
    // Flipper consumes only the low six bits of the direct matrix-index byte.
    // Aurora stores three XF rows as one Mat3x4, hence /3 after masking.
    return fmt::format("((raw_fetch_u8_1(&{}, {}) & 0x3fu) / 3u)", buf, offs);
  case GX_VA_TEX0MTXIDX:
  case GX_VA_TEX1MTXIDX:
  case GX_VA_TEX2MTXIDX:
  case GX_VA_TEX3MTXIDX:
  case GX_VA_TEX4MTXIDX:
  case GX_VA_TEX5MTXIDX:
  case GX_VA_TEX6MTXIDX:
  case GX_VA_TEX7MTXIDX:
    return fmt::format("(raw_fetch_u8_1(&{}, {}) & 0x3fu)", buf, offs);
)GEKKO", "Aurora six-bit per-vertex matrix indices v142");
  std::cout << "GEKKOAOT_AURORA_VERTEX_MATRIX_INDEX_V142=1 "
               "pn=mask6+packed-row-div3 tex=mask6 identity=60 reference=dolphin-vertex-loader\n";

  // v143: model the XF transform bank as row-addressed Flipper memory rather
  // than 10 packed SDK matrices. The hardware exposes 0x000..0x0ff as 64
  // vec4 transform rows shared by position and texture transforms, and
  // 0x400..0x45f as 32 vec3 normal rows. Per-vertex PNMTXIDX is a six-bit row
  // selector; Dolphin reconstructs a matrix from rows idx, idx+1 and idx+2.
  // This also makes partial/indexed XF loads coherent instead of requiring a
  // perfectly aligned 12/9-word whole-matrix update.
  const auto aurora_gx_h_v143 = p.aurora_src / "lib/gx/gx.hpp";
  ReplaceOnceInFile(aurora_gx_h_v143,
R"GEKKO(constexpr u32 MaxPnMtx = (GX_PNMTX9 / 3) + 1;
constexpr u32 MaxIndexAttr = 12; // VA_POS -> VA_TEX7
)GEKKO",
R"GEKKO(constexpr u32 MaxPnMtx = (GX_PNMTX9 / 3) + 1;
constexpr u32 MaxXfPosRows = 64;
constexpr u32 MaxXfPosUniformRows = 66; // idx 63 + two robust zero rows
constexpr u32 MaxXfNrmRows = 32;
constexpr u32 MaxXfNrmUniformRows = 34; // idx 31 + two robust zero rows
constexpr u32 MaxIndexAttr = 12; // VA_POS -> VA_TEX7
)GEKKO", "Aurora XF row constants v143");

  ReplaceOnceInFile(aurora_gx_h_v143,
R"GEKKO(constexpr u32 MaxUniformSize = 3840;
)GEKKO",
R"GEKKO(constexpr u32 MaxUniformSize = 4096;
)GEKKO", "Aurora XF expanded uniform capacity v143");

  ReplaceOnceInFile(aurora_gx_h_v143,
R"GEKKO(  // Decoded state
  std::array<PnMtx, MaxPnMtx> pnMtx;
  u32 currentPnMtx;
)GEKKO",
R"GEKKO(  // Decoded state
  // Legacy packed matrices are retained for SDK-facing helpers/tests, but
  // retail FIFO rendering uses the raw row-addressed XF banks below.
  std::array<PnMtx, MaxPnMtx> pnMtx;
  std::array<Vec4<float>, MaxXfPosRows> xfPosRows{};
  std::array<Vec4<float>, MaxXfNrmRows> xfNrmRows{};
  u32 currentPnMtx;
)GEKKO", "Aurora XF raw row state v143");

  // CP/XF MatrixIndexA stores a raw six-bit row selector. v141/v142 divided
  // it by three only because Aurora previously addressed packed Mat3x4 slots.
  ReplaceOnceInFile(aurora_regs_v141,
R"GEKKO(void cp_mtx_index(u8, u32 value) noexcept {
  g_gxState.currentPnMtx = reg_get(value, 6, 0) / 3;
  g_gxState.xfRegValid.reset(0x18);
)GEKKO",
R"GEKKO(void cp_mtx_index(u8, u32 value) noexcept {
  g_gxState.currentPnMtx = reg_get(value, 6, 0) & 0x3f;
  g_gxState.xfRegValid.reset(0x18);
)GEKKO", "Aurora CP raw PN row selector v143");

  ReplaceOnceInFile(aurora_regs_v141,
R"GEKKO(void xf_mtx_index_a(u8, u32 value) noexcept {
  g_gxState.currentPnMtx = reg_get(value, 6, 0) / 3;
)GEKKO",
R"GEKKO(void xf_mtx_index_a(u8, u32 value) noexcept {
  g_gxState.currentPnMtx = reg_get(value, 6, 0) & 0x3f;
)GEKKO", "Aurora XF raw PN row selector v143");

  ReplaceOnceInFile(aurora_regs_v141,
R"GEKKO(  if (addr < 0x78) {
    // Position matrices (0x0000-0x0077)
    u32 mtxIdx = addr / 12;
    u32 startOffset = addr % 12;
    CHECK(mtxIdx < MaxPnMtx, "XF: PosMtx copy oob? Should never happen; mtxIdx={}", mtxIdx);
    CHECK(startOffset == 0 && len == 12, "XF: PosMtx sub-copy unsupported: offs={}, len={}", startOffset, len);
    f32* flat = reinterpret_cast<f32*>(&g_gxState.pnMtx[mtxIdx].pos);
    bool changed = false;
    for (u32 i = 0; i < len; i++) {
      changed |= store_xf_f32(flat[i], read_bits<u32>(data + i * 4, e));
    }
    if (changed) {
      g_gxState.dirty |= DirtyUniform;
    }
    return true;
  }
  if (addr < 0x0F0) {
    // Texture matrices (0x078-0x0EF)
    u32 texBase = addr - 0x078;
    u32 mtxIdx = texBase / 12;
    u32 startOffset = texBase % 12;
    CHECK(mtxIdx < MaxTexMtx, "XF TexMtx copy oob? Should never happen; mtxIdx={}", mtxIdx);
    CHECK(startOffset == 0 && (len == 8 || len == 12), "XF TexMtx sub-copy unsupported: offs={}, len={}", startOffset,
          len);

    f32* flat = reinterpret_cast<f32*>(&g_gxState.texMtxs[mtxIdx]);
    bool changed = false;
    for (u32 i = 0; i < len; i++) {
      changed |= store_xf_f32(flat[i], read_bits<u32>(data + i * 4, e));
    }
    if (changed) {
      g_gxState.dirty |= DirtyUniform;
    }
    return true;
  }
)GEKKO",
R"GEKKO(  if (addr < 0x100 && len <= 0x100 - addr) {
    // Flipper XF transform memory is one row-addressed bank shared by
    // position and regular texture matrices (0x0000-0x00ff). Accept arbitrary
    // aligned or partial loads instead of assuming SDK whole-matrix commands.
    bool changed = false;
    for (u32 i = 0; i < len; ++i) {
      const u32 word = addr + i;
      const u32 row = word / 4;
      const u32 col = word % 4;
      changed |= store_xf_f32(g_gxState.xfPosRows[row][col],
                              read_bits<u32>(data + i * 4, e));
    }
    if (changed) g_gxState.dirty |= DirtyUniform;
    return true;
  }
)GEKKO", "Aurora raw XF position/texture rows v143");

  ReplaceOnceInFile(aurora_regs_v141,
R"GEKKO(  if (addr >= 0x400 && addr < 0x45A) {
    // Normal matrices (0x400-0x459)
    u32 nrmBase = addr - 0x400;
    u32 mtxIdx = nrmBase / 9;
    u32 startOffset = nrmBase % 9;
    CHECK(mtxIdx < MaxPnMtx, "XF: NrmMtx copy oob? Should never happen; mtxIdx={}", mtxIdx);
    CHECK(startOffset == 0 && len == 9, "XF: NrmMtx sub-copy unsupported: offs={}, len={}", startOffset, len);
    f32* flat = reinterpret_cast<f32*>(&g_gxState.pnMtx[mtxIdx].nrm);
    bool changed = false;
    for (u32 i = 0; i < len; i++) {
      // 3x3 source packed into 3x4 storage
      u32 row = i / 3;
      u32 col = i % 3;
      changed |= store_xf_f32(flat[row * 4 + col], read_bits<u32>(data + i * 4, e));
    }
    if (changed) {
      g_gxState.dirty |= DirtyUniform;
    }
    return true;
  }
)GEKKO",
R"GEKKO(  if (addr >= 0x400 && addr < 0x460 && len <= 0x460 - addr) {
    // Normal XF memory is 32 row-addressed vec3 rows. The PN selector uses
    // its low five bits for normals, matching Flipper/Dolphin.
    bool changed = false;
    for (u32 i = 0; i < len; ++i) {
      const u32 word = (addr - 0x400) + i;
      const u32 row = word / 3;
      const u32 col = word % 3;
      changed |= store_xf_f32(g_gxState.xfNrmRows[row][col],
                              read_bits<u32>(data + i * 4, e));
    }
    if (changed) g_gxState.dirty |= DirtyUniform;
    return true;
  }
)GEKKO", "Aurora raw XF normal rows v143");

  const auto aurora_shader_info_v143 = p.aurora_src / "lib/gx/shader_info.cpp";
  ReplaceOnceInFile(aurora_shader_info_v143,
R"GEKKO(  // 10 position matrices, 10 texture matrices, 10 normal matrices.
  info.uniformSize += sizeof(Mat3x4<float>) * 30;
)GEKKO",
R"GEKKO(  // Raw Flipper XF row banks: 64 transform rows (+2 zero safety rows)
  // and 32 normal rows (+2 zero safety rows).
  info.uniformSize += sizeof(Vec4<float>) * (MaxXfPosUniformRows + MaxXfNrmUniformRows);
)GEKKO", "Aurora XF raw uniform sizing v143");

  ReplaceOnceInFile(aurora_shader_info_v143,
R"GEKKO(  for (int i = 0; i < MaxPnMtx; i++) {
    buf.append(g_gxState.pnMtx[i].pos);
  }

  for (int i = 0; i < MaxTexMtx; i++) {
    buf.append(g_gxState.texMtxs[i]);
  }

  for (int i = 0; i < MaxPnMtx; i++) {
    buf.append(g_gxState.pnMtx[i].nrm);
  }
)GEKKO",
R"GEKKO(  for (const auto& row : g_gxState.xfPosRows) buf.append(row);
  for (u32 i = MaxXfPosRows; i < MaxXfPosUniformRows; ++i) buf.append(Vec4<float>{});
  for (const auto& row : g_gxState.xfNrmRows) buf.append(row);
  for (u32 i = MaxXfNrmRows; i < MaxXfNrmUniformRows; ++i) buf.append(Vec4<float>{});
)GEKKO", "Aurora XF raw uniform upload v143");

  // v142's six-bit mask remains correct, but /3 is not: PNMTXIDX addresses a
  // raw XF row. Texture matrix indices are raw rows as well.
  ReplaceOnceInFile(aurora_matrix_index_shader_v142,
R"GEKKO(    // Flipper consumes only the low six bits of the direct matrix-index byte.
    // Aurora stores three XF rows as one Mat3x4, hence /3 after masking.
    return fmt::format("((raw_fetch_u8_1(&{}, {}) & 0x3fu) / 3u)", buf, offs);
)GEKKO",
R"GEKKO(    // Flipper consumes the low six bits as a raw XF row selector.
    return fmt::format("(raw_fetch_u8_1(&{}, {}) & 0x3fu)", buf, offs);
)GEKKO", "Aurora raw per-vertex PN row selector v143");

  ReplaceOnceInFile(aurora_matrix_index_shader_v142,
R"GEKKO(  uniBufAttrs += "\n    proj: mat4x4f,";
  uniBufAttrs += fmt::format("\n    postex_mtx: array<mat3x4f, {}>,", MaxPnMtx + MaxTexMtx);
  uniBufAttrs += fmt::format("\n    nrm_mtx: array<mat3x4f, {}>,", MaxPnMtx);
)GEKKO",
R"GEKKO(  uniBufAttrs += "\n    proj: mat4x4f,";
  uniBufAttrs += fmt::format("\n    xf_pos_rows: array<vec4f, {}>,", MaxXfPosUniformRows);
  uniBufAttrs += fmt::format("\n    xf_nrm_rows: array<vec4f, {}>,", MaxXfNrmUniformRows);
  texBindings += R"WGSL(
fn gekkoaot_xf_pos(v: vec4f, idx_raw: u32) -> vec3f {
    let i = idx_raw & 0x3fu;
    return vec3f(dot(v, ubuf.xf_pos_rows[i]),
                 dot(v, ubuf.xf_pos_rows[i + 1u]),
                 dot(v, ubuf.xf_pos_rows[i + 2u]));
}
fn gekkoaot_xf_nrm(v: vec4f, idx_raw: u32) -> vec3f {
    let i = idx_raw & 31u;
    return vec3f(dot(v, ubuf.xf_nrm_rows[i]),
                 dot(v, ubuf.xf_nrm_rows[i + 1u]),
                 dot(v, ubuf.xf_nrm_rows[i + 2u]));
}
)WGSL";
)GEKKO", "Aurora raw XF shader uniforms/helpers v143");

  // Position paths: points, expanded lines and ordinary triangles.
  ReplaceOnceInFile(aurora_matrix_index_shader_v142,
R"GEKKO(          "\n    let mv_pos = vec4f(in_pos, 1.0) * ubuf.postex_mtx[in_pnmtxidx];",
)GEKKO",
R"GEKKO(          "\n    let mv_pos = gekkoaot_xf_pos(vec4f(in_pos, 1.0), in_pnmtxidx);",
)GEKKO", "Aurora XF point position v143");

  ReplaceOnceInFile(aurora_matrix_index_shader_v142,
R"GEKKO(          "\n    let mv_pos_a = vec4f(pos_a, 1.0) * ubuf.postex_mtx[pnmtxidx_a];"
          "\n    let mv_pos_b = vec4f(pos_b, 1.0) * ubuf.postex_mtx[pnmtxidx_b];"
)GEKKO",
R"GEKKO(          "\n    let mv_pos_a = gekkoaot_xf_pos(vec4f(pos_a, 1.0), pnmtxidx_a);"
          "\n    let mv_pos_b = gekkoaot_xf_pos(vec4f(pos_b, 1.0), pnmtxidx_b);"
)GEKKO", "Aurora XF line position v143");

  ReplaceOnceInFile(aurora_matrix_index_shader_v142,
R"GEKKO(    vtxXfrAttrsPre += fmt::format(
        "\n    let mv_pos = vec4f({}, 1.0) * ubuf.postex_mtx[in_pnmtxidx];"
        "\n    out.pos = vec4f(mv_pos, 1.0) * ubuf.proj;",
        vtx_attr(config, GX_VA_POS));
)GEKKO",
R"GEKKO(    vtxXfrAttrsPre += fmt::format(
        "\n    let mv_pos = gekkoaot_xf_pos(vec4f({}, 1.0), in_pnmtxidx);"
        "\n    out.pos = vec4f(mv_pos, 1.0) * ubuf.proj;",
        vtx_attr(config, GX_VA_POS));
)GEKKO", "Aurora XF triangle position v143");

  ReplaceOnceInFile(aurora_matrix_index_shader_v142,
R"GEKKO(  vtxXfrAttrsPre += fmt::format(
      "\n    let nrm_tmp = vec4f({}, 0.0) * ubuf.nrm_mtx[in_pnmtxidx];"
      "\n    let mv_nrm = select(nrm_tmp, normalize(nrm_tmp), dot(nrm_tmp, nrm_tmp) > 1e-10);",
      vtx_attr(config, GX_VA_NRM));
)GEKKO",
R"GEKKO(  vtxXfrAttrsPre += fmt::format(
      "\n    let nrm_tmp = gekkoaot_xf_nrm(vec4f({}, 0.0), in_pnmtxidx);"
      "\n    let mv_nrm = select(nrm_tmp, normalize(nrm_tmp), dot(nrm_tmp, nrm_tmp) > 1e-10);",
      vtx_attr(config, GX_VA_NRM));
)GEKKO", "Aurora XF vertex normal v143");

  ReplaceOnceInFile(aurora_matrix_index_shader_v142,
R"GEKKO(          "\n    let bump_tan{0} = vec4f(in_tangent, 0.0) * ubuf.nrm_mtx[in_pnmtxidx];"
          "\n    let bump_bin{0} = vec4f(in_binrm, 0.0) * ubuf.nrm_mtx[in_pnmtxidx];"
)GEKKO",
R"GEKKO(          "\n    let bump_tan{0} = gekkoaot_xf_nrm(vec4f(in_tangent, 0.0), in_pnmtxidx);"
          "\n    let bump_bin{0} = gekkoaot_xf_nrm(vec4f(in_binrm, 0.0), in_pnmtxidx);"
)GEKKO", "Aurora XF bump normals v143");

  // Regular texture transforms select rows from the same 64-row XF bank.
  ReplaceOnceInFile(aurora_matrix_index_shader_v142,
R"GEKKO(        // GX_IDENTITY (60) bypasses the texture matrix. It would otherwise
        // address postex_mtx[20], one element past the 10 PN + 10 tex matrices.
        vtxXfrAttrs += fmt::format(
            "\n    var tc{0}_tmp = tc{0}.xyz;"
            "\n    if (in_texmtxidx{0} < {1}u) {{"
            "\n        tc{0}_tmp = tc{0} * ubuf.postex_mtx[in_texmtxidx{0} / 3u];"
            "\n    }}",
            i, static_cast<u32>(GX_IDENTITY));
)GEKKO",
R"GEKKO(        // GX_IDENTITY (60) bypasses the texture matrix; every other
        // direct index selects a raw row in the shared XF transform bank.
        vtxXfrAttrs += fmt::format(
            "\n    var tc{0}_tmp = tc{0}.xyz;"
            "\n    if (in_texmtxidx{0} < {1}u) {{"
            "\n        tc{0}_tmp = gekkoaot_xf_pos(tc{0}, in_texmtxidx{0});"
            "\n    }}",
            i, static_cast<u32>(GX_IDENTITY));
)GEKKO", "Aurora XF dynamic texture matrix v143");

  ReplaceOnceInFile(aurora_matrix_index_shader_v142,
R"GEKKO(        u32 texMtxIdx = (tcg.mtx) / 3;
        vtxXfrAttrs += fmt::format("\n    var tc{0}_tmp = tc{0} * ubuf.postex_mtx[{1}];", i, texMtxIdx);
)GEKKO",
R"GEKKO(        const u32 texMtxRow = static_cast<u32>(tcg.mtx) & 0x3fu;
        vtxXfrAttrs += fmt::format("\n    var tc{0}_tmp = gekkoaot_xf_pos(tc{0}, {1}u);", i, texMtxRow);
)GEKKO", "Aurora XF fixed texture matrix v143");

  std::cout << "GEKKOAOT_AURORA_XF_PARITY_V143=1 "
               "transform-rows=64 normal-rows=32 pn=row-index texture=shared-bank partial-loads=1 reference=dolphin-xfmem\\n";

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

  const auto aurora_efb_depth_copy_patch =
      p.root / "patches/aurora/gekkoaot-efb-depth-copy-v1.patch";
  if (!fs::exists(aurora_efb_depth_copy_patch))
    throw std::runtime_error("missing Aurora EFB depth-copy patch: "+
                             aurora_efb_depth_copy_patch.string());
  Require(Run("git -C "+Quote(p.aurora_src)+" apply --check "+
              Quote(aurora_efb_depth_copy_patch)),
          "Aurora EFB depth-copy patch check");
  Require(Run("git -C "+Quote(p.aurora_src)+" apply "+
              Quote(aurora_efb_depth_copy_patch)),
          "Aurora EFB depth-copy patch");
  std::cout << "GEKKOAOT_AURORA_EFB_DEPTH_COPY_V1=1 "
               "formats=z4+z8+z8m+z8l+z16+z16r+z16l+z24x8 raw-z16r=0x3b\n";

  const auto aurora_ztexture_patch =
      p.root / "patches/aurora/gekkoaot-ztexture-v1.patch";
  if (!fs::exists(aurora_ztexture_patch))
    throw std::runtime_error("missing Aurora Z-texture patch: "+
                             aurora_ztexture_patch.string());
  // shader.cpp and texture_convert.cpp are already changed by earlier Aurora
  // patches. Apply every independent Z-texture hunk with git, then patch those
  // two files through exact unique anchors. This avoids nested-patch line drift.
  const std::string ztexture_excludes =
      " --exclude=lib/gx/shader.cpp --exclude=lib/gfx/texture_convert.cpp ";
  Require(Run("git -C "+Quote(p.aurora_src)+" apply --recount --check"+ztexture_excludes+
              Quote(aurora_ztexture_patch)),
          "Aurora Z-texture independent patch check");
  Require(Run("git -C "+Quote(p.aurora_src)+" apply --recount"+ztexture_excludes+
              Quote(aurora_ztexture_patch)),
          "Aurora Z-texture independent patch");

  const auto aurora_shader = p.aurora_src / "lib/gx/shader.cpp";
  ReplaceOnceInFile(aurora_shader,
R"GEKKO(  std::string vtxXfrAttrsPre;
  std::string vtxXfrAttrs;
  size_t vtxOutIdx = 0;

  // Load points for line/point expansion
)GEKKO",
R"GEKKO(  std::string vtxXfrAttrsPre;
  std::string vtxXfrAttrs;
  size_t vtxOutIdx = 0;
  const bool zTextureEnabled = config.zTexOp != GX_ZT_DISABLE && config.tevStageCount != 0 &&
                               config.tevStages[config.tevStageCount - 1].texMapId != GX_TEXMAP_NULL &&
                               config.tevStages[config.tevStageCount - 1].texCoordId != GX_TEXCOORD_NULL;
  if (zTextureEnabled) {
    uniBufAttrs += "\n    ztex_bias: vec4u,";
  }

  // Load points for line/point expansion
)GEKKO", "Aurora Z-texture shader state");

  ReplaceOnceInFile(aurora_shader,
R"GEKKO(    const bool needsTextureSample = uses_texture_sample(stage);
    if (!needsTevTexCoord && !needsTextureSample) {
)GEKKO",
R"GEKKO(    const bool zTextureSource = zTextureEnabled && i + 1 == config.tevStageCount;
    const bool needsTextureSample = uses_texture_sample(stage) || zTextureSource;
    if (!needsTevTexCoord && !needsTextureSample) {
)GEKKO", "Aurora Z-texture last-TEV sample");

  ReplaceOnceInFile(aurora_shader,
R"GEKKO(  auto shaderSource =
)GEKKO",
R"GEKKO(  // GekkoAOT: compose Z-texture fragment depth with Aurora's current
  // normal-attachment and dual-source destination-alpha output paths.
  if (zTextureEnabled) {
    const u32 lastStageIdx = config.tevStageCount - 1;
    std::string zTexValue;
    switch (config.zTexType) {
    case 0:
      zTexValue = fmt::format("u32(round(clamp(sampled{0}.a, 0.0, 1.0) * 255.0))", lastStageIdx);
      break;
    case 1:
      zTexValue = fmt::format(
          "(u32(round(clamp(sampled{0}.r, 0.0, 1.0) * 255.0)) | "
          "(u32(round(clamp(sampled{0}.a, 0.0, 1.0) * 255.0)) << 8u))", lastStageIdx);
      break;
    case 2:
    default:
      zTexValue = fmt::format(
          "((u32(round(clamp(sampled{0}.r, 0.0, 1.0) * 255.0)) << 16u) | "
          "(u32(round(clamp(sampled{0}.g, 0.0, 1.0) * 255.0)) << 8u) | "
          "u32(round(clamp(sampled{0}.b, 0.0, 1.0) * 255.0)))", lastStageIdx);
      break;
    }
    const std::string currentGuestDepth = UseReversedZ ? "(1.0 - in.pos.z)" : "in.pos.z";
    const std::string addCurrent = config.zTexOp == GX_ZT_ADD
                                       ? fmt::format(" + u32(round(clamp({}, 0.0, 1.0) * 16777215.0))", currentGuestDepth)
                                       : std::string{};
    const std::string hostDepth = UseReversedZ ? "1.0 - (f32(gx_z) / 16777215.0)"
                                                : "f32(gx_z) / 16777215.0";
    const std::string zDepthCode = fmt::format(
        "\n    let gx_z = ({} + ubuf.ztex_bias.x{}) & 0x00FFFFFFu;"
        "\n    let gx_depth = clamp({}, 0.0, 1.0);",
        zTexValue, addCurrent, hostDepth);

    if (dstAlphaMode == DstAlphaMode::DualSource) {
      fragmentOutput =
          "\nstruct FragmentOutput {\n"
          "    @location(0) @blend_src(0) color: vec4f,\n"
          "    @location(0) @blend_src(1) blend: vec4f,\n"
          "    @builtin(frag_depth) depth: f32,\n"
          "};\n";
      fragmentOutputType = "FragmentOutput"sv;
      fragmentReturn = zDepthCode +
          "\n    var out: FragmentOutput;"
          "\n    out.color = vec4f(prev.rgb, 1.0);"
          "\n    out.blend = prev;"
          "\n    out.depth = gx_depth;"
          "\n    return out;";
    } else if (normalAttachment != UINT32_MAX) {
      fragmentOutput = fmt::format(
          "\nstruct FragmentOutput {{\n"
          "    @location(0) color: vec4f,\n"
          "    @location({}) normal: vec4f,\n"
          "    @builtin(frag_depth) depth: f32,\n"
          "}};\n",
          normalAttachment);
      fragmentOutputType = "FragmentOutput"sv;
      fragmentReturn = zDepthCode + "\n    var out: FragmentOutput;\n    out.color = prev;";
      if (useNormalTarget) {
        fragmentReturn +=
            "\n    let nrm_len_sq = dot(in.mv_nrm, in.mv_nrm);"
            "\n    let unit_nrm = select(vec3f(0.0), normalize(in.mv_nrm), nrm_len_sq > 1e-10);"
            "\n    out.normal = vec4f(unit_nrm * 0.5 + 0.5, select(0.0, 1.0, nrm_len_sq > 1e-10));";
      } else {
        fragmentReturn += "\n    out.normal = vec4f(0.5, 0.5, 0.5, 0.0);";
      }
      fragmentReturn += "\n    out.depth = gx_depth;\n    return out;";
    } else {
      fragmentOutput =
          "\nstruct FragmentOutput {\n"
          "    @location(0) color: vec4f,\n"
          "    @builtin(frag_depth) depth: f32,\n"
          "};\n";
      fragmentOutputType = "FragmentOutput"sv;
      fragmentReturn = zDepthCode +
          "\n    var out: FragmentOutput;"
          "\n    out.color = prev;"
          "\n    out.depth = gx_depth;"
          "\n    return out;";
    }
  }

  auto shaderSource =
)GEKKO", "Aurora Z-texture fragment output latest");

  const auto aurora_texture_convert = p.aurora_src / "lib/gfx/texture_convert.cpp";
  ReplaceOnceInFile(aurora_texture_convert,
R"GEKKO(  case GX_TF_I8:
    converted = DecodeTiled<TextureDecoderI8>(width, height, mips, data);
)GEKKO",
R"GEKKO(  case GX_TF_I8:
  case GX_TF_Z8:
    converted = DecodeTiled<TextureDecoderI8>(width, height, mips, data);
)GEKKO", "Aurora Z8 load conversion");
  ReplaceOnceInFile(aurora_texture_convert,
R"GEKKO(  case GX_TF_IA8:
    converted = DecodeTiled<TextureDecoderIA8>(width, height, mips, data);
)GEKKO",
R"GEKKO(  case GX_TF_IA8:
  case GX_TF_Z16:
    converted = DecodeTiled<TextureDecoderIA8>(width, height, mips, data);
)GEKKO", "Aurora Z16 load conversion");
  ReplaceOnceInFile(aurora_texture_convert,
R"GEKKO(  case GX_TF_RGBA8:
    converted = BuildRGBA8FromGCN(width, height, mips, data);
)GEKKO",
R"GEKKO(  case GX_TF_RGBA8:
  case GX_TF_Z24X8:
    converted = BuildRGBA8FromGCN(width, height, mips, data);
)GEKKO", "Aurora Z24X8 load conversion");

  // v35: GameCube I4/I8 textures are one-channel intensity sources.  Aurora's
  // historical CPU-load path detiled them into RGBA8, multiplying conversion
  // and upload traffic by four.  On WebGPU implementations with component
  // swizzle, detile into R8 and expose R as RGBA in the view.  EFB-copy
  // conversion remains untouched because it uses new_conv_texture().
  ReplaceOnceInFile(aurora_texture_convert,
R"GEKKO(struct TextureDecoderI8 {
  using Source = uint8_t;
  using Target = RGBA8;

  static constexpr uint32_t Frac = 1;
  static constexpr uint32_t BlockWidth = 8;
  static constexpr uint32_t BlockHeight = 4;

  static void decode_texel(Target* target, const Source* in, const uint32_t x) {
    const uint8_t intensity = in[x];
    target[x].r = intensity;
    target[x].g = intensity;
    target[x].b = intensity;
    target[x].a = intensity;
  }
};
)GEKKO",
R"GEKKO(struct TextureDecoderI8 {
  using Source = uint8_t;
  using Target = RGBA8;

  static constexpr uint32_t Frac = 1;
  static constexpr uint32_t BlockWidth = 8;
  static constexpr uint32_t BlockHeight = 4;

  static void decode_texel(Target* target, const Source* in, const uint32_t x) {
    const uint8_t intensity = in[x];
    target[x].r = intensity;
    target[x].g = intensity;
    target[x].b = intensity;
    target[x].a = intensity;
  }
};

struct TextureDecoderI8R8 {
  using Source = uint8_t;
  using Target = uint8_t;
  static constexpr uint32_t Frac = 1;
  static constexpr uint32_t BlockWidth = 8;
  static constexpr uint32_t BlockHeight = 4;
  static void decode_texel(Target* target, const Source* in, const uint32_t x) { target[x] = in[x]; }
};

struct TextureDecoderI4R8 {
  using Source = uint8_t;
  using Target = uint8_t;
  static constexpr uint32_t Frac = 2;
  static constexpr uint32_t BlockWidth = 8;
  static constexpr uint32_t BlockHeight = 8;
  static void decode_texel(Target* target, const Source* in, const uint32_t x) {
    target[x] = ExpandTo8<4>(in[x / 2] >> (x & 1 ? 0 : 4) & 0xf);
  }
};
)GEKKO", "Aurora native R8 intensity decoders v35");

  ReplaceOnceInFile(aurora_texture_convert,
R"GEKKO(  case GX_TF_I4:
    converted = DecodeTiled<TextureDecoderI4>(width, height, mips, data);
    break;
  case GX_TF_I8:
  case GX_TF_Z8:
    converted = DecodeTiled<TextureDecoderI8>(width, height, mips, data);
    break;
)GEKKO",
R"GEKKO(  case GX_TF_I4:
    converted = webgpu::g_textureComponentSwizzleSupported
                    ? DecodeTiled<TextureDecoderI4R8>(width, height, mips, data)
                    : DecodeTiled<TextureDecoderI4>(width, height, mips, data);
    break;
  case GX_TF_I8:
    converted = webgpu::g_textureComponentSwizzleSupported
                    ? DecodeTiled<TextureDecoderI8R8>(width, height, mips, data)
                    : DecodeTiled<TextureDecoderI8>(width, height, mips, data);
    break;
  case GX_TF_Z8:
    converted = DecodeTiled<TextureDecoderI8>(width, height, mips, data);
    break;
)GEKKO", "Aurora CPU-loaded I4/I8 R8 conversion v35");

  ReplaceOnceInFile(aurora_texture_convert,
R"GEKKO(  const auto wgpuFormat = to_wgpu(format);
)GEKKO",
R"GEKKO(  const auto wgpuFormat =
      ((format == GX_TF_I4 || format == GX_TF_I8) && webgpu::g_textureComponentSwizzleSupported)
          ? wgpu::TextureFormat::R8Unorm
          : to_wgpu(format);
)GEKKO", "Aurora intensity converted physical format v35");

  const auto aurora_gfx_texture_v35 = p.aurora_src / "lib/gfx/texture.cpp";
  // v35c: texture.cpp contains more than one texture-allocation helper with
  // the same `to_wgpu(gxFormat)` prefix (new_dynamic_texture_2d and
  // new_conv_texture).  Anchor on the complete dynamic-texture function
  // prefix so the R8 optimization cannot accidentally patch the conversion
  // render-target path or fail merely because Aurora gained another helper.
  ReplaceOnceInFile(aurora_gfx_texture_v35,
R"GEKKO(TextureHandle new_dynamic_texture_2d(uint32_t width, uint32_t height, uint32_t mips, u32 gxFormat,
                                     const char* label) noexcept {
  ZoneScopedS(3);
  const auto wgpuFormat = to_wgpu(gxFormat);
  const wgpu::Extent3D size{
)GEKKO",
R"GEKKO(TextureHandle new_dynamic_texture_2d(uint32_t width, uint32_t height, uint32_t mips, u32 gxFormat,
                                     const char* label) noexcept {
  ZoneScopedS(3);
  const u32 physicalGxFormat =
      ((gxFormat == GX_TF_I4 || gxFormat == GX_TF_I8) && webgpu::g_textureComponentSwizzleSupported)
          ? GX_TF_R8_PC
          : gxFormat;
  const auto wgpuFormat = to_wgpu(physicalGxFormat);
  const wgpu::Extent3D size{
)GEKKO", "Aurora intensity R8 allocation v35c");
  ReplaceOnceInFile(aurora_gfx_texture_v35,
R"GEKKO(  wgpu::TextureComponentSwizzleDescriptor swizzle;
  if (setup_swizzle(swizzle, gxFormat)) {
    textureViewDescriptor.nextInChain = &swizzle;
  }
)GEKKO",
R"GEKKO(  wgpu::TextureComponentSwizzleDescriptor swizzle;
  if (setup_swizzle(swizzle, physicalGxFormat)) {
    textureViewDescriptor.nextInChain = &swizzle;
  }
)GEKKO", "Aurora intensity R8 view swizzle v35c");

  std::cout << "GEKKOAOT_AURORA_INTENSITY_R8_V35C=1 "
               "formats=i4+i8 physical=r8 swizzle=runtime-feature fallback=rgba8 efb-copy=unchanged\n";

  std::cout << "GEKKOAOT_AURORA_ZTEXTURE_V1D=1 "
               "bp=f4+f5 source=last-tev formats=z8+z16+z24x8 "
               "ops=add+replace depth=fragment-output apply=aurora-latest-composed-output\n";
  std::cout << "GEKKOAOT_AURORA_ZTEXTURE_WGSL_NEWLINE_V127G=1 escape=real-newline parser=wgsl-safe\n";

  // MOH Frontline / generic Flipper correctness: Aurora 9c0bf66f is the direct
  // parent of upstream 3840bf9 ("Fix GX EFB alpha handling"). Keep the pinned
  // renderer, but backport the core no-alpha EFB semantics without pulling the
  // newer copy/presentation implementation into GekkoAOT's custom XFB path.
  const auto aurora_efb_alpha_h = p.aurora_src / "lib/gx/gx.hpp";
  ReplaceOnceInFile(aurora_efb_alpha_h,
R"GEKKO(u8 comp_type_size(GXAttr attr, GXCompType type) noexcept;
u8 comp_cnt_count(GXAttr attr, GXCompCnt cnt) noexcept;
} // namespace aurora::gx
)GEKKO",
R"GEKKO(u8 comp_type_size(GXAttr attr, GXCompType type) noexcept;
u8 comp_cnt_count(GXAttr attr, GXCompCnt cnt) noexcept;

// Flipper only exposes destination alpha in RGBA6_Z24. RGB8_Z24 and
// RGB565_Z16 behave as if destination alpha is constantly one.
constexpr bool efb_has_alpha(GXPixelFmt format) noexcept { return format == GX_PF_RGBA6_Z24; }
} // namespace aurora::gx
)GEKKO", "Aurora EFB alpha capability v175");

  const auto aurora_efb_alpha_gx = p.aurora_src / "lib/gx/gx.cpp";
  ReplaceOnceInFile(aurora_efb_alpha_gx,
R"GEKKO(  const auto [polygonOffset, polygonOffsetScale] = polygon_offset_for_cull_mode(cullMode);
  config = {
)GEKKO",
R"GEKKO(  const auto [polygonOffset, polygonOffsetScale] = polygon_offset_for_cull_mode(cullMode);
  const bool hasAlpha = efb_has_alpha(g_gxState.pixelFmt);
  const bool alphaUpdate = hasAlpha && g_gxState.alphaUpdate;
  const auto blendFactor = [hasAlpha](GXBlendFactor factor) {
    if (!hasAlpha) {
      if (factor == GX_BL_DSTALPHA) return GX_BL_ONE;
      if (factor == GX_BL_INVDSTALPHA) return GX_BL_ZERO;
    }
    return factor;
  };
  config = {
)GEKKO", "Aurora no-alpha EFB blend semantics v175");
  ReplaceOnceInFile(aurora_efb_alpha_gx,
R"GEKKO(      .blendFacSrc = g_gxState.blendFacSrc,
      .blendFacDst = g_gxState.blendFacDst,
)GEKKO",
R"GEKKO(      .blendFacSrc = blendFactor(g_gxState.blendFacSrc),
      .blendFacDst = blendFactor(g_gxState.blendFacDst),
)GEKKO", "Aurora destination-alpha blend factors v175");
  ReplaceOnceInFile(aurora_efb_alpha_gx,
R"GEKKO(      .dstAlpha = g_gxState.dstAlpha,
)GEKKO",
R"GEKKO(      .dstAlpha = alphaUpdate ? g_gxState.dstAlpha : UINT32_MAX,
)GEKKO", "Aurora GXSetDstAlpha EFB capability v175");
  ReplaceOnceInFile(aurora_efb_alpha_gx,
R"GEKKO(      .alphaUpdate = g_gxState.alphaUpdate,
)GEKKO",
R"GEKKO(      .alphaUpdate = alphaUpdate,
)GEKKO", "Aurora alpha-update EFB capability v175");

  const auto aurora_efb_alpha_regs = p.aurora_src / "lib/gx/regs.cpp";
  ReplaceOnceInFile(aurora_efb_alpha_regs,
R"GEKKO(  regs[0x43] = {bp_pe_ctrl};
)GEKKO",
R"GEKKO(  regs[0x43] = {bp_pe_ctrl, DirtyPipeline};
)GEKKO", "Aurora PE-control pipeline dirty v175");

  const auto aurora_efb_alpha_fb = p.aurora_src / "lib/dolphin/gx/GXFrameBuffer.cpp";
  ReplaceOnceInFile(aurora_efb_alpha_fb,
R"GEKKO(  const auto clearAlpha = clear && g_gxState.alphaUpdate;
)GEKKO",
R"GEKKO(  const auto clearAlpha = clear && g_gxState.alphaUpdate && efb_has_alpha(g_gxState.pixelFmt);
)GEKKO", "Aurora no-alpha EFB clear semantics v175");

  std::cout << "GEKKOAOT_AURORA_EFB_ALPHA_SEMANTICS_V175=1 "
               "rgb8+rgb565=no-alpha dstalpha=one invdstalpha=zero "
               "alpha-update=masked bp43-dirty=pipeline upstream=3840bf9-core\n";

  // v177: preserve the guest EFB across the host frame boundary that GekkoAOT
  // deliberately inserts at GXCopyDisp. Aurora's copy_tex() already applies
  // the exact guest clear masks (color/alpha/depth) before the frame is
  // submitted. A fresh Aurora recording normally starts with a full EFB clear,
  // which destroys channel-masked clears used by retail engines. MOH Frontline
  // does this before its weapon render bin; Terminator 3's vkWindow::clear uses
  // the same GXCopyDisp(clear=1) mechanism for arbitrary clear-bit masks.
  const auto aurora_efb_continuity_hpp = p.aurora_src / "lib/gfx/recording.hpp";
  ReplaceOnceInFile(aurora_efb_continuity_hpp,
R"GEKKO(RenderTargetLayout get_render_target_layout() noexcept;
void clear_caches() noexcept;
)GEKKO",
R"GEKKO(RenderTargetLayout get_render_target_layout() noexcept;
void clear_caches() noexcept;
// GekkoAOT host-boundary hook: the current frame already produced the exact
// post-GXCopyDisp EFB state, so the next recording must load it rather than
// applying Aurora's normal first-pass clear.
void gekkoaot_preserve_efb_on_next_begin(bool preserveColor, bool preserveDepth) noexcept;
)GEKKO", "Aurora EFB continuity declaration v177");

  const auto aurora_efb_continuity_cpp = p.aurora_src / "lib/gfx/recording.cpp";
  ReplaceOnceInFile(aurora_efb_continuity_cpp,
R"GEKKO(FrameRecorder g_recorder;
)GEKKO",
R"GEKKO(FrameRecorder g_recorder;
// One-shot request consumed by detail::begin_recording(). This remains on the
// CPU recording thread; the render worker never reads or writes it.
uint32_t g_gekkoaotPreserveEfbOnNextBegin = 0u;
)GEKKO", "Aurora EFB continuity state v177");

  ReplaceOnceInFile(aurora_efb_continuity_cpp,
R"GEKKO(} // namespace

namespace detail {

void begin_recording(FramePacket& packet, size_t frameSlot) {
)GEKKO",
R"GEKKO(} // namespace

void gekkoaot_preserve_efb_on_next_begin(bool preserveColor, bool preserveDepth) noexcept {
  g_gekkoaotPreserveEfbOnNextBegin =
      (preserveColor ? 1u : 0u) | (preserveDepth ? 2u : 0u);
}

namespace detail {

void begin_recording(FramePacket& packet, size_t frameSlot) {
)GEKKO", "Aurora EFB continuity hook v177");

  ReplaceOnceInFile(aurora_efb_continuity_cpp,
R"GEKKO(  pass.label = pass_label("EFB");
  set_efb_targets(pass);
  pass.colorAttachments[SceneColorAttachmentIndex].clearValue = gx::g_gxState.clearColor;
  pass.clearDepthValue = gx::clear_depth_value();
  g_recorder.currentRenderPass = 0;
)GEKKO",
R"GEKKO(  pass.label = pass_label("EFB");
  set_efb_targets(pass);
  const uint32_t preserveEfb =
      std::exchange(g_gekkoaotPreserveEfbOnNextBegin, 0u);
  if ((preserveEfb & 1u) != 0u) {
    // Preserve the post-copy color attachment only when the guest clear left
    // color/alpha data alive.  This avoids carrying stale color through full
    // clears while retaining channel-masked copy semantics.
    for (uint32_t i = 0; i < pass.colorAttachmentCount; ++i) {
      pass.colorAttachments[i].clear = false;
      pass.colorAttachments[i].loadOp = wgpu::LoadOp::Load;
    }
  } else {
    pass.colorAttachments[SceneColorAttachmentIndex].clearValue =
        gx::g_gxState.clearColor;
  }

  if ((preserveEfb & 2u) != 0u) {
    // Depth is loaded only when the guest did not clear it.  A depth-only
    // GXCopyDisp therefore starts the next recording with a normal depth clear
    // instead of loading the previous depth attachment.
    pass.clearDepth = false;
    pass.depthLoadOp = wgpu::LoadOp::Load;
  } else {
    pass.clearDepthValue = gx::clear_depth_value();
  }
  g_recorder.currentRenderPass = 0;
)GEKKO", "Aurora EFB begin-recording load v177");

  ReplaceOnceInFile(aurora_efb_continuity_cpp,
R"GEKKO(  g_recorder.suppressRenderWorker = false;
  g_recorder.normalRequested = false;
}
)GEKKO",
R"GEKKO(  g_recorder.suppressRenderWorker = false;
  g_recorder.normalRequested = false;
  g_gekkoaotPreserveEfbOnNextBegin = 0u;
}
)GEKKO", "Aurora EFB continuity shutdown reset v177");

  std::cout << "GEKKOAOT_AURORA_EFB_CONTINUITY_SELECTIVE_V1=1 "
               "gxcopydisp=submit next-recording=attachment-selective-load "
               "clear-semantics=copy-tex-pe-masks evidence=moh2-renderbin+t3-vkwindow-clear "
               "hpcos=bounded-frame\\n";

  const auto aurora_draw_range_diag_patch =
      p.root / "patches/aurora/gekkoaot-draw-range-diagnostics-v1.patch";
  if (!fs::exists(aurora_draw_range_diag_patch))
    throw std::runtime_error("missing Aurora draw-range diagnostics patch: "+
                             aurora_draw_range_diag_patch.string());

  // v159b: diagnostics must never block the composed Aurora build. Earlier
  // renderer patches legitimately rewrite command_processor.cpp, so this
  // optional patch may lose its original context.
  bool aurora_draw_range_diag_enabled = false;
  if (Run("git -C "+Quote(p.aurora_src)+" apply --recount --check "+
          Quote(aurora_draw_range_diag_patch)) == 0) {
    Require(Run("git -C "+Quote(p.aurora_src)+" apply --recount "+
                Quote(aurora_draw_range_diag_patch)),
            "Aurora draw-range diagnostics patch");
    aurora_draw_range_diag_enabled = true;
  } else if (Run("git -C "+Quote(p.aurora_src)+" apply --recount --reverse --check "+
                 Quote(aurora_draw_range_diag_patch)) == 0) {
    aurora_draw_range_diag_enabled = true;
  } else {
    std::cerr
        << "[gekkoaot] warning: skipping stale Aurora draw-range diagnostics "
           "patch (diagnostics-only; renderer unchanged)\n";
  }
  std::cout << "GEKKOAOT_AURORA_DRAW_RANGE_DIAGNOSTICS_V1="
            << (aurora_draw_range_diag_enabled ? "1" : "0")
            << " env=GEKKOAOT_GX_DIAG_DRAW_RANGES"
            << (aurora_draw_range_diag_enabled ? "" : " reason=context-drift")
            << "\n";

  // v163: Aurora upstream already has an asynchronous CPU depth-peek snapshot
  // for GXPeekZ. Add the matching color snapshot so the GameCube CPU-visible
  // EFB color aperture can stay native too (Sunshine uses GXPeekARGB). The
  // bridge reads the newest completed logical-frame snapshot and requests the
  // next one, avoiding a synchronous GPU readback on every guest load.
  const auto aurora_color_peek_patch =
      p.root / "patches/aurora/gekkoaot-color-peek-v163.patch";
  if (!fs::exists(aurora_color_peek_patch))
    throw std::runtime_error("missing Aurora EFB color-peek patch: "+
                             aurora_color_peek_patch.string());
  Require(Run("git -C "+Quote(p.aurora_src)+" apply --unidiff-zero --recount --check "+
              Quote(aurora_color_peek_patch)),
          "Aurora EFB color-peek patch check");
  Require(Run("git -C "+Quote(p.aurora_src)+" apply --unidiff-zero --recount "+
              Quote(aurora_color_peek_patch)),
          "Aurora EFB color-peek patch");
  std::cout << "GEKKOAOT_AURORA_EFB_COLOR_PEEK_V163=1 policy=async-logical-frame-snapshot format=argb8\n";

  // v133: preserve the complete modern Aurora renderer and change only the
  // selection of its new GXSetDstAlpha dual-source path.  Aurora already has
  // a correctness fallback for hardware without dual-source blending: an
  // alpha-only prepass followed by the normal color pass.  Default to that
  // fallback on GekkoAOT while retaining an env-controlled A/B switch.
  const auto aurora_pipeline = p.aurora_src / "lib/gx/pipeline.cpp";
  ReplaceOnceInFile(aurora_pipeline,
R"GEKKO(#include <tracy/Tracy.hpp>

namespace aurora::gx {
)GEKKO",
R"GEKKO(#include <tracy/Tracy.hpp>

#include <atomic>
#include <cstdio>
#include <cstdlib>

namespace aurora::gx {
namespace {
bool gekkoaot_allow_dual_source() noexcept {
  const char* value = std::getenv("GEKKOAOT_AURORA_DUAL_SOURCE");
  return value != nullptr && value[0] == '1' && value[1] == '\0';
}

std::atomic_bool g_gekkoaotDualSourceLog{false};
} // namespace
)GEKKO", "Aurora dual-source diagnostics helpers");
  ReplaceOnceInFile(aurora_pipeline,
R"GEKKO(    } else if (webgpu::g_dualSourceBlendingSupported && layout.colorAttachmentCount == 1) {
      options.dstAlphaMode = DstAlphaMode::DualSource;
    } else {
      // Write alpha before RGB, no depth write
)GEKKO",
R"GEKKO(    } else if (webgpu::g_dualSourceBlendingSupported && gekkoaot_allow_dual_source() &&
               layout.colorAttachmentCount == 1) {
      if (!g_gekkoaotDualSourceLog.exchange(true, std::memory_order_relaxed)) {
        std::fprintf(stderr,
                     "GEKKOAOT_AURORA_DUAL_SOURCE_V133=1 hit=1 path=dual-source\n");
      }
      options.dstAlphaMode = DstAlphaMode::DualSource;
    } else {
      if (webgpu::g_dualSourceBlendingSupported && layout.colorAttachmentCount == 1 &&
          !g_gekkoaotDualSourceLog.exchange(true, std::memory_order_relaxed)) {
        std::fprintf(stderr,
                     "GEKKOAOT_AURORA_DUAL_SOURCE_V133=0 hit=1 path=alpha-prepass\n");
      }
      // Write alpha before RGB, no depth write
)GEKKO", "Aurora dual-source GXSetDstAlpha selector");
  std::cout << "GEKKOAOT_AURORA_DUAL_SOURCE_GUARD_V133=1 "
               "default=alpha-prepass optin=GEKKOAOT_AURORA_DUAL_SOURCE=1 "
               "pin=9c0bf66f renderer=unchanged\n";

  // Aurora 9c0bf66f already computes GX raster lighting from channel controls,
  // material/ambient sources and the active light mask before TEV.  A blanket
  // post-TEV RGB boost modifies textured and alpha/EFB passes as well, even if
  // the shader also samples a lit raster channel.  Keep upstream's lighting and
  // destination-alpha outputs intact instead of changing the final TEV color.
  std::cout << "GEKKOAOT_AURORA_LIGHT_V128=1 policy=upstream-gx-channel-lighting "
               "post-tev-boost=off shadow=aurora-9c0bf66f-native\n";
  std::cout << "GEKKOAOT_AURORA_V131_BISECT=1 pin=9c0bf66f core-3840-efb-alpha-semantics=1 modern-renderer=1\n";
  const auto logic_patch = p.root / "patches/aurora/gekkoaot-logic-ops-v2.patch";
  if (!fs::exists(logic_patch))
    throw std::runtime_error("missing Aurora logic operations patch: " + logic_patch.string());
  Require(Run("git -C " + Quote(p.aurora_src) + " apply --check " + Quote(logic_patch)),
          "Aurora logic operations patch does not match the composed renderer");
  Require(Run("git -C " + Quote(p.aurora_src) + " apply " + Quote(logic_patch)),
          "Aurora logic operations patch");
  std::cout << "GEKKOAOT_AURORA_LOGIC_OPS_V2=1 mode=integer-framebuffer-logic\n";

  const auto provenance_patch = p.root / "patches/aurora/gekkoaot-copy-provenance-v2.patch";
  bool provenance_patch_enabled = false;
  if (Run("git -C " + Quote(p.aurora_src) + " apply --check " + Quote(provenance_patch)) == 0) {
    Require(Run("git -C " + Quote(p.aurora_src) + " apply " + Quote(provenance_patch)),
            "Aurora copy provenance patch");
    provenance_patch_enabled = true;
  } else if (Run("git -C " + Quote(p.aurora_src) + " apply --reverse --check " + Quote(provenance_patch)) == 0) {
    provenance_patch_enabled = true;
  } else {
    std::cerr << "[gekkoaot] warning: skipping stale Aurora copy-provenance patch; "
                 "composed renderer source has moved past its original context\n";
  }
  std::cout << "GEKKOAOT_AURORA_COPY_PROVENANCE_V2=" << (provenance_patch_enabled ? "1" : "0")
            << (provenance_patch_enabled ? "" : " reason=context-drift") << "\n";
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
  // Secondary REL/DOL/apploader modules use LLVM even when main.dol uses C.
  // Selecting the C main backend must not remove that compiler capability.
  std::string cfg="cmake -S "+Quote(p.dolrecomp_src)+" -B "+Quote(b)+" -G Ninja -DCMAKE_BUILD_TYPE=Release -DDOLRECOMP_ENABLE_LLVM=ON";
  const auto llvm=Env("GEKKOAOT_LLVM_DIR");
  if(!llvm.empty()) cfg += " -DLLVM_DIR="+Quote(fs::path(llvm));
  Require(Run(cfg),"DolRecomp configure");
  Require(Run("cmake --build "+Quote(b)+" --target dolrecomp -j"+std::to_string(Jobs())),
          "DolRecomp build");

#ifdef _WIN32
  // Ninja is a single-config generator, so the executable is emitted directly
  // into the build root as dolrecomp.exe. Keep Release/ as a fallback for
  // multi-config/stale build trees, then search recursively as a last resort.
  fs::path exe=b/"dolrecomp.exe";
  if(!fs::exists(exe)) exe=b/"Release/dolrecomp.exe";
  if(!fs::exists(exe)) {
    const auto found=FindFileRecursive(b,"dolrecomp.exe");
    if(!found.empty()) exe=found;
  }
#else
  fs::path exe=b/"dolrecomp";
  if(!fs::exists(exe)) {
    const auto found=FindFileRecursive(b,"dolrecomp");
    if(!found.empty()) exe=found;
  }
#endif

  if(!fs::exists(exe))
    throw std::runtime_error("dolrecomp executable not found under "+b.string());

  std::cout<<"GEKKOAOT_DOLRECOMP_BUILT_V1=1 path=\""<<exe.string()
           <<"\" backend="<<backend<<"\n";
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
  // CMake installs additional source overlays. Compare only after those have
  // completed; restoring before configuration would still rebuild every file
  // touched by the idempotent window/XFB/widescreen overlays.
  if (p.aurora_source_times) {
    std::cout << "GEKKOAOT_SOURCE_REUSE backend=aurora unchanged="
              << p.aurora_source_times->RestoreUnchanged() << '\n';
    p.aurora_source_times.reset();
  }
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
  // v4: caches written before FST subdirectories were walked lack nested RELs
  // (Hide & Sneak files/mickey2/*.rel), so they are refreshed once.
  if(!fs::exists(p.disc/"sys/main.dol")||!fs::exists(p.disc/"meta/disc-id.txt")||
     ReadLine(p.disc/"meta/exec-cache-policy.txt")!="v5-embedded-rel")
    Require(RunProcess(p.native_disc,std::vector<fs::path>{image,p.disc}),"nod disc extraction");
  p.game_id=ReadLine(p.disc/"meta/disc-id.txt");
  if(p.game_id.size()!=6)throw std::runtime_error("invalid GameCube disc id in nod cache");
  std::cout<<"GEKKOAOT_PROGRESS=30|Disc executable cache ready\n";
}
enum class PgoMode : std::uint8_t { Off, Generate, Use };

struct CompilePlan {
  PgoMode pgo = PgoMode::Off;
  fs::path profile_path;
  fs::path adaptive_hot_manifest;
  fs::path crossgame_hot_manifest;
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
  // PGO augments the ordinary optimized build. Training and use must begin
  // with the same partitioning, guards, ABI and scalar lowering as that build.
  CompilePlan plan=NormalCompilePlan();
  plan.pgo=mode;
  plan.profile_path=profile;
  plan.chunk_instructions=EnvUnsigned("GEKKOAOT_PGO_CHUNK_INSTRUCTIONS",plan.chunk_instructions,64u,8192u);
  plan.guard_cycles=EnvUnsigned("GEKKOAOT_PGO_GUARD_CYCLES",plan.guard_cycles,256u,1048576u);
  plan.guard_steps=EnvUnsigned("GEKKOAOT_PGO_GUARD_STEPS",plan.guard_steps,2048u,1048576u);
  plan.thinlto=mode==PgoMode::Use && Env("GEKKOAOT_PGO_THINLTO","1")!="0";
  return plan;
}

fs::path MatchCrossGamePrior(Pipeline& p,unsigned chunk);

std::uint32_t DolReadBE32(const std::vector<unsigned char>& bytes,std::size_t offset) {
  if(offset+4u>bytes.size()) return 0u;
  return (std::uint32_t(bytes[offset])<<24u)|(std::uint32_t(bytes[offset+1u])<<16u)|
         (std::uint32_t(bytes[offset+2u])<<8u)|std::uint32_t(bytes[offset+3u]);
}

fs::path BuildRelToDolEntrypointManifest(const fs::path& secondary_tool,
                                      const fs::path& disc,const fs::path& out) {
  const auto manifest=out/"rel-to-dol-v206.txt";
  fs::create_directories(out);
  Require(RunProcess(secondary_tool,
      {fs::path("--root"),disc,fs::path("--output"),out,
       fs::path("--entrypoints-manifest"),manifest}),
      "REL -> DOL entrypoint pre-scan");
  if(!fs::exists(manifest))
    throw std::runtime_error("REL -> DOL entrypoint pre-scan did not produce "+manifest.string());
  return manifest;
}

fs::path BuildRelToDolEntrypointManifest(Pipeline& p) {
  return BuildRelToDolEntrypointManifest(p.secondary_tool,p.disc,
                                       p.cache/"entrypoints"/p.game_id);
}

fs::path BuildStaticDolCodePointerMap(Pipeline& p) {
  const auto dol=p.disc/"sys/main.dol";
  std::ifstream in(dol,std::ios::binary);
  if(!in) throw std::runtime_error("cannot read DOL for static code-pointer scan: "+dol.string());
  const std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(in)),{});
  if(bytes.size()<0x100u) throw std::runtime_error("DOL is too small for static code-pointer scan");

  struct Range { std::uint32_t begin=0,end=0; };
  std::vector<Range> text;
  for(unsigned i=0;i<7u;++i) {
    const auto address=DolReadBE32(bytes,0x48u+i*4u);
    const auto size=DolReadBE32(bytes,0x90u+i*4u);
    if(size && address+size>=address) text.push_back({address,address+size});
  }
  const auto is_code=[&](std::uint32_t address) {
    if((address&3u)!=0u) return false;
    return std::any_of(text.begin(),text.end(),[&](const Range& r){
      return address>=r.begin && address<r.end;
    });
  };

  // Direct B/BC targets are already discovered by DolRecomp.  Keep the MAP
  // focused on data-published callbacks/jump-table labels so it does not
  // unnecessarily fragment ordinary hot code.
  std::vector<std::uint32_t> direct;
  for(unsigned i=0;i<7u;++i) {
    const auto file=DolReadBE32(bytes,i*4u);
    const auto address=DolReadBE32(bytes,0x48u+i*4u);
    const auto size=DolReadBE32(bytes,0x90u+i*4u);
    if(!size || std::uint64_t(file)+size>bytes.size()) continue;
    for(std::uint32_t off=0;off+4u<=size;off+=4u) {
      const auto raw=DolReadBE32(bytes,std::size_t(file)+off);
      const auto pc=address+off;
      const auto op=raw>>26u;
      std::uint32_t target=0u;
      bool valid=false;
      if(op==18u) {
        std::int32_t delta=static_cast<std::int32_t>(raw&0x03fffffcu);
        if(delta&0x02000000) delta-=0x04000000;
        target=((raw>>1u)&1u)?static_cast<std::uint32_t>(delta):pc+static_cast<std::uint32_t>(delta);
        valid=true;
      } else if(op==16u) {
        std::int32_t delta=static_cast<std::int32_t>(raw&0x0000fffcu);
        if(delta&0x00008000) delta-=0x00010000;
        target=((raw>>1u)&1u)?static_cast<std::uint32_t>(delta):pc+static_cast<std::uint32_t>(delta);
        valid=true;
      }
      if(valid && is_code(target)) direct.push_back(target);
    }
  }
  std::sort(direct.begin(),direct.end());
  direct.erase(std::unique(direct.begin(),direct.end()),direct.end());

  std::vector<std::uint32_t> pointers;
  for(unsigned i=0;i<11u;++i) {
    const auto file=DolReadBE32(bytes,0x1cu+i*4u);
    const auto size=DolReadBE32(bytes,0xacu+i*4u);
    if(!size || std::uint64_t(file)+size>bytes.size()) continue;
    for(std::uint32_t off=0;off+4u<=size;off+=4u) {
      const auto value=DolReadBE32(bytes,std::size_t(file)+off);
      if(is_code(value) && !std::binary_search(direct.begin(),direct.end(),value))
        pointers.push_back(value);
    }
  }
  const auto program_entry=DolReadBE32(bytes,0xe0u);
  if(is_code(program_entry)) pointers.push_back(program_entry);

  // REL imports against module 0 are calls/pointers back into main.dol. Feed
  // those targets through DolRecomp's existing symbol-map entry-point path so
  // LLVM creates an explicit native region leader at each cross-module target.
  // This is intentionally selective: unlike REL side modules, the main DOL
  // does not need all-instruction entry points.
  const auto rel_entrypoints=BuildRelToDolEntrypointManifest(p);
  std::size_t rel_targets=0;
  {
    std::ifstream rels(rel_entrypoints);
    std::string line;
    while(std::getline(rels,line)) {
      if(line.empty()) continue;
      try {
        std::size_t used=0;
        const auto value=std::stoul(line,&used,16);
        if(used==line.size() && value<=std::numeric_limits<std::uint32_t>::max()) {
          const auto address=static_cast<std::uint32_t>(value);
          if(is_code(address) && !std::binary_search(direct.begin(),direct.end(),address)) {
            pointers.push_back(address);
            rel_targets++;
          }
        }
      } catch(...) {}
    }
  }

  std::sort(pointers.begin(),pointers.end());
  pointers.erase(std::unique(pointers.begin(),pointers.end()),pointers.end());

  const auto path=p.cache/"entrypoints"/p.game_id/"dol-static+rel-code-pointers-v206.map";
  std::ostringstream map;
  map<<std::hex;
  for(const auto address:pointers)
    map<<address<<" static_ptr_"<<address<<'\n';
  WriteText(path,map.str());
  std::cout<<"GEKKOAOT_DOL_STATIC_ENTRYPOINTS_V206=1 game="<<p.game_id
           <<" pointers="<<pointers.size()<<" direct_known="<<direct.size()
           <<" rel_to_dol="<<rel_targets
           <<" map=\""<<path.string()
           <<"\" policy=dol-data-code-pointers+module0-relocations-minus-direct-branches\n";
  return path;
}


static bool V213Hex(char c) {
  return (c >= '0' && c <= '9') ||
         (c >= 'a' && c <= 'f') ||
         (c >= 'A' && c <= 'F');
}

static unsigned V213Nibble(char c) {
  if (c >= '0' && c <= '9') return unsigned(c - '0');
  if (c >= 'a' && c <= 'f') return unsigned(c - 'a') + 10u;
  return unsigned(c - 'A') + 10u;
}

/*
 * v213
 *
 * Every module-0 relocation emitted by a REL is an architectural entry into
 * the main DOL. It MUST be an LLVM partition/range boundary even when the same
 * address is also the target of a normal direct DOL branch.
 *
 * v206 currently removes those direct-known addresses from the synthetic map.
 * That is unsafe for cross-module entry because local SSA state from the
 * preceding DOL instructions is then allowed to describe an entry which was
 * actually reached from a REL.
 *
 * Keep every REL->DOL target in the map. DolRecomp itself filters symbols
 * outside executable DOL sections, so unrelated hexadecimal values are inert.
 */
static void EnsureRelToDolHardBoundariesV213(Pipeline& p) {
  const auto dir =
      p.cache / "entrypoints" / p.game_id;

  const auto manifest =
      dir / "rel-to-dol-v206.txt";

  const auto map =
      dir / "dol-static+rel-code-pointers-v206.map";

  if (!fs::exists(manifest) || !fs::exists(map))
    return;

  std::ifstream input(manifest);
  if (!input)
    throw std::runtime_error(
        "cannot read REL->DOL manifest: " + manifest.string());

  std::vector<std::uint32_t> candidates;
  std::string line;

  while (std::getline(input, line)) {
    for (std::size_t i = 0; i + 8 <= line.size(); ++i) {
      if (i != 0 && V213Hex(line[i - 1]))
        continue;

      bool valid = true;
      for (std::size_t n = 0; n < 8; ++n) {
        if (!V213Hex(line[i + n])) {
          valid = false;
          break;
        }
      }

      if (!valid)
        continue;

      if (i + 8 < line.size() && V213Hex(line[i + 8]))
        continue;

      std::uint32_t value = 0;

      for (std::size_t n = 0; n < 8; ++n)
        value = (value << 4) | V213Nibble(line[i + n]);

      /*
       * Keep MEM1-looking addresses. Values which belong to a REL rather than
       * the DOL are harmless because DolRecomp later checks llvm_code_address().
       */
      if (value >= 0x80000000u &&
          value < 0x81800000u)
        candidates.push_back(value);

      i += 7;
    }
  }

  std::sort(candidates.begin(), candidates.end());
  candidates.erase(
      std::unique(candidates.begin(), candidates.end()),
      candidates.end());

  std::ifstream current(map, std::ios::binary);
  if (!current)
    throw std::runtime_error(
        "cannot read v206 DOL map: " + map.string());

  std::string existing(
      (std::istreambuf_iterator<char>(current)),
      std::istreambuf_iterator<char>());

  std::string additions;
  std::size_t added = 0;

  for (const auto address : candidates) {
    char symbol[64];

    std::snprintf(
        symbol,
        sizeof(symbol),
        "gekkoaot_rel_to_dol_%08X",
        address);

    if (existing.find(symbol) != std::string::npos)
      continue;

    char record[128];

    std::snprintf(
        record,
        sizeof(record),
        "%08X 00000000 %s\n",
        address,
        symbol);

    additions += record;
    ++added;
  }

  if (!additions.empty()) {
    std::ofstream out(
        map,
        std::ios::binary | std::ios::app);

    if (!out)
      throw std::runtime_error(
          "cannot extend v206 DOL map: " + map.string());

    out << additions;

    if (!out)
      throw std::runtime_error(
          "failed writing v213 DOL map");
  }

  std::cout
      << "GEKKOAOT_REL_TO_DOL_HARD_BOUNDARY_V213=1"
      << " candidates=" << candidates.size()
      << " added=" << added
      << " policy=all-rel-dol-targets-are-llvm-boundaries"
      << " map=\"" << map.string() << "\"\n";
}

fs::path CompileModuleVariant(Pipeline& p,const fs::path& dolrecomp,std::string_view backend,
                              std::string_view policy_tag,const fs::path& static_intercepts,
                              CompilePlan plan=CompilePlan{},bool force=false) {
  const auto codegen_level=EnvUnsigned("GEKKOAOT_LLVM_CODEGEN_LEVEL",2u,0u,3u);
  const auto ir_level=EnvUnsigned("GEKKOAOT_LLVM_IR_LEVEL",3u,0u,3u);
  const auto worker_batch=EnvUnsigned("GEKKOAOT_LLVM_WORKER_BATCH",4u,1u,64u);
  const auto coalesce_size=EnvUnsigned("GEKKOAOT_LLVM_COALESCE_SIZE",100u,1u,4096u);
  const auto coalesce_limit=EnvUnsigned("GEKKOAOT_LLVM_COALESCE_LIMIT",32u,1u,4096u);
  const auto slp_limit=EnvUnsigned("GEKKOAOT_LLVM_SLP_MAX_INSTRUCTIONS",8192u,64u,1048576u);
  const auto poll_guard_cycles=EnvUnsigned("GEKKOAOT_AOT_POLL_GUARD_CYCLES",128u,16u,8192u);
  const std::string intercept_hash=static_intercepts.empty()?"dynamic":FileHash64(static_intercepts);
  const bool llvm_backend=backend=="llvm";
  const bool fpr_state_ssa = llvm_backend && Env("GEKKOAOT_FP_STATE_SSA", "0") == "1";
  const bool native_address = llvm_backend && Env("GEKKOAOT_NATIVE_ADDRESS_LOWERING", "1") != "0";
  const bool fp_region_coalesce = llvm_backend && FPRegionCoalesceEnabled();
  const bool native_sdk_clock = llvm_backend && Env("GEKKOAOT_NATIVE_SDK_CLOCK_LOWERING", "1") == "1";
  const bool native_wgpipe_try = llvm_backend && Env("GEKKOAOT_NATIVE_WGPIPE_TRY_LOWERING", "1") == "1";
  const bool native_hardware = llvm_backend &&
      (native_wgpipe_try || Env("GEKKOAOT_NATIVE_HARDWARE_LOWERING", "1") == "1");
  const auto static_entry_map=llvm_backend ? BuildStaticDolCodePointerMap(p) : fs::path{};
  const auto static_entry_hash=llvm_backend ? FileHash64(static_entry_map) : std::string("none");
  if(!llvm_backend && plan.pgo!=PgoMode::Off)
    throw std::runtime_error("PGO requires the LLVM backend");
  if(plan.pgo==PgoMode::Use && (plan.profile_path.empty() || !fs::exists(plan.profile_path)))
    throw std::runtime_error("PGO-use profile is missing: "+plan.profile_path.string());

  // Cross-title evidence is only an optimizer hint. A game's own IR profile
  // remains authoritative, and training/use must keep identical pre-PGO CFGs.
  if(llvm_backend && plan.pgo==PgoMode::Off)
    plan.crossgame_hot_manifest=MatchCrossGamePrior(p,plan.chunk_instructions);
  const auto crossgame_hash=plan.crossgame_hot_manifest.empty() ? std::string("none") :
      FileHash64(plan.crossgame_hot_manifest);

  const std::string profile_hash=plan.pgo==PgoMode::Use ? FileHash64(plan.profile_path) :
                                 plan.pgo==PgoMode::Generate ? TextHash64(plan.profile_path.string()) : "none";
  // v159: module_export.c is host-side dispatch glue, not part of DolRecomp's
  // translated-code cache. Key the final DSO on it explicitly so runtime-only
  // dispatch optimizations relink once without invalidating LLVM translation.
  const std::string module_runtime_hash=TextHash64(
      FileHash64(p.root/"runtime/module/module_export.c") +
      FileHash64(p.root/"runtime/module/native_hardware_shims.inc") +
      FileHash64(p.root/"runtime/core/native_hardware.h") +
      FileHash64(p.root/"runtime/module/module.exports"));
  // v285: use reviewed perf-selected functions for native register residency.
  // This is optimizer input only: no guest function replacement or game hack.
  // Leave training/use CFGs identical; disabled unless a profile is supplied.
  const auto native_hot_mode = Env("GEKKOAOT_NATIVE_HOT_RESIDENCY", "fp");
  if (native_hot_mode != "fp" && native_hot_mode != "full" && native_hot_mode != "0")
    throw std::runtime_error("GEKKOAOT_NATIVE_HOT_RESIDENCY must be fp, full or 0");
  const auto native_hot = native_hot_mode == "0" ? std::string() : Env("GEKKOAOT_NATIVE_HOT_FUNCTIONS");
  if (!native_hot.empty()) {
    if (!llvm_backend || plan.pgo != PgoMode::Off)
      throw std::runtime_error("GEKKOAOT_NATIVE_HOT_FUNCTIONS requires LLVM without IR PGO");
    plan.adaptive_hot_manifest = fs::absolute(native_hot);
    std::ifstream profile(plan.adaptive_hot_manifest);
    if (!profile) throw std::runtime_error("cannot read native hot-function profile");
    const auto dol_path = p.disc / "sys/main.dol";
    const auto hot = GekkoAOT::Build::ReadNativeHotProfile(profile, FileHash64(dol_path));
    std::ifstream dol_input(dol_path, std::ios::binary);
    const std::vector<unsigned char> dol_bytes((std::istreambuf_iterator<char>(dol_input)), {});
    for (const auto address : hot) {
      bool executable = false;
      for (unsigned i = 0; i != 7; ++i) {
        const auto begin = DolReadBE32(dol_bytes, 0x48u + i*4u);
        const auto size = DolReadBE32(dol_bytes, 0x90u + i*4u);
        if (address >= begin && std::uint64_t(address)-begin < size) executable = true;
      }
      if (!executable) throw std::runtime_error("native hot-function address is outside DOL text");
    }
    std::cout << "GEKKOAOT_NATIVE_HOT_RESIDENCY_V285=1 functions=" << hot.size()
              << " mode=" << native_hot_mode << " policy=registers-between-observation-boundaries\n";
  }
  const std::string adaptive_hash=!plan.adaptive_hot_manifest.empty() && fs::exists(plan.adaptive_hot_manifest)
      ? FileHash64(plan.adaptive_hot_manifest) : "none";
  const auto sampling=plan.pgo==PgoMode::Generate ? PgoSamplingFromEnv() : PgoSamplingConfig{};
  std::string tuning="c"+std::to_string(plan.chunk_instructions)+
      "-q"+std::to_string(plan.guard_cycles)+"-s"+std::to_string(plan.guard_steps)+
      "-i"+std::to_string(plan.inline_bytes)+"-ih"+std::to_string(plan.inline_hint_bytes)+
      "-rg"+std::to_string(plan.relaxed_guard_bytes)+
      "-pgo"+PgoModeName(plan.pgo)+(plan.thinlto?"-thin":"");
  if(plan.pgo!=PgoMode::Off) tuning += "-standard-ir-c-pgo192";
  if(Env("GEKKOAOT_EE_PENDING_QUERY_CODEGEN","1")!="0") tuning += "-eeq274";
  if(Env("GEKKOAOT_RFI_PENDING_QUERY_CODEGEN","1")!="0") tuning += "-rfi284";
  if(Env("GEKKOAOT_FP_COLD_EXIT_CODEGEN","1")!="0") tuning += "-fpx274";
  if(Env("GEKKOAOT_PARTITION_INTERNAL_LABELS","0")=="1") tuning += "-part275";
  // v274: the native ABI policy changes generated code; a module built with
  // another policy must not be reused (unrestricted keeps the old key).
  if(const auto abi=Env("GEKKOAOT_NATIVE_ABI","unrestricted"); abi!="unrestricted")
    tuning += "-abi-"+abi;
  if(crossgame_hash!="none") tuning += "-crossgame193-"+crossgame_hash;
  if(sampling.enabled)
    tuning += "-sampled-p"+std::to_string(sampling.period)+"-b"+
              std::to_string(sampling.burst)+"-x"+std::to_string(sampling.scale);
  if(adaptive_hash!="none")
    tuning += "-adaptive-"+adaptive_hash+"-residency285-"+native_hot_mode;
  // These transforms affect both portable-C and LLVM outputs.  Keep their
  // cache identity outside the LLVM-only block so switching backends never
  // reuses a module compiled with different helper/runtime semantics.
  tuning += Env("GEKKOAOT_SCALAR_FP_FASTPATH", "1") != "0"
                ? "-scalarfp28b-helper"
                : "-scalarfp-exact";
  tuning += Env("GEKKOAOT_PSQ_RAW_F32_FASTPATH", "1") != "0"
                ? "-psqraw34"
                : "-psqraw-exact";
  if (Env("GEKKOAOT_PSQ_FLOAT_SPECULATION", "1") != "0")
    tuning += "-psqf32spec320-res325-" + Env("GEKKOAOT_PSQ_RESOLVED", "1") +
              "-wg330-" + Env("GEKKOAOT_PSQ_WGPIPE", "1");
  if (Env("GEKKOAOT_PSQ_RAW_F32_FASTPATH", "1") != "0") tuning += "-psqdsi321";
  tuning += "-psqext331-" + Env("GEKKOAOT_PSQ_EXTEND_FAST", "1");
  tuning += "-pure337-dynwg338-" + Env("GEKKOAOT_DYNAMIC_WGPIPE_TRY", "1") +
            "-tbaa339-" + Env("GEKKOAOT_STATE_TBAA", "1") +
            "-ctl340-" + Env("GEKKOAOT_NATIVE_MATH_ANY_CONTROLS", "1") +
            "-pfma341-" + Env("GEKKOAOT_PAIRED_FMA_NI_DENORMAL", "1") +
            "-pni342-" + Env("GEKKOAOT_PAIRED_NI_DENORMAL", "1");
  tuning += Env("GEKKOAOT_GX_MATRIX_HLE", "1") != "0"
                ? "-gxmtx34"
                : "-gxmtx-guest";
  // v217 changes the set of DOL addresses admitted as executable code.  Keep
  // old modules/LLVM objects out of the cache even when the static MAP itself
  // is unchanged (direct branch targets are discovered by DolRecomp).
  tuning += "-datacode217";
  tuning += Env("GEKKOAOT_DIRECT_SCALAR_FP", "1") != "0"
                ? "-directfp186"
                : "-directfp-off";
  if(llvm_backend) {
    tuning += fp_region_coalesce ? "-fpregion292-on" : "-fpregion292-off";
    if (native_address) tuning += "-address287-flat304-" + Env("GEKKOAOT_NATIVE_FLAT_VMEM", "1") + "-lc308-" + Env("GEKKOAOT_NATIVE_LOCKED_CACHE", "1");
    if (native_address) tuning += "-pagedvm322-" + Env("GEKKOAOT_NATIVE_PAGED_VMEM", "1");
  if (Env("GEKKOAOT_LLVM_HOT_COLD_SPLIT", "0") != "0") tuning += "-hcs323";
  if (Env("GEKKOAOT_STATE_IN_MEMORY", "1") == "0") tuning += "-statessa327";
    tuning += "-math316-" + Env("GEKKOAOT_NATIVE_SCALAR_MATH", "1");
    if (fpr_state_ssa) tuning += "-fpssa228";
    if (native_sdk_clock) tuning += "-sdkclock238";
    if (native_hardware) tuning += "-nativehw243-stablemmio280-dynamic281";
    if (native_wgpipe_try) tuning += "-wgtry244";
    if (Env("GEKKOAOT_DIRECT_SCALAR_FP", "1") != "0")
      tuning += "-fprflazy187-dynamicfma189";
    tuning += "-sharedpoll75-pollpark42g"+std::to_string(poll_guard_cycles)+
              "-crossentry206-"+static_entry_hash+
              "-staticadmission44-halfwordreverse45"+
              "-ctxabi76-globalindirect87-memrestart95-vmcap97-sprresume167-ctrabi129-msree151-cg"+
              std::to_string(codegen_level)+"-ir"+std::to_string(ir_level)+
              "-wb"+std::to_string(worker_batch)+"-join"+std::to_string(coalesce_size)+
              "x"+std::to_string(coalesce_limit)+"-slp"+std::to_string(slp_limit)+"-chainstate24-xmodgpr207";
  } else {
    tuning += "-cpoll43-staticdispatch43";
    tuning += Env("GEKKOAOT_PORTABLE_C_MEM32_FASTPATH", "1") != "0"
                  ? "-cmem40-mem32-inline43"
                  : "-cmem34safe39";
  }

  /*
   * v213 repairs the v206 map before looking up the main module cache.
   * The map hash belongs only to the top-level DOL artifact key.
   *
   * Do NOT put it in 'tuning': the LLVM job cache already hashes function
   * ranges/entry-points individually, allowing unaffected objects to stay
   * cached.
   */
  EnsureRelToDolHardBoundariesV213(p);

  const auto rel_to_dol_map_v213 =
      p.cache / "entrypoints" / p.game_id /
      "dol-static+rel-code-pointers-v206.map";

  const std::string rel_to_dol_map_hash_v213 =
      fs::exists(rel_to_dol_map_v213)
          ? FileHash64(rel_to_dol_map_v213)
          : "nomap";

  // v328: profile-guided machine function splitting for PGO-use modules
  // (HPCOS main thread -5%). It only changes the final link, so it belongs to
  // the module key, never to the LLVM object/bitcode cache key.
  const bool split_machine_functions =
      plan.pgo == PgoMode::Use && Env("GEKKOAOT_SPLIT_MACHINE_FUNCTIONS", "1") != "0";
  const std::string artifact_tuning_v213 =
      tuning + "-ep" + rel_to_dol_map_hash_v213 + "-lookup227-hostfilter278" +
      (split_machine_functions ? "-mfs328" : "");

const std::string artifact_key=std::string("v")+GEKKOAOT_VERSION+"-"+
      std::string(backend)+"-"+std::string(policy_tag)+"-"+artifact_tuning_v213+"-"+profile_hash+"-"+
      intercept_hash+"-modrt"+module_runtime_hash.substr(0,8)+"-"+
      std::string(GEKKOAOT_DOLRECOMP_REV).substr(0,8)+"-"+
      std::string(GEKKOAOT_AURORA_REV).substr(0,8);
  const auto artifact=p.cache/"modules"/p.game_id/CacheComponent(artifact_key);
#ifdef _WIN32
  std::cout<<"GEKKOAOT_WINDOWS_CACHE_PATH_V1=1 kind=module full-key-chars="<<artifact_key.size()
           <<" compact-key=\""<<artifact.filename().string()<<"\" path-chars="
           <<artifact.wstring().size()<<"\n";
#endif
  const auto module=artifact/("g"+p.game_id+"_recomp"+SharedSuffix());
  if(fs::exists(module)&&!force){std::cout<<"cache hit: native module "<<module<<"\nGEKKOAOT_PROGRESS=93|Native module cache hit\n";return module;}
  fs::create_directories(artifact);
  const auto output=artifact/"dolrecomp-output"; if(force){std::error_code ec;fs::remove_all(output,ec);}
  fs::create_directories(output);
  std::cout<<"GEKKOAOT_STAGE=compile\nGEKKOAOT_PROGRESS=38|"<<(llvm_backend?"LLVM AOT":"Portable C AOT")<<" recompilation\n";
  std::string gen;
  if(llvm_backend) {
    const auto native_abi=Env("GEKKOAOT_NATIVE_ABI","unrestricted");
    const std::string llvm_cache_key=
        std::string("cache24-crossentry206-sprresume167-msree151-ctrabi129-chainstate24-xmodgpr207-")+intercept_hash+"-"+tuning+"-"+profile_hash+"-"+
        std::string(GEKKOAOT_DOLRECOMP_REV).substr(0,8)+"-"+native_abi+
        (Env("GEKKOAOT_STATE_IN_MEMORY","1")!="0" ? "-state-in-memory-host-exact" : "-state-ssa-v327-host-exact");
    const auto llvm_cache=p.cache/"dolrecomp-llvm"/CacheComponent(llvm_cache_key);
#ifdef _WIN32
    std::cout<<"GEKKOAOT_WINDOWS_CACHE_PATH_V1=1 kind=llvm full-key-chars="<<llvm_cache_key.size()
             <<" compact-key=\""<<llvm_cache.filename().string()<<"\" path-chars="
             <<llvm_cache.wstring().size()<<"\n";
#endif
    fs::create_directories(llvm_cache);
    std::cout << "GEKKOAOT_LLVM_COMPILE_TURBO_V1=1 ir=" << ir_level
              << " worker_batch=" << worker_batch
              << " thinlto=" << (plan.thinlto?1:0)
              << " fallback_audit=0 target_machine_cache=1\n";
    gen = "cmake -E env " +
          QuoteText(std::string("DOLRECOMP_LLVM_CACHE=")+llvm_cache.string()) + " " +
          QuoteText("DOLRECOMP_DISPATCH_LOOKUP=indexed") + " " +
          QuoteText(std::string("DOLRECOMP_NATIVE_HOT_RESIDENCY=")+native_hot_mode) + " " +
          QuoteText(std::string("DOLRECOMP_NATIVE_ADDRESS_LOWERING=")+(native_address ? "1" : "0")) + " " +
          QuoteText(std::string("DOLRECOMP_NATIVE_FLAT_VMEM=")+Env("GEKKOAOT_NATIVE_FLAT_VMEM", "1")) + " " +
          QuoteText(std::string("DOLRECOMP_NATIVE_PAGED_VMEM=")+Env("GEKKOAOT_NATIVE_PAGED_VMEM", "1")) + " " +
          QuoteText(std::string("DOLRECOMP_PSQ_RESOLVED=")+Env("GEKKOAOT_PSQ_RESOLVED", "1")) + " " +
          QuoteText(std::string("DOLRECOMP_PSQ_WGPIPE=")+Env("GEKKOAOT_PSQ_WGPIPE", "1")) + " " +
          QuoteText(std::string("DOLRECOMP_PSQ_EXTEND_FAST=")+Env("GEKKOAOT_PSQ_EXTEND_FAST", "1")) + " " +
          QuoteText(std::string("DOLRECOMP_DYNAMIC_WGPIPE_TRY=")+Env("GEKKOAOT_DYNAMIC_WGPIPE_TRY", "1")) + " " +
          QuoteText(std::string("DOLRECOMP_STATE_TBAA=")+Env("GEKKOAOT_STATE_TBAA", "1")) + " " +
          QuoteText(std::string("DOLRECOMP_NATIVE_MATH_ANY_CONTROLS=")+Env("GEKKOAOT_NATIVE_MATH_ANY_CONTROLS", "1")) + " " +
          QuoteText(std::string("DOLRECOMP_PAIRED_FMA_NI_DENORMAL=")+Env("GEKKOAOT_PAIRED_FMA_NI_DENORMAL", "1")) + " " +
          QuoteText(std::string("DOLRECOMP_PAIRED_NI_DENORMAL=")+Env("GEKKOAOT_PAIRED_NI_DENORMAL", "1")) + " " +
          (Env("GEKKOAOT_LLVM_HOT_COLD_SPLIT", "0") != "0"
               ? QuoteText(std::string("DOLRECOMP_LLVM_OPTIONS=")+HotColdSplitLlvmOptions()) + " "
               : std::string()) +
          QuoteText(std::string("DOLRECOMP_NATIVE_LOCKED_CACHE=")+Env("GEKKOAOT_NATIVE_LOCKED_CACHE", "1")) + " " +
          QuoteText(std::string("DOLRECOMP_NATIVE_SCALAR_MATH=")+Env("GEKKOAOT_NATIVE_SCALAR_MATH", "1")) + " " +
          QuoteText(std::string("DOLRECOMP_FP_STATE_SSA=")+(fpr_state_ssa ? "1" : "0")) + " " +
          QuoteText(std::string("DOLRECOMP_FP_REGION_COALESCE=")+(fp_region_coalesce ? "1" : "0")) + " " +
          QuoteText(std::string("DOLRECOMP_NATIVE_SDK_CLOCK_LOWERING=")+(native_sdk_clock ? "1" : "0")) + " " +
          QuoteText(std::string("DOLRECOMP_NATIVE_HARDWARE_LOWERING=")+(native_hardware ? "1" : "0")) + " " +
          QuoteText(std::string("DOLRECOMP_NATIVE_WGPIPE_TRY_LOWERING=")+(native_wgpipe_try ? "1" : "0")) + " " +
          QuoteText(std::string("DOLRECOMP_LLVM_CHUNK_INSTRUCTIONS=")+std::to_string(plan.chunk_instructions)) + " " +
          QuoteText(std::string("DOLRECOMP_GUARD_CYCLES=")+std::to_string(plan.guard_cycles)) + " " +
          QuoteText(std::string("DOLRECOMP_GUARD_STEPS=")+std::to_string(plan.guard_steps)) + " " +
          QuoteText(std::string("DOLRECOMP_POLL_GUARD_CYCLES=")+std::to_string(poll_guard_cycles)) + " " +
          QuoteText("DOLRECOMP_LLVM_CODEGEN_LEVEL="+std::to_string(codegen_level)) + " " +
          QuoteText("DOLRECOMP_LLVM_IR_LEVEL="+std::to_string(ir_level)) + " " +
          QuoteText("DOLRECOMP_LLVM_WORKER_BATCH="+std::to_string(worker_batch)) + " " +
          QuoteText(std::string("DOLRECOMP_LLVM_EMIT_THINLTO=")+(plan.thinlto?"1":"0")) + " " +
          QuoteText("DOLRECOMP_FALLBACK_AUDIT=0") + " " +
          QuoteText("DOLRECOMP_LLVM_COALESCE_SIZE="+std::to_string(coalesce_size)) + " " +
          QuoteText("DOLRECOMP_LLVM_COALESCE_LIMIT="+std::to_string(coalesce_limit)) + " " +
          QuoteText("DOLRECOMP_LLVM_SLP_MAX_INSTRUCTIONS="+std::to_string(slp_limit)) + " " +
          QuoteText(std::string("DOLRECOMP_NATIVE_INLINE_BYTES=")+std::to_string(plan.inline_bytes)) + " " +
          QuoteText(std::string("DOLRECOMP_NATIVE_INLINE_HINT_BYTES=")+std::to_string(plan.inline_hint_bytes)) + " " +
          QuoteText(std::string("DOLRECOMP_RELAXED_CALL_GUARD_BYTES=")+std::to_string(plan.relaxed_guard_bytes)) +
          (Env("GEKKOAOT_PARTITION_INTERNAL_LABELS","0")=="1"
               ? " "+QuoteText("DOLRECOMP_PARTITION_INTERNAL_LABELS=1") : std::string());
    if(sampling.enabled) {
      const std::string llvm_options="-enable-sampled-instrumentation -sampled-instr-period="+
          std::to_string(sampling.period)+" -sampled-instr-burst-duration="+
          std::to_string(sampling.burst);
      gen += " " + QuoteText(std::string("DOLRECOMP_LLVM_OPTIONS=")+llvm_options);
    }
    if(!static_intercepts.empty())
      gen += " " + QuoteText(std::string("DOLRECOMP_STATIC_INTERCEPTS=")+static_intercepts.string());

    const auto rel_to_dol_liveins =
        p.cache / "entrypoints" / p.game_id / "rel-to-dol-v206.txt";

    if (fs::exists(rel_to_dol_liveins))
      gen += " " + QuoteText(
          std::string("DOLRECOMP_REL_TO_DOL_LIVEINS_V211_DISABLED=") +
          rel_to_dol_liveins.string());

    gen += " " + QuoteText(std::string("DOLRECOMP_CROSSGAME_HOT_FUNCTIONS=")+
                           plan.crossgame_hot_manifest.string());
    if(plan.pgo!=PgoMode::Off)
      // A post-training address list must not change the pre-PGO CFG/state
      // lowering. LLVM IRUse consumes counters for the original optimized IR
      // and makes hot/cold, inlining and layout decisions for all functions.
      gen += " " + QuoteText("DOLRECOMP_ADAPTIVE_HOT_FUNCTIONS=");
    else if(adaptive_hash!="none")
      gen += " " + QuoteText(std::string("DOLRECOMP_ADAPTIVE_HOT_FUNCTIONS=")+plan.adaptive_hot_manifest.string());
    gen += " " + Quote(dolrecomp) +
          " -j"+std::to_string(Jobs())+" --backend="+std::string(backend)+
          " --cpu gekko --gamecube";
    // v327 experiment: GEKKOAOT_STATE_IN_MEMORY=0 keeps every function's guest
    // state in SSA (as PGO-hot/native-ABI functions already do) instead of
    // reloading/storing CPUState around each instruction.
    if (Env("GEKKOAOT_STATE_IN_MEMORY", "1") != "0") gen += " --state-in-memory";
    gen += " --native-abi="+native_abi+" --targets=host --semantics=exact";
    gen += " --partition-instructions "+std::to_string(plan.chunk_instructions);
    gen += " --map "+Quote(static_entry_map);
    if(plan.pgo==PgoMode::Generate)
      gen += " --profile-generate "+Quote(plan.profile_path);
    else if(plan.pgo==PgoMode::Use)
      gen += " --profile-use "+Quote(plan.profile_path);
    std::cout<<"GEKKOAOT_LLVM_TURBO_V12=1 state-in-memory="<<(Env("GEKKOAOT_STATE_IN_MEMORY","1")!="0"?1:0)<<" native-abi="<<native_abi
             <<" target=host semantics=exact dispatch=indexed static-intercepts="
             <<(static_intercepts.empty()?"off":intercept_hash)<<" cache="<<llvm_cache<<"\n";
    std::cout<<"GEKKOAOT_NATIVE_AOT_QUANTUM_V12=1 chunk="<<plan.chunk_instructions
             <<" cycles="<<plan.guard_cycles<<" steps="<<plan.guard_steps
             <<" pgo="<<PgoModeName(plan.pgo)<<" thinlto="<<(plan.thinlto?1:0)<<"\n";
    std::cout << "GEKKOAOT_FP_REGION_COALESCE_V292=" << (fp_region_coalesce ? 1 : 0)
              << " max-paired-run=32 first-fp-restart=retained independent-entries=retained\n";
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
  Require(
      RunProcess(
          p.meta_tool,
          std::vector<fs::path>{
              generated/"generated.h",
              smc,
              p.disc/"sys/main.dol",
              tables,
          }),
      "module metadata");
  const auto mb=artifact/"module-build"; std::error_code ec; fs::remove_all(mb,ec);
  std::string cfg="cmake -S "+Quote(p.root/"runtime/module")+" -B "+Quote(mb)+" -G Ninja -DCMAKE_BUILD_TYPE=Release -DGAME_ID="+QuoteText(p.game_id)+" -DGENERATED_DIR="+Quote(generated)+" -DDOLRECOMP_DIR="+Quote(p.dolrecomp_src)+" -DGEKKOAOT_ROOT="+Quote(p.root)+" -DMODULE_TABLES="+Quote(tables);
  const auto portable_toolchain=p.root/"toolchain/gekkoaot-zig.cmake";
  if(BundledToolchainReady(p.root) && fs::exists(portable_toolchain))
    cfg += " -DCMAKE_TOOLCHAIN_FILE="+Quote(portable_toolchain);
  cfg += " -DGEKKOAOT_PGO_MODE="+QuoteText(PgoModeName(plan.pgo));
  if(sampling.enabled)
    cfg += " -DGEKKOAOT_PGO_SAMPLE_PERIOD="+std::to_string(sampling.period)+
           " -DGEKKOAOT_PGO_SAMPLE_BURST="+std::to_string(sampling.burst);
  cfg += " -DGEKKOAOT_USE_THINLTO="+std::string(plan.thinlto?"ON":"OFF");
  if(plan.pgo==PgoMode::Use && Env("GEKKOAOT_SPLIT_MACHINE_FUNCTIONS","1")!="0")
    cfg += " -DGEKKOAOT_SPLIT_MACHINE_FUNCTIONS=ON";
  if(plan.pgo==PgoMode::Use) cfg += " -DGEKKOAOT_PGO_PROFILE="+Quote(plan.profile_path);
  if(plan.pgo!=PgoMode::Off) {
    // Match the C instrumentation runtime to DolRecomp's LLVM/profile tools;
    // PATH may otherwise pick a different major version (20 vs 22 here).
    const auto sibling=FindLlvmProfdata().parent_path()/
#ifdef _WIN32
        "clang.exe";
#else
        "clang";
#endif
    const auto clang=Env("GEKKOAOT_CLANG",fs::exists(sibling)?sibling.string():"clang");
    cfg += " -DCMAKE_C_COMPILER="+QuoteText(clang);
  }
  Require(Run(cfg),"game module configure"); Require(Run("cmake --build "+Quote(mb)+" -j"+std::to_string(Jobs())),"game module link");
  fs::path built=mb/("g"+p.game_id+"_recomp"+SharedSuffix()); if(!fs::exists(built)){auto x=FindFileRecursive(mb,built.filename().string());if(!x.empty())built=x;}
  if (!fs::exists(built)) throw std::runtime_error("linked game module not found");
  // Cancellation during a large copy must not turn a truncated DSO into a
  // cache hit on the next launch. Publish only the complete native module.
  auto temporary_module=module;
  temporary_module += ".tmp";
  fs::copy_file(built,temporary_module,fs::copy_options::overwrite_existing);
  fs::rename(temporary_module,module,ec);
  if(ec) { // Windows rename does not replace an existing destination.
    fs::remove(module); fs::rename(temporary_module,module);
  }
  std::cout<<"GEKKOAOT_PROGRESS=98|Native module ready\n"; return module;
}

fs::path EnsureSdkManifest(Pipeline& p,const fs::path&,std::string_view) {
  // The SDK manifest is generated by structural analysis of this exact DOL and
  // by the resolver compiled into gekkoaot-native-run.  v10 keyed only the
  // policy, which meant an old 5-hook manifest could survive resolver fixes
  // forever.  Include the DOL plus an explicit resolver ABI in the cache key;
  // bump the ABI whenever admission/matching semantics change.
  // v153 correctness default: SDK OS code stays in guest PPC->AOT unless
  // GEKKOAOT_NATIVE_OS=1 explicitly opts into the host NativeOS replacement.
  // Bump whenever resolver admission/matching or the effective NativeOS default
  // changes so stale manifests cannot survive.
  // v31 (v300): SDK service corpus pass (VM-correct libc/cache helpers) and
  // the native PADRead service can add hooks to existing games.
  // v32 (v301): bounded string helpers (strncmp/strncpy/strncat/strnlen) are
  // VM-correct and join the corpus seed set.
  // v33 (v306): proven DVD FST leaf and normal-matrix native services.
  // v34 (v311): complete GXSetArray proof and native vertex-array binding.
  constexpr std::string_view kSdkResolverAbi = "34";
  const auto dol_hash = FileHash64(p.disc/"sys/main.dol");
  const std::string policy = std::string("auto=")+Env("GEKKOAOT_NATIVE_SDK_AUTO","1")+
      ";os="+Env("GEKKOAOT_NATIVE_OS","0")+
      ";admission="+Env("GEKKOAOT_NATIVE_OS_ADMISSION","hot")+
      ";bisect="+Env("GEKKOAOT_NATIVE_OS_BISECT","all")+
      ";interrupt-leaves="+Env("GEKKOAOT_NATIVE_OS_INTERRUPT_LEAVES","0")+
      ";service-corpus="+Env("GEKKOAOT_SDK_SERVICE_CORPUS","1")+
      ";native-pad="+Env("GEKKOAOT_NATIVE_PAD","1")+
      ";dvd-fst="+Env("GEKKOAOT_NATIVE_DVD_FST","1")+
      ";gx-normal="+Env("GEKKOAOT_NATIVE_GX_NORMAL_MATRIX","1")+
      ";gx-arrays="+Env("GEKKOAOT_NATIVE_GX_ARRAY_SERVICE","1")+
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
  Require(
      RunProcess(
          p.native_run,
          std::vector<fs::path>{
              fs::path("--dol"),
              p.disc/"sys/main.dol",
              fs::path("--emit-sdk-manifest"),
              p.sdk_manifest,
          }),
      "build-time SDK/HLE analysis");
  if(!fs::exists(p.sdk_manifest)) throw std::runtime_error("SDK manifest was not produced");
  std::cout<<"GEKKOAOT_SDK_COMPILETIME_V21=1 cache=miss manifest=\""<<p.sdk_manifest.string()
           <<"\" hash="<<FileHash64(p.sdk_manifest)<<" policy-hash="<<policy_hash<<"\n";
  return p.sdk_manifest;
}

fs::path AuditNativePort(Pipeline& p, const fs::path& dolrecomp) {
  const auto sdk = EnsureSdkManifest(p, dolrecomp, "llvm");
  const auto entry_map = BuildStaticDolCodePointerMap(p);
  EnsureRelToDolHardBoundariesV213(p);
  const auto identity = TextHash64("side-exit232;dol=" + FileHash64(p.disc/"sys/main.dol") +
      ";sdk=" + FileHash64(sdk) + ";entries=" + FileHash64(entry_map));
  const auto directory = p.state/"native-port"/p.game_id/identity;
  fs::create_directories(directory);
  const auto report = directory/"side-exits.json";
  // Do not accept a stale report after an interrupted/failed analysis.
  std::error_code ec; fs::remove(report, ec);
  std::string command = "cmake -E env " +
      QuoteText("DOLRECOMP_SIDE_EXIT_ANALYSIS_ONLY=1") + " " +
      QuoteText("DOLRECOMP_SIDE_EXIT_REPORT=" + report.string()) + " " +
      QuoteText("DOLRECOMP_STATIC_INTERCEPTS=" + sdk.string()) + " " +
      QuoteText("DOLRECOMP_LLVM_CACHE=off") + " " + Quote(dolrecomp) +
      " --backend=llvm --cpu gekko --gamecube --state-in-memory" +
      " --native-abi=unrestricted --targets=host --semantics=exact" +
      " --partition-instructions 1024 --map " + Quote(entry_map) + " " +
      Quote(p.disc/"sys/main.dol") + " " + Quote(directory/"analysis");
  Require(Run(command), "native-port side-exit analysis");
  if (!fs::exists(report)) throw std::runtime_error("native-port report was not produced");
  std::cout << "GEKKOAOT_NATIVE_PORT_REPORT_V232=1 scope=main-dol stage=source-analysis"
            << " full_native=0 executable=none report=\"" << report.string() << "\"\n";
  return report;
}

std::string PgoIRIdentity(const Pipeline& p) {
  const auto plan=PgoCompilePlan(PgoMode::Generate,{});
  return "standard-ir192-directfp189;dol="+FileHash64(p.disc/"sys/main.dol")+
      ";sdk="+(p.sdk_manifest.empty()?std::string("none"):FileHash64(p.sdk_manifest))+
      ";cpu="+FileHash64(p.dolrecomp_src/"src/cpu/cpu.c")+
      ";glue="+FileHash64(p.root/"runtime/module/module_export.c")+
      ";hardware-shims="+FileHash64(p.root/"runtime/module/native_hardware_shims.inc")+
      ";hardware-abi="+FileHash64(p.root/"runtime/core/native_hardware.h")+
      ((Env("GEKKOAOT_NATIVE_HARDWARE_LOWERING","1")=="1" ||
        Env("GEKKOAOT_NATIVE_WGPIPE_TRY_LOWERING","1")=="1") ? ";hardware-lowering=243" : "")+
      (Env("GEKKOAOT_NATIVE_HARDWARE_LOWERING","1")=="1" ? ";stable-mmio=280;dynamic-mmio=281" : "")+
      (Env("GEKKOAOT_NATIVE_WGPIPE_TRY_LOWERING","1")=="1" ? ";wgpipe-try=244" : "")+
      (Env("GEKKOAOT_NATIVE_SDK_CLOCK_LOWERING","1")=="1" ? ";sdk-clock-lowering=238" : "")+
      (Env("GEKKOAOT_FP_STATE_SSA","0")=="1" ? ";fpr-ssa=228" : "")+
      // Enabled coalescing changes pre-PGO CFG identities. Existing profiles
      // remain in their previous directory; automatic Play uses normal AOT
      // until this exact policy has fresh counters. The opt-out retains the
      // previous CFG identity for useful regression comparisons.
      (FPRegionCoalesceEnabled() ? ";fp-region-coalesce=292-on" : "")+
      (Env("GEKKOAOT_NATIVE_SCALAR_MATH","1")=="1" ? ";native-scalar-math=316-on" : "")+
      ";chunk="+std::to_string(plan.chunk_instructions)+
      ";cycles="+std::to_string(plan.guard_cycles)+";steps="+std::to_string(plan.guard_steps)+
      ";inline="+std::to_string(plan.inline_bytes)+";hint="+std::to_string(plan.inline_hint_bytes)+
      ";relax="+std::to_string(plan.relaxed_guard_bytes)+
      ";poll="+std::to_string(EnvUnsigned("GEKKOAOT_AOT_POLL_GUARD_CYCLES",128u,16u,8192u))+
      ";abi="+Env("GEKKOAOT_NATIVE_ABI","unrestricted")+
      ";scalar="+(Env("GEKKOAOT_SCALAR_FP_FASTPATH","1")!="0"?"1":"0")+
      ";psq="+(Env("GEKKOAOT_PSQ_RAW_F32_FASTPATH","1")!="0"?"1":"0")+
      ";matrix="+(Env("GEKKOAOT_GX_MATRIX_HLE","1")!="0"?"1":"0")+
      ";direct="+(Env("GEKKOAOT_DIRECT_SCALAR_FP","1")!="0"?"1":"0")+
      ";slp="+std::to_string(EnvUnsigned("GEKKOAOT_LLVM_SLP_MAX_INSTRUCTIONS",8192u,64u,1048576u));
}

fs::path PgoDirectory(const Pipeline& p) {
  // LLVM instrumentation identities are tied to the generated CFG. Keep old
  // profiles available, but isolate profiles produced by this hot-path ABI so
  // stale counters cannot be silently reused after compiler CFG changes.
  const auto compat=Env("GEKKOAOT_PGO_COMPAT_ID","perf192-standard-ir-c-pgo");
  const auto base=p.cache/"pgo"/p.game_id/compat;
  const auto identity=PgoIRIdentity(p);
  // The v192 sessions recorded during this audit have a verified context.
  // Other configurations get distinct cumulative profiles and raw folders.
  // Legacy profiles without a recorded context are retained, never guessed.
  if(ReadLine(base/"compile-context-v193.txt")==identity) return base;
  return base/("ir-"+TextHash64(identity));
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

fs::path MatchCrossGamePrior(Pipeline& p,unsigned chunk) {
  if(Env("GEKKOAOT_CROSSGAME_ENABLE","1")=="0") return {};
  const auto tool=CrossGameTool(p),community=CrossGameCommunityDb(p),local=CrossGameLocalDb(p);
  if(!fs::exists(tool) || (!fs::exists(community) && !fs::exists(local))) return {};
  const auto dol=p.disc/"sys/main.dol";
  const auto key=TextHash64(FileHash64(dol)+FileHash64(tool)+
      (fs::exists(community)?FileHash64(community):"none")+
      (fs::exists(local)?FileHash64(local):"none")+p.game_id+std::to_string(chunk)+"exact-193");
  const auto dir=p.cache/"crossgame-priors"/p.game_id/key;
  const auto manifest=dir/"hot-symbols.txt",env=dir/"match.env";
  fs::create_directories(dir);
  if(!fs::exists(env) || !fs::exists(manifest)) {
    std::string cmd=QuoteText(CrossGamePython())+" "+Quote(tool)+" match --dol "+Quote(dol)+
        " --db "+Quote(community)+" --env "+Quote(env)+" --symbol-order "+Quote(manifest)+
        " --compile-chunk "+std::to_string(chunk)+" --chunk-locked --exact-only --exclude-game "+
        QuoteText(p.game_id);
    if(fs::exists(local)) cmd += " --local "+Quote(local);
    if(Run(cmd)!=0) {
      std::cerr<<"[gekkoaot] warning: CrossGameDB match failed; using standard O3\n";
      std::error_code ec;fs::remove(env,ec);fs::remove(manifest,ec);return {};
    }
  }
  if(!fs::exists(manifest) || fs::file_size(manifest)==0) {
    std::cout<<"GEKKOAOT_CROSSGAME_PRIOR_V193=0 reason=no-other-title-match\n";
    return {};
  }
  std::cout<<"GEKKOAOT_CROSSGAME_PRIOR_V193=1 manifest=\""<<manifest.string()
           <<"\" policy=exact-normalized-ppc-hot-attribute own-pgo=authoritative\n";
  return manifest;
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
  // The hybrid FPR experiment changes the IR control flow. Existing profiles
  // describe the normal compiler policy and require fresh training for it.
  const bool fpr_state_ssa = Env("GEKKOAOT_FP_STATE_SSA", "0") == "1";
  if(backend=="llvm" && !fpr_state_ssa && Env("GEKKOAOT_PGO_AUTO_USE","1")!="0" && fs::exists(profile)) {
    std::cout<<"GEKKOAOT_PGO_AUTO_USE_V12=1 profile=\""<<profile.string()
             <<"\" hash="<<FileHash64(profile)<<"\n";
    auto plan=PgoCompilePlan(PgoMode::Use,profile);
    return CompileModuleVariant(p,dolrecomp,backend,"perf192-standard-pgo-use",manifest,
                                plan,force);
  }
  return CompileModuleVariant(p,dolrecomp,backend,"perf35-dynamic-mmio-v281",manifest,
                              NormalCompilePlan(),force);
}

std::vector<fs::path> BuildSecondaryModules(Pipeline& p,const fs::path& dolrecomp) {
  const auto out=p.cache/"secondary-aot"/p.game_id;
  const auto manifest=out/"modules.txt";
  fs::create_directories(out);

  // v212 diagnostic/reuse path:
  // keep already-built REL modules when only the main DOL compiler policy
  // changed. Explicit opt-in avoids ever hiding a genuine REL/content change.
  if (Env("GEKKOAOT_REUSE_SECONDARY","0") == "1" && fs::exists(manifest)) {
    std::vector<fs::path> cached;
    std::ifstream in(manifest);
    std::string line;
    bool valid = true;

    while (std::getline(in,line)) {
      if (line.empty())
        continue;
      fs::path module=line;
      if (!fs::exists(module)) {
        valid=false;
        break;
      }
      cached.push_back(fs::absolute(module));
    }

    if (valid && !cached.empty()) {
      std::cout
          << "GEKKOAOT_SECONDARY_MANIFEST_REUSE_V212=1 modules="
          << cached.size()
          << " policy=skip-secondary-build-existing-manifest\n";
      return cached;
    }

    std::cout
        << "GEKKOAOT_SECONDARY_MANIFEST_REUSE_V212=0 reason=invalid-manifest\n";
  }
  // v274: give secondary executables the runtime's interception set as an
  // address-only manifest. Without any manifest DolRecomp makes every function
  // entry ask the host whether its range holds an SDK intercept
  // (ppc_native_region_available): ~20M host calls/s in GoldenEye: Rogue Agent,
  // whose game code lives in a side ELF. The runtime registers exactly the
  // primary manifest's hooks, so a range without one of those addresses always
  // gets "allowed"; ranges that contain one keep the runtime probe and its code
  // proof, because the tokens and ABI classes are stripped (no compile-time HLE
  // or context-capture ABI is applied to secondary code).
  std::string secondary_intercepts;
  if (Env("GEKKOAOT_SECONDARY_STATIC_INTERCEPTS","1") != "0" &&
      !p.sdk_manifest.empty() && fs::exists(p.sdk_manifest)) {
    const auto stripped = out/("probe-intercepts-v274-"+FileHash64(p.sdk_manifest)+".txt");
    std::ifstream source(p.sdk_manifest);
    std::ostringstream text;
    text<<"# GEKKOAOT_SECONDARY_PROBE_INTERCEPTS_V274 address name\n";
    std::string row;
    while (std::getline(source,row)) {
      if (row.empty() || row[0]=='#') continue;
      std::istringstream fields(row);
      std::string address,name;
      if (!(fields>>address>>name)) throw std::runtime_error("malformed SDK manifest row: "+row);
      text<<address<<' '<<name<<'\n';
    }
    WriteText(stripped,text.str());
    secondary_intercepts=stripped.string();
    std::cout<<"GEKKOAOT_SECONDARY_STATIC_INTERCEPTS_V274=1 manifest=\""<<secondary_intercepts
             <<"\" policy=address-only-runtime-probe\n";
  }
  std::ostringstream cmd;
  cmd<<"cmake -E env "<<QuoteText("DOLRECOMP_DISPATCH_LOOKUP=indexed")<<" ";
  if (!secondary_intercepts.empty())
    cmd<<QuoteText("GEKKOAOT_SECONDARY_STATIC_INTERCEPTS="+secondary_intercepts)<<" ";
  cmd<<Quote(p.secondary_tool)
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

  // Preserve the existing Linux compatibility default. An explicit setting
  // remains available for profiling and renderer correctness comparisons.
#ifdef _WIN32
  const auto texture_cache = Env("GEKKOAOT_NATIVE_GX_FIFO_TEXTURE_CACHE", "1");
#else
  const auto texture_cache = Env("GEKKOAOT_NATIVE_GX_FIFO_TEXTURE_CACHE", "0");
#endif
#ifdef _WIN32
  _putenv_s("GEKKOAOT_NATIVE_GX_FIFO_TEXTURE_CACHE",texture_cache.c_str());
#else
  setenv("GEKKOAOT_NATIVE_GX_FIFO_TEXTURE_CACHE",texture_cache.c_str(),1);
#endif
}
// Cache directory names can be compact hashes. Preserve the exact controller
// selection instead of making diagnostic launchers infer SDK/REL admission
// from a filename, mtime or game ID shared by multiple disc revisions.
void WriteAotLaunchMetadata(const Pipeline& p,const fs::path& image,
                            const fs::path& module,const fs::path& bridge,
                            const std::vector<fs::path>& secondary_modules) {
  const auto json_string=[](std::string_view value) {
    constexpr char hex[]="0123456789abcdef";
    std::string out="\"";
    for(const unsigned char c:value) {
      if(c=='\"' || c=='\\') { out+='\\'; out+=static_cast<char>(c); }
      else if(c<0x20) { out+="\\u00"; out+=hex[c>>4]; out+=hex[c&15]; }
      else out+=static_cast<char>(c);
    }
    out+='\"'; return out;
  };
  const auto path_string=[&](const fs::path& value) {
    return json_string(value.empty()?std::string{}:fs::absolute(value).string());
  };
  std::ostringstream out;
  out<<"{\n  \"schema\": 1,\n  \"provenance\": \"controller-cache-key-selection\",\n"
     <<"  \"game_id\": "<<json_string(p.game_id)<<",\n"
     <<"  \"source\": "<<path_string(image)<<",\n"
     <<"  \"dol\": "<<path_string(p.disc/"sys/main.dol")<<",\n"
     <<"  \"dol_fnv64\": "<<json_string(FileHash64(p.disc/"sys/main.dol"))<<",\n"
     <<"  \"module\": "<<path_string(module)<<",\n"
     <<"  \"bridge\": "<<path_string(bridge)<<",\n"
     <<"  \"sdk_manifest\": "<<path_string(p.sdk_manifest)<<",\n"
     <<"  \"sdk_fnv64\": "<<json_string(p.sdk_manifest.empty()?std::string{}:FileHash64(p.sdk_manifest))<<",\n"
     <<"  \"native_os\": "<<json_string(Env("GEKKOAOT_NATIVE_OS","0"))<<",\n"
     <<"  \"secondary_modules\": [";
  for(std::size_t i=0;i<secondary_modules.size();++i) {
    if(i) out<<", ";
    out<<path_string(secondary_modules[i]);
  }
  out<<"]\n}\n";
  const auto destination=p.disc/"meta/aot-launch.json";
  const auto temporary=p.disc/"meta/aot-launch.json.tmp";
  WriteText(temporary,out.str());
  std::error_code ec;
  fs::rename(temporary,destination,ec);
  if(ec) { // Windows cannot replace an existing destination with rename.
    fs::remove(destination); fs::rename(temporary,destination);
  }
  std::cout<<"GEKKOAOT_AOT_LAUNCH_V233=1 manifest=\""<<destination.string()<<"\"\n";
}
int RunGame(Pipeline& p,const fs::path& image,const fs::path& module,const fs::path& bridge,
            const std::vector<fs::path>& secondary_modules,const fs::path& stop_file={}) {
  ApplyGraphicsEnv();

  std::vector<fs::path> args{
      fs::path("--module"), module,
      fs::path("--dol"), p.disc/"sys/main.dol",
      fs::path("--game-id"), fs::path(p.game_id),
      fs::path("--disc-image"), image,
      fs::path("--native-gx-bridge"), bridge,
  };
  if(!p.sdk_manifest.empty()) {
    args.emplace_back("--sdk-manifest");
    args.push_back(p.sdk_manifest);
  }
  if(fs::exists(p.disc/"sys/boot.bin")) {
    args.emplace_back("--boot-bin");
    args.push_back(p.disc/"sys/boot.bin");
  }
  if(fs::exists(p.disc/"sys/fst.bin")) {
    args.emplace_back("--fst-bin");
    args.push_back(p.disc/"sys/fst.bin");
  }
  if(fs::exists(p.disc/"sys/bi2.bin")) {
    args.emplace_back("--bi2-bin");
    args.push_back(p.disc/"sys/bi2.bin");
  }
  for(const auto& secondary:secondary_modules) {
    args.emplace_back("--secondary-module");
    args.push_back(secondary);
  }
  if(!stop_file.empty()) {
    args.emplace_back("--stop-file");
    args.push_back(stop_file);
  }

  // The normal GUI/runtime path must not be reparsed by cmd.exe on Windows.
  // Keep shell execution only for explicit developer wrappers/extra arguments
  // such as GDB/perf, where shell syntax is intentionally requested.
  const auto run_prefix=Env("GEKKOAOT_RUN_PREFIX");
  const auto extra=Env("GEKKOAOT_RUN_ARGS");
  std::cout<<"GEKKOAOT_STAGE=runtime\nGEKKOAOT_PROGRESS=100|Game running\n"<<std::flush;
  if(run_prefix.empty() && extra.empty())
    return RunProcess(p.native_run,args);

  std::ostringstream cmd;
  if(!run_prefix.empty()) cmd<<run_prefix<<' ';
  cmd<<Quote(p.native_run);
  for(const auto& arg:args) cmd<<' '<<Quote(arg);
  if(!extra.empty()) cmd<<' '<<extra;
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

  // Keep the manifest as an inspectable hot-function report. It must not
  // rewrite translated IR after collection: IRUse owns the compiler policy.
  (void)BuildAdaptivePgoManifest(p,merged_profile,profdata);

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

  std::cout<<"[6/6] Building the PGO module\n"<<std::flush;
  auto use_plan=PgoCompilePlan(PgoMode::Use,merged_profile);
  const auto optimized=CompileModuleVariant(p,dolrecomp,"llvm","perf192-standard-pgo-use",p.sdk_manifest,
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
  WriteText(pgo_dir/"compile-context-v193.txt",PgoIRIdentity(p)+"\n");

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
  const auto training=CompileModuleVariant(p,dolrecomp,"llvm","perf192-standard-pgo-gen",p.sdk_manifest,
      PgoCompilePlan(PgoMode::Generate,raw_template),true);
  const auto secondary_modules=BuildSecondaryModules(p,dolrecomp);

  std::cout<<"[2/6] PGO training has started\n"
             "GEKKOAOT_PGO_V18=1 mode=training stop-file=\""<<stop_file.string()
           <<"\" profile-template=\""<<raw_template.string()<<"\" collector="
           <<(sampling.enabled?"sampled-ir":"full-ir")
           <<" prior-profile-applied=0 reason=llvm-irinstr-and-iruse-are-exclusive\n"<<std::flush;
  // C instrumentation can supply its own weak default filename. Select one
  // explicit destination for every translation unit before loading the DSO.
  const auto previous_profile_file=Env("LLVM_PROFILE_FILE");
  SetEnvValue("LLVM_PROFILE_FILE",raw_template.string());
  const int run_rc=RunGame(p,image,training,bridge,secondary_modules,stop_file);
  SetEnvValue("LLVM_PROFILE_FILE",previous_profile_file);

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
    const bool native_port_build = action == "build" && argc == 4 &&
        std::string_view(argv[2]) == "--target" && std::string_view(argv[3]) == "native-port";
    const bool native_port_audit = action == "native-port-audit" || native_port_build;
    if (action == "build" && !native_port_build)
      throw std::runtime_error("usage: gekkoaotctl build --target native-port");
    if (action != "status" && action != "clean" && action != "tools" &&
        action != "inspect" && action != "compile" && action != "run" &&
        action != "pgo" && action != "pgo-recover" && !native_port_audit)
      throw std::runtime_error("unknown action: " + action);
    if(action=="status") {
      std::cout<<"GekkoAOT v"<<GEKKOAOT_VERSION
               <<"\npython=not-required\ndolrecomp="<<GEKKOAOT_DOLRECOMP_REV
               <<"\nrenderer=gekkoaot-native-fifo/aurora\n";
      return 0;
    }

    const auto root=Root();
    ConfigureBundledToolchain(root);
    if(action=="clean") {
      GekkoAOT::Build::PipelineLock pipeline_lock(DefaultStateRoot(root));
      std::error_code ec;
      fs::remove_all(root/".gekkoaot/cache",ec);
      std::cout<<"GEKKOAOT_PROGRESS=100|Cache cleaned\n";
      return ec?1:0;
    }

    if(action=="tools") {
      Pipeline p(root/".gekkoaot/toolchain-placeholder.rvz");
      GekkoAOT::Build::PipelineLock pipeline_lock(p.state);
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
    GekkoAOT::Build::PipelineLock pipeline_lock(p.state);
    const std::string backend=Env("GEKKOAOT_BACKEND","llvm");
    if(backend!="c"&&backend!="llvm") throw std::runtime_error("GEKKOAOT_BACKEND must be c or llvm");
    std::cout<<"GEKKOAOT_CONTROLLER_VERSION="<<GEKKOAOT_VERSION
             <<"\nGEKKOAOT_BACKEND="<<backend
             <<"\nGEKKOAOT_PROGRESS=2|Preparing native toolchain\n";
    EnsureSources(p);
    if (native_port_audit && backend != "llvm")
      throw std::runtime_error("native-port analysis requires GEKKOAOT_BACKEND=llvm");
    const auto shared_dolrecomp=BuildDolRecomp(p,backend);
    if (native_port_audit) {
      // This path analyzes source only: no renderer build, module relink,
      // gameplay launch, input injection or memory-card access.
      GekkoAOT::Build::PipelineLock disc_lock(p.state/"build-locks"/("disc-"+TextHash64(p.disc.string())));
      ExtractDisc(p,image);
      (void)AuditNativePort(p,shared_dolrecomp);
      if (native_port_build)
        throw std::runtime_error("native-port executable unavailable: hardware services and whole-program coverage remain unproven; see side-exits.json");
      return 0;
    }
    const auto shared_bridge=BuildAuroraBridge(p);
    const auto concurrent=StageConcurrentToolchain(p,shared_dolrecomp,shared_bridge);
    p.dolrecomp_src=concurrent.dolrecomp_src;
    const auto dolrecomp=concurrent.dolrecomp;
    const auto bridge=concurrent.bridge;
    pipeline_lock.Release();
    std::cout<<"GEKKOAOT_PIPELINE_LOCK_V196=0 phase=game-aot reason=immutable-toolchain-snapshot\n";
    std::cout<<"GEKKOAOT_PROGRESS=20|Native SDK and AuroraGX ready\n";
    // Separate games compile concurrently. Requests for the same extraction
    // directory or game module cache must not mutate those files concurrently,
    // including requests from another launcher or a command-line process.
    GekkoAOT::Build::PipelineLock disc_lock(p.state/"build-locks"/("disc-"+TextHash64(p.disc.string())));
    ExtractDisc(p,image);

    if(action=="inspect") {
      std::cout<<"Disc ID: "<<p.game_id<<"\nGEKKOAOT_PROGRESS=100|Inspection complete\n";
      return 0;
    }

    GekkoAOT::Build::PipelineLock game_lock(p.state/"build-locks"/("game-"+TextHash64(p.game_id)));

    if(action=="pgo-recover") {
      if(backend!="llvm") throw std::runtime_error("PGO recovery requires GEKKOAOT_BACKEND=llvm");
      return RecoverPgo(p,dolrecomp);
    }
    if(action=="pgo") return TrainPgo(p,image,dolrecomp,bridge);

    const auto module=CompileModule(p,dolrecomp,backend,false);
    const auto secondary_modules=BuildSecondaryModules(p,dolrecomp);
    WriteAotLaunchMetadata(p,image,module,bridge,secondary_modules);
    game_lock.Release();
    disc_lock.Release();
    if(action=="compile") return 0;
    if(action=="run") {
      return RunGame(p,image,module,bridge,secondary_modules);
    }
    throw std::runtime_error("unknown action: "+action);
  } catch(const std::exception& e) {
    std::cerr<<"[gekkoaot] error: "<<e.what()<<'\n';
    return 1;
  }
}
