// SPDX-License-Identifier: GPL-3.0-or-later
#include <nod.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;

namespace {

struct Handle {
  NodHandle* value = nullptr;
  Handle() = default;
  explicit Handle(NodHandle* h) : value(h) {}
  Handle(const Handle&) = delete;
  Handle& operator=(const Handle&) = delete;
  ~Handle() { nod_free(value); }
};

std::string Lower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return value;
}

const char* FormatName(NodFormat format) {
  switch (format) {
  case NOD_FORMAT_ISO: return "ISO";
  case NOD_FORMAT_CISO: return "CISO";
  case NOD_FORMAT_GCZ: return "GCZ";
  case NOD_FORMAT_NFS: return "NFS";
  case NOD_FORMAT_RVZ: return "RVZ";
  case NOD_FORMAT_WBFS: return "WBFS";
  case NOD_FORMAT_WIA: return "WIA";
  case NOD_FORMAT_TGC: return "TGC";
  }
  return "unknown";
}

std::string JsonEscape(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (const unsigned char ch : text) {
    switch (ch) {
    case '\\': out += "\\\\"; break;
    case '"': out += "\\\""; break;
    case '\n': out += "\\n"; break;
    case '\r': out += "\\r"; break;
    case '\t': out += "\\t"; break;
    default:
      if (ch >= 0x20u) out.push_back(static_cast<char>(ch));
      break;
    }
  }
  return out;
}

bool WriteBlob(const fs::path& path, const NodBlob& blob) {
  if (!blob.data || blob.size == 0) return false;
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) return false;
  out.write(reinterpret_cast<const char*>(blob.data), static_cast<std::streamsize>(blob.size));
  return static_cast<bool>(out);
}

bool WriteText(const fs::path& path, std::string_view text) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) return false;
  out.write(text.data(), static_cast<std::streamsize>(text.size()));
  return static_cast<bool>(out);
}

bool SafeName(std::string_view name) {
  return !name.empty() && name != "." && name != ".." &&
         name.find('/') == std::string_view::npos &&
         name.find('\\') == std::string_view::npos;
}

bool CopyFile(NodHandle* partition, std::uint32_t index, std::uint32_t size,
              const fs::path& destination, std::string* error) {
  NodHandle* raw = nullptr;
  if (nod_partition_open_file(partition, index, &raw) != NOD_RESULT_OK || !raw) {
    if (error) *error = "nod_partition_open_file failed";
    return false;
  }
  Handle file(raw);

  std::error_code ec;
  fs::create_directories(destination.parent_path(), ec);
  if (ec) {
    if (error) *error = "could not create output directory: " + ec.message();
    return false;
  }

  std::ofstream out(destination, std::ios::binary | std::ios::trunc);
  if (!out) {
    if (error) *error = "could not create " + destination.string();
    return false;
  }

  std::array<std::uint8_t, 256u * 1024u> buffer{};
  std::uint64_t remaining = size;
  while (remaining) {
    const std::size_t request = static_cast<std::size_t>(std::min<std::uint64_t>(remaining, buffer.size()));
    const std::int64_t got = nod_read(file.value, buffer.data(), request);
    if (got <= 0) {
      if (error) *error = "short/error read from nod file stream";
      return false;
    }
    out.write(reinterpret_cast<const char*>(buffer.data()), static_cast<std::streamsize>(got));
    if (!out) {
      if (error) *error = "write failed for " + destination.string();
      return false;
    }
    remaining -= static_cast<std::uint64_t>(got);
  }
  return true;
}

struct DirFrame {
  std::uint32_t end_index = 0;
  fs::path path;
};

struct WalkState {
  NodHandle* partition = nullptr;
  fs::path output;
  std::vector<DirFrame> directories;
  std::size_t executable_count = 1; // main.dol is already part of partition metadata.
  bool banner_cached = false;
  bool ok = true;
  std::string error;
};

std::uint32_t WalkFst(std::uint32_t index, NodNodeKind kind, const char* raw_name,
                      std::uint32_t size, void* user_data) {
  auto& state = *static_cast<WalkState*>(user_data);
  if (!state.ok) return NOD_FST_STOP;

  while (!state.directories.empty() && index >= state.directories.back().end_index)
    state.directories.pop_back();

  const std::string_view name = raw_name ? std::string_view(raw_name) : std::string_view{};
  if (index == 0u && kind == NOD_NODE_KIND_DIRECTORY) {
    state.directories.push_back({size, {}});
    return 1u;
  }
  if (!SafeName(name)) {
    state.ok = false;
    state.error = "unsafe/invalid FST name";
    return NOD_FST_STOP;
  }

  const fs::path base = state.directories.empty() ? fs::path{} : state.directories.back().path;
  const fs::path relative = base / fs::path(std::string(name));
  if (kind == NOD_NODE_KIND_DIRECTORY) {
    if (size <= index) {
      state.ok = false;
      state.error = "invalid FST directory end index";
      return NOD_FST_STOP;
    }
    state.directories.push_back({size, relative});
    return index + 1u;
  }

  const std::string lower = Lower(std::string(name));
  const std::string extension = Lower(relative.extension().string());
  if (!state.banner_cached && lower == "opening.bnr") {
    if (!CopyFile(state.partition, index, size, state.output / "meta/opening.bnr", &state.error)) {
      state.ok = false;
      return NOD_FST_STOP;
    }
    state.banner_cached = true;
  }

  // Cache every explicit executable container plus .bin candidates.  The
  // secondary-module builder validates .bin by file signature before treating
  // it as PPC code, so ordinary assets remain inert even though they are made
  // available for classification.
  if (extension == ".rel" || extension == ".elf" || extension == ".dol" ||
      extension == ".bin") {
    if (!CopyFile(state.partition, index, size, state.output / "files" / relative, &state.error)) {
      state.ok = false;
      return NOD_FST_STOP;
    }
    ++state.executable_count;
  }
  return index + 1u;
}

