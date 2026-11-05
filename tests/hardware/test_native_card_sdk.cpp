// SPDX-License-Identifier: GPL-3.0-or-later
#include "card/sdk_card.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#define CHECK(x)                                                               \
  do {                                                                         \
    if (!(x)) {                                                                \
      std::cerr << __LINE__ << ": " #x "\n";                                   \
      return 1;                                                                \
    }                                                                          \
  } while (false)
using namespace GekkoAOT;
int main(int argc, char** argv) {
  // Read-only validation of a real RAW fixture; never repairs or commits it.
  if (argc == 2) {
    if (!std::filesystem::is_regular_file(argv[1])) return 2;
    NativeHW::EXI::HostCard image(argv[1], 0);
    Card::SdkCard card;
    const auto result = card.Mount(&image);
    std::cout << "RAW mount=" << result << " capacity=" << card.Capacity()
              << " free_bytes=" << (result ? -1 : card.FreeBytes()) << '\n';
    return result != 0;
  }
  const auto root =
      std::filesystem::temp_directory_path() /
      ("gekkoaot-native-card-" +
       std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()));
  std::filesystem::create_directories(root);
  struct Cleanup {
    std::filesystem::path p;
    ~Cleanup() {
      std::error_code ec;
      std::filesystem::remove_all(p, ec);
    }
  } cleanup{root};
  NativeHW::EXI::HostCard image(root / "A.raw", 0), imageB(root / "B.raw", 1);
  Card::SdkCard a, b;
  const std::array<std::uint8_t, 6> id{'G', 'T', 'E', 'S', '0', '1'};
  a.SetDisk(id);
  b.SetDisk(id);
  CHECK(a.Mount(&image) == 0);
  CHECK(b.Mount(&imageB) == 0);
  CHECK(a.Check() == 0);
  CHECK(a.MetadataCopiesValid());
  image.Image()[8192+0x1ffc]^=1;
  CHECK(!a.MetadataCopiesValid()); // validation must not pretend a repair
  image.Image()[8192+0x1ffc]^=1;
  CHECK(a.MetadataCopiesValid());
  CHECK(a.FreeBytes() == 251 * 8192);
  CHECK(a.FreeFiles() == 127);
  Card::SdkCard::Info info;
  const std::array<std::uint8_t, 4> name{'s', 'a', 'v', 'e'};
  CHECK(a.Create(name, 3 * 8192, 123, info) == 0);
  CHECK(a.Submit());
  CHECK(a.Result() == -1);
  CHECK(a.Wait() == 0);
  CHECK(info.file == 0);
  CHECK(a.Create(name, 8192, 0, info) == -7);
  CHECK(a.Open(name, info) == 0);
  std::array<std::uint8_t, 3 * 8192> data{}, out{};
  for (unsigned i = 0; i < data.size(); ++i)
    data[i] = i * 19;
  CHECK(a.Write(info, data, 0) == 0);
  CHECK(a.Submit());
  CHECK(a.Wait() == 0);
  CHECK(a.Read(info, out, 0) == 0);
  CHECK(out == data);
  {
    NativeHW::EXI::HostCard reopened(root / "A.raw",0);
    Card::SdkCard loaded;loaded.SetDisk(id);
    CHECK(loaded.Mount(&reopened)==0);Card::SdkCard::Info saved;
    CHECK(loaded.Open(name,saved)==0);CHECK(loaded.Read(saved,out,0)==0);
    CHECK(out==data);
  }
  CHECK(a.Read(info, std::span(out).first(512), 512) == 0);
  CHECK(std::equal(out.begin(), out.begin() + 512, data.begin() + 512));
  CHECK(a.Read(info, std::span(out).first(512), 3 * 8192) == -11);
  CHECK(a.Read(info, std::span(out).first(511), 0) == -128);
  CHECK(a.Write(info, std::span(data).first(8192), 1) == -128);
  CHECK(b.Open(name, info) == -4);
  CHECK(a.Open(name, info) == 0);
  const std::array<std::uint8_t, 6> other{'G', 'T', 'E', 'S', '0', '2'};
  a.SetDisk(other);
  CHECK(a.Open(name, info) == -4);
  CHECK(a.FastOpen(0, info) == 0); // public read
  CHECK(a.Write(info, data, 0) == -10);
  a.SetDisk(id);
  std::array<std::uint8_t, 108> stat{};
  CHECK(a.GetStatus(0, stat) == 0);
  CHECK(stat[39] == 123);
  CHECK(a.Check() == 0);
  // Worker result failure preserves the previous on-disk image and rolls back
  // both the native directory cache and the changed payload bytes.
  std::filesystem::rename(root / "A.raw", root / "old.raw");
  std::filesystem::create_directory(root / "A.raw");
  CHECK(a.Open(name, info) == 0);
  std::fill(data.begin(), data.end(), 0x7f);
  CHECK(a.Write(info, data, 0) == 0);
  CHECK(a.Submit());
  CHECK(a.Wait() == -5);
  CHECK(a.Read(info, out, 0) == 0);
  CHECK(out[1] == 19);
  CHECK(a.Check() == 0);
  std::filesystem::remove(root / "A.raw");
  std::filesystem::rename(root / "old.raw", root / "A.raw");
  CHECK(a.Delete(0) == 0);
  CHECK(a.Submit());
  CHECK(a.Wait() == 0);
  CHECK(a.FreeFiles() == 127);
  CHECK(a.FreeBytes() == 251 * 8192);
  CHECK(a.Unmount() == 0);
  CHECK(a.Mount(&image) == 0);
  CHECK(a.Open(name, info) == -4);
  // Corrupt chains with otherwise-valid checksums must not be accepted or
  // repaired implicitly. Original bytes remain unchanged on failed mount.
  image.Image()[0x1fc] ^= 1;
  const auto original = image.Image()[0x1fc];
  Card::SdkCard broken;
  CHECK(broken.Mount(&image) == -6);
  CHECK(image.Image()[0x1fc] == original);
  std::cout << "NativeCARD: slots, RAW round-trip, partial reads, permissions, "
               "limits, persistence failure rollback, corruption PASS\n";
}
