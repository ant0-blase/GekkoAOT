// Exercise the controller's actual environment and PGO-directory policy.
#ifndef GEKKOAOT_SOURCE_DIR
#define GEKKOAOT_SOURCE_DIR "."
#endif
#ifndef GEKKOAOT_VERSION
#define GEKKOAOT_VERSION "probe"
#endif
#ifndef GEKKOAOT_DOLRECOMP_REV
#define GEKKOAOT_DOLRECOMP_REV "probe"
#endif
#ifndef GEKKOAOT_AURORA_REV
#define GEKKOAOT_AURORA_REV "probe"
#endif
#define main gekkoaot_controller_main_for_fp_policy_test
#include "../../tools/gekkoaotctl.cpp"
#undef main

#define CHECK(x) do { if (!(x)) { \
  std::cerr << "FP policy check failed: " << #x << '\n'; return 1; } } while (0)

int main() {
  const auto directory = fs::temp_directory_path() /
      ("gekkoaot-fp-policy-" + TextHash64(std::to_string(
          std::chrono::steady_clock::now().time_since_epoch().count())));
  Pipeline p("fp-policy.rvz");
  p.root = directory;
  p.cache = directory/"cache";
  p.disc = directory/"disc";
  p.dolrecomp_src = directory/"compiler";
  p.game_id = "TEST01";
  WriteText(p.disc/"sys/main.dol","test-dol");
  WriteText(p.dolrecomp_src/"src/cpu/cpu.c","test-cpu");
  WriteText(p.root/"runtime/module/module_export.c","test-glue");
  WriteText(p.root/"runtime/module/native_hardware_shims.inc","test-shims");
  WriteText(p.root/"runtime/core/native_hardware.h","test-abi");
  SetEnvValue("GEKKOAOT_FP_REGION_COALESCE","0");
  CHECK(!FPRegionCoalesceEnabled());
  const auto offIdentity = PgoIRIdentity(p);
  CHECK(offIdentity.find("fp-region-coalesce") == std::string::npos);
  const auto legacy = p.cache/"pgo"/p.game_id/
      Env("GEKKOAOT_PGO_COMPAT_ID","perf192-standard-ir-c-pgo");
  WriteText(legacy/"compile-context-v193.txt",offIdentity+"\n");
  WriteText(legacy/"merged.profdata","retained-old-profile");
  CHECK(PgoDirectory(p) == legacy);

  SetEnvValue("GEKKOAOT_FP_REGION_COALESCE","1");
  CHECK(FPRegionCoalesceEnabled());
  const auto onIdentity = PgoIRIdentity(p);
  CHECK(onIdentity != offIdentity);
  CHECK(onIdentity.find("fp-region-coalesce=292-on") != std::string::npos);
  const auto fresh = PgoDirectory(p);
  CHECK(fresh != legacy);
  CHECK(!fs::exists(PgoMergedProfile(p)));
  CHECK(ReadText(legacy/"merged.profdata") == "retained-old-profile");
  WriteText(fresh/"merged.profdata","fresh-v292-profile");
  CHECK(ReadText(PgoMergedProfile(p)) == "fresh-v292-profile");

  SetEnvValue("GEKKOAOT_FP_REGION_COALESCE","0");
  CHECK(PgoDirectory(p) == legacy);
  CHECK(ReadText(PgoMergedProfile(p)) == "retained-old-profile");
  SetEnvValue("GEKKOAOT_FP_REGION_COALESCE","");
  CHECK(FPRegionCoalesceEnabled());
  CHECK(PgoDirectory(p) == fresh);
  SetEnvValue("GEKKOAOT_NATIVE_SCALAR_MATH","0");
  const auto oldMath=PgoIRIdentity(p);
  SetEnvValue("GEKKOAOT_NATIVE_SCALAR_MATH","1");
  CHECK(PgoIRIdentity(p)!=oldMath);
  CHECK(PgoIRIdentity(p).find("native-scalar-math=316-on")!=std::string::npos);
  // Native scalar math is on by default: the opt-out owns a separate profile.
  CHECK(PgoDirectory(p)==fresh);
  SetEnvValue("GEKKOAOT_NATIVE_SCALAR_MATH","0");
  CHECK(PgoDirectory(p)!=fresh);
  SetEnvValue("GEKKOAOT_NATIVE_SCALAR_MATH","");
  CHECK(PgoDirectory(p)==fresh);
  fs::remove_all(directory);
  std::cout << "FP_REGION_POLICY_PASSED defaults=on optout=independent pgo=isolated old-profile=retained\n";
  return 0;
}