std::string TrimTitle(const char (&title)[64]) {
  std::size_t len = 0;
  while (len < sizeof(title) && title[len] != '\0') ++len;
  while (len && (title[len - 1] == ' ' || title[len - 1] == '\0')) --len;
  return std::string(title, title + len);
}

} // namespace

int main(int argc, char** argv) {
  if (argc != 3) {
    std::cerr << "usage: gekkoaot-native-disc <disc-image> <exec-cache-root>\n";
    return 2;
  }

  const fs::path image = fs::absolute(argv[1]);
  const fs::path output = fs::absolute(argv[2]);
  const auto image_u8 = image.u8string();
  const std::string image_utf8(image_u8.begin(), image_u8.end());

  NodDiscOptions options{};
  options.preloader_threads = 2u;
  NodHandle* raw_disc = nullptr;
  if (nod_disc_open(image_utf8.c_str(), &options, &raw_disc) != NOD_RESULT_OK || !raw_disc) {
    const char* detail = nod_error_message();
    std::cerr << "nod could not open disc image" << (detail ? ": " : "")
              << (detail ? detail : "") << '\n';
    return 1;
  }
  Handle disc(raw_disc);

  NodDiscHeader header{};
  constexpr std::array<std::uint8_t, 4> kGameCubeMagic{0xc2u, 0x33u, 0x9fu, 0x3du};
  if (nod_disc_header(disc.value, &header) != NOD_RESULT_OK ||
      !std::equal(kGameCubeMagic.begin(), kGameCubeMagic.end(), header.gcn_magic)) {
    std::cerr << "disc is not a GameCube image\n";
    return 1;
  }

  NodDiscMeta disc_meta{};
  if (nod_disc_meta(disc.value, &disc_meta) != NOD_RESULT_OK) {
    std::cerr << "nod could not read disc metadata\n";
    return 1;
  }

  NodPartitionOptions partition_options{};
  NodHandle* raw_partition = nullptr;
  if (nod_disc_open_partition_kind(disc.value, NOD_PARTITION_KIND_DATA,
                                   &partition_options, &raw_partition) != NOD_RESULT_OK ||
      !raw_partition) {
    std::cerr << "nod could not open GameCube data partition\n";
    return 1;
  }
  Handle partition(raw_partition);

  NodPartitionMeta meta{};
  if (nod_partition_meta(partition.value, &meta) != NOD_RESULT_OK ||
      meta.raw_boot.size < 0x440u || meta.raw_dol.size == 0u || meta.raw_fst.size < 12u) {
    std::cerr << "nod returned incomplete GameCube partition metadata\n";
    return 1;
  }

  std::error_code ec;
  fs::remove_all(output, ec);
  ec.clear();
  fs::create_directories(output / "sys", ec);
  fs::create_directories(output / "files", ec);
  fs::create_directories(output / "meta", ec);
  if (ec) {
    std::cerr << "could not create executable cache: " << ec.message() << '\n';
    return 1;
  }

  if (!WriteBlob(output / "sys/boot.bin", meta.raw_boot) ||
      !WriteBlob(output / "sys/main.dol", meta.raw_dol) ||
      !WriteBlob(output / "sys/fst.bin", meta.raw_fst)) {
    std::cerr << "could not write required GameCube system files\n";
    return 1;
  }
  if ((meta.raw_bi2.data && meta.raw_bi2.size &&
       !WriteBlob(output / "sys/bi2.bin", meta.raw_bi2)) ||
      (meta.raw_apploader.data && meta.raw_apploader.size &&
       !WriteBlob(output / "sys/apploader.img", meta.raw_apploader))) {
    std::cerr << "could not write optional GameCube system files\n";
    return 1;
  }

  WalkState state{};
  state.partition = partition.value;
  state.output = output;
  nod_partition_iterate_fst(partition.value, &WalkFst, &state);
  if (!state.ok) {
    std::cerr << "could not cache GameCube FST files: " << state.error << '\n';
    return 1;
  }

  const std::string game_id(header.game_id, header.game_id + 6);
  const std::string game_name = TrimTitle(header.game_title);
  const std::string format = FormatName(disc_meta.format);
  if (!WriteText(output / "meta/disc-id.txt", game_id + "\n") ||
      !WriteText(output / "meta/game-name.txt", game_name + "\n")) {
    std::cerr << "could not write disc metadata cache\n";
    return 1;
  }

  const std::string source_json =
      "{\n  \"version\": 2,\n  \"reader\": \"encounter/nod\",\n  \"source\": \"" +
      JsonEscape(image_utf8) + "\",\n  \"format\": \"" + format +
      "\",\n  \"game_id\": \"" + JsonEscape(game_id) + "\"\n}\n";
  if (!WriteText(output / "meta/disc-source.json", source_json)) {
    std::cerr << "could not write nod source metadata\n";
    return 1;
  }

  std::cout << "GEKKOAOT_NOD_DISC=1 format=" << format
            << " logical_size=" << nod_disc_size(disc.value) << '\n'
            << "Disc ID: " << game_id << '\n'
            << "Game: " << game_name << '\n'
            << "Executable files cached: " << state.executable_count << '\n'
            << "FST bytes cached: " << meta.raw_fst.size << '\n'
            << "opening.bnr: " << (state.banner_cached ? "cached" : "not present") << '\n';
  return 0;
}
