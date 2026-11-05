// SPDX-License-Identifier: GPL-3.0-or-later
#include "native/cheats.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
namespace fs = std::filesystem;
using GekkoAOT::Native::CheatEngine;
static void require(bool ok, const char* message) { if(!ok) throw std::runtime_error(message); }
int main() {
  const fs::path path=fs::temp_directory_path()/"gekkoaot-cheat-reload-test.txt";
  try {
    auto write=[&](const char* content) {
      std::ofstream out(path,std::ios::trunc); out<<content; out.close();
      // Simulate QSaveFile atomic replacement with a distinct mtime.
      static unsigned timeStep=0;
      fs::last_write_time(path, fs::last_write_time(path)+std::chrono::seconds(++timeStep));
    };
    CheatEngine cheats;
    std::string error;
    write("04000100 12345678\n");
    require(cheats.Load(path,&error) && cheats.Count()==1,"initial cheat load failed");
    write("04000100 12345678\n02000200 0000ABCD\n");
    require(cheats.ReloadIfChanged(&error) && cheats.Count()==2,"hot reload failed");
    write("C2000100 00000001\n");
    require(!cheats.ReloadIfChanged(&error) && cheats.Count()==2,"invalid edit erased valid active cheats");
    write("");
    require(cheats.ReloadIfChanged(&error) && cheats.Count()==0,"disable all did not reload");
    fs::remove(path);
    std::cout<<"PASS live Gecko reload, disable, invalid-edit preservation\n";
    return 0;
  } catch(const std::exception& e) {
    fs::remove(path);
    std::cerr <<"FAIL: "<<e.what()<<'\n'; return 1;
  }
}
