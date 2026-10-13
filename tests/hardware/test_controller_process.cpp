// SPDX-License-Identifier: GPL-3.0-or-later
// Exercise the actual controller and secondary helper, including Windows paths.
#ifndef GEKKOAOT_SOURCE_DIR
#define GEKKOAOT_SOURCE_DIR "."
#endif
#define GEKKOAOT_VERSION "probe"
#define GEKKOAOT_DOLRECOMP_REV "probe"
#define GEKKOAOT_AURORA_REV "probe"
#define main gekkoaot_controller_main_for_process_test
#include "../../tools/gekkoaotctl.cpp"
#undef main

#define CHECK(x) do { if (!(x)) throw std::runtime_error(#x); } while (false)

int main(int argc,char** argv) {
  if(argc>1 && std::string_view(argv[1])=="--arguments") {
    const std::vector<std::string> expected={"space value","quote\"value","trailing\\","a&b","(value)"};
    if(argc!=static_cast<int>(expected.size())+2) return 1;
    for(std::size_t i=0;i<expected.size();++i) if(argv[i+2]!=expected[i]) return 2;
    return 0;
  }
  if(argc>1 && std::string_view(argv[1])=="--exit") return 37;
  if(argc>1 && std::string_view(argv[1])=="--shell")
    return argc==3 && std::string_view(argv[2])=="space value" ? 0 : 3;
  try {
    CHECK(argc==3);
    const auto directory=fs::absolute(fs::path(argv[2])) /
        ("paths with spaces & (brackets)-"+std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(directory);
    const auto helper=directory/("native helper (copy)"+fs::path(argv[1]).extension().string());
    fs::copy_file(argv[1],helper);
    const auto disc=directory/"Harry Potter (USA) & disc";
    const auto out=directory/"cache with spaces"/"entrypoints"/"TEST01";
    fs::create_directories(disc/"sys");
    // Independently authored DOL with one executable blr instruction.
    std::string dol(0x104,'\0');
    auto word=[&](std::size_t offset,std::uint32_t value) {
      for(unsigned i=0;i<4;++i) dol[offset+i]=static_cast<char>(value>>(24u-8u*i));
    };
    word(0,0x100);word(0x48,0x80003100);word(0x90,4);
    word(0xe0,0x80003100);word(0x100,0x4e800020);
    WriteText(disc/"sys/main.dol",dol);
    CHECK(fs::is_regular_file(BuildRelToDolEntrypointManifest(helper,disc,out)));
    const auto self=fs::absolute(argv[0]);
    CHECK(RunProcess(self,{"--arguments","space value","quote\"value","trailing\\","a&b","(value)"})==0);
    CHECK(RunProcess(self,{"--exit"})==37);
    CHECK(Run(Quote(self)+" --shell "+QuoteText("space value"))==0);
    fs::remove_all(directory);
    std::cout<<"Controller process and entrypoint pre-scan regression passed\n";
    return 0;
  } catch(const std::exception& error) {
    std::cerr<<"Controller process test: "<<error.what()<<'\n';
    return 1;
  }
}
