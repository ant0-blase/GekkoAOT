// SPDX-License-Identifier: GPL-3.0-or-later
#include "../../tools/source_timestamps.h"
#include <chrono>
#include <iostream>
#include <cstdlib>

#define CHECK(x) do { if (!(x)) { std::cerr << __LINE__ << ": " #x "\n"; std::exit(1); } } while (false)
namespace fs = std::filesystem;
static void Write(const fs::path& path, const char* text) {
  fs::create_directories(path.parent_path());
  std::ofstream stream(path); stream << text; CHECK(bool(stream));
}
int main(int argc, char** argv) {
  CHECK(argc == 2);
  const fs::path root(argv[1]); fs::remove_all(root); fs::create_directories(root);
  const auto source = root / "src/emitter.cpp";
  const auto changed = root / "lib/gx.cpp";
  const auto nested = root / "dependency/lib/dependency.cpp";
  Write(source, "current patch\n"); Write(changed, "old patch\n");
  Write(root / "dependency/.git", "gitdir: ../.git/modules/dependency\n");
  Write(nested, "dependency\n");
  const auto old = fs::file_time_type::clock::now() - std::chrono::hours(24);
  fs::last_write_time(source, old); fs::last_write_time(changed, old);
  fs::last_write_time(nested, old);
  const GekkoAOT::Build::SourceTimestamps timestamps(root);
  // A reset and identical reapplication must keep Ninja's input timestamp.
  Write(source, "upstream\n"); Write(source, "current patch\n");
  Write(changed, "new patch\n"); Write(nested, "dependency\n");
  const auto changed_time = fs::last_write_time(changed);
  const auto nested_time = fs::last_write_time(nested);
  CHECK(timestamps.RestoreUnchanged() == 1);
  CHECK(fs::last_write_time(source) == old);
  CHECK(fs::last_write_time(changed) == changed_time);
  CHECK(fs::last_write_time(nested) == nested_time);
  // Removed and newly introduced sources must never be hidden by restoration.
  fs::remove(source); Write(root / "src/new.cpp", "new\n");
  CHECK(timestamps.RestoreUnchanged() == 0);
  fs::remove_all(root);
  std::cout << "Source preparation timestamp regression passed\n";
}
