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
#include <unordered_set>
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

std::uint32_t Be32(const std::uint8_t* p) {
  return (std::uint32_t(p[0]) << 24u) | (std::uint32_t(p[1]) << 16u) |
         (std::uint32_t(p[2]) << 8u) | std::uint32_t(p[3]);
}

// v352: extent of a raw REL image starting at `at`, or 0 when the bytes are
// not a self-consistent REL (header, section table, import table and every
// relocation list must lie inside the container).
std::size_t EmbeddedRelExtent(const std::vector<std::uint8_t>& bytes, std::size_t at) {
  const std::size_t avail = bytes.size() - at;
  if (avail < 0x4cu) return 0;
  const std::uint8_t* h = bytes.data() + at;
  const std::uint32_t id = Be32(h), next = Be32(h + 4), prev = Be32(h + 8);
  const std::uint32_t sections = Be32(h + 12), section_info = Be32(h + 16), version = Be32(h + 28);
  if (id == 0u || id > 0xffffu || next || prev || sections < 2u || sections > 64u ||
      version < 1u || version > 3u)
    return 0;
  if (section_info != (version == 3u ? 0x4cu : version == 2u ? 0x48u : 0x40u)) return 0;
  const std::uint32_t relocations = Be32(h + 0x24), imports = Be32(h + 0x28), import_size = Be32(h + 0x2c);
  if (!relocations || !imports || !import_size || import_size % 8u ||
      std::uint64_t(section_info) + sections * 8ull > avail ||
      std::uint64_t(imports) + import_size > avail || relocations >= avail)
    return 0;
  std::uint64_t end = std::max<std::uint64_t>(section_info + sections * 8ull, imports + import_size);
  bool executable = false;
  for (std::uint32_t i = 0; i < sections; ++i) {
    const std::uint32_t word = Be32(h + section_info + i * 8u);
    const std::uint32_t offset = word & ~3u;
    const std::uint32_t size = Be32(h + section_info + i * 8u + 4u);
    if (!offset) continue; // bss or empty section
    if (std::uint64_t(offset) + size > avail) return 0;
    executable |= (word & 1u) != 0u && size != 0u;
    end = std::max<std::uint64_t>(end, std::uint64_t(offset) + size);
  }
  if (!executable) return 0;
  for (std::uint32_t i = 0; i < import_size / 8u; ++i) {
    std::uint64_t cursor = Be32(h + imports + i * 8u + 4u);
    if (cursor < relocations) return 0;
    for (;;) {
      if (cursor + 8u > avail) return 0;
      const std::uint8_t type = h[cursor + 2u];
      if (type > 13u && (type < 201u || type > 203u)) return 0;
      cursor += 8u;
      if (type == 203u) break; // R_DOLPHIN_END
    }
    end = std::max(end, cursor);
  }
  return static_cast<std::size_t>(end);
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
  std::size_t embedded_rels = 0;
  std::unordered_set<std::uint64_t> embedded_hashes;
  bool banner_cached = false;
  bool metadata_only = false;
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
  if (state.metadata_only) return index + 1u;

  // Cache every explicit executable container plus .bin candidates.  The
  // secondary-module builder validates .bin by file signature before treating
  // it as PPC code, so ordinary assets remain inert even though they are made
  // available for classification.
  if (extension == ".rel" || extension == ".elf" || extension == ".dol" ||
      extension == ".bin" ||
      ((extension == ".arc" || extension == ".szs") && lower.find("rel") != std::string::npos)) {
    if (!CopyFile(state.partition, index, size, state.output / "files" / relative, &state.error)) {
      state.ok = false;
      return NOD_FST_STOP;
    }
    ++state.executable_count;
    return index + 1u;
  }

  // v352: some titles pack their REL modules inside proprietary archives
  // (RE4 keeps ~120 enemy/weapon RELs in .drs files). Stage every raw REL
  // image found in a non-stream file as its own .rel so the secondary builder
  // compiles it like a plain disc REL. Streamed media cannot hold code.
  static const std::array<std::string_view, 12> kStreams{
      ".thp", ".sfd", ".str", ".adp", ".ast", ".dsp", ".h4m", ".bik", ".hps", ".mpc", ".afc", ".pcm"};
  if (size >= 0x4cu && size <= 256u * 1024u * 1024u &&
      std::find(kStreams.begin(), kStreams.end(), extension) == kStreams.end()) {
    NodHandle* raw = nullptr;
    if (nod_partition_open_file(state.partition, index, &raw) == NOD_RESULT_OK && raw) {
      Handle file(raw);
      std::vector<std::uint8_t> bytes(size);
      std::size_t got = 0;
      while (got < bytes.size()) {
        const std::int64_t n = nod_read(file.value, bytes.data() + got, bytes.size() - got);
        if (n <= 0) break;
        got += static_cast<std::size_t>(n);
      }
      bytes.resize(got);
      for (std::size_t at = 0; at + 0x4cu <= bytes.size(); at += 4u) {
        const std::size_t extent = EmbeddedRelExtent(bytes, at);
        if (!extent) continue;
        std::uint64_t hash = 1469598103934665603ull;
        for (std::size_t i = 0; i < extent; ++i) hash = (hash ^ bytes[at + i]) * 1099511628211ull;
        if (!state.embedded_hashes.insert(hash).second) continue;
        std::string flat = relative.generic_string();
        std::replace(flat.begin(), flat.end(), '/', '_');
        const fs::path destination = state.output / "files/.embedded-rel" /
            (flat + "@" + std::to_string(at) + ".rel");
        std::error_code ec;
        fs::create_directories(destination.parent_path(), ec);
        std::ofstream out(destination, std::ios::binary | std::ios::trunc);
        if (!out) continue;
        out.write(reinterpret_cast<const char*>(bytes.data() + at), static_cast<std::streamsize>(extent));
        if (out) ++state.embedded_rels;
      }
    }
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

int DiscMain(const fs::path& image_arg, const fs::path& output_arg, bool metadata_only = false) {
  const fs::path image = fs::absolute(image_arg);
  const fs::path output = fs::absolute(output_arg);
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
      (!metadata_only && !WriteBlob(output / "sys/main.dol", meta.raw_dol)) ||
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
  state.metadata_only = metadata_only;
  if (metadata_only) state.executable_count = 0;
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
  if (!metadata_only && !WriteText(output / "meta/exec-cache-policy.txt", "v5-embedded-rel\n")) {
    std::cerr << "could not write executable cache policy\n";
    return 1;
  }

  std::cout << "GEKKOAOT_DISC_METADATA_ONLY=" << metadata_only << "\n"
            << "GEKKOAOT_NOD_DISC=1 format=" << format
            << " logical_size=" << nod_disc_size(disc.value) << '\n'
            << "Disc ID: " << game_id << '\n'
            << "Game: " << game_name << '\n'
            << "Executable files cached: " << state.executable_count << '\n'
            << "GEKKOAOT_EMBEDDED_REL_V352=1 staged=" << state.embedded_rels << '\n'
            << "FST bytes cached: " << meta.raw_fst.size << '\n'
            << "opening.bnr: " << (state.banner_cached ? "cached" : "not present") << '\n';
  return 0;
}

#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
  if (argc == 4 && std::wstring_view(argv[1]) == L"--metadata")
    return DiscMain(fs::path(argv[2]), fs::path(argv[3]), true);
  if (argc != 3) {
    std::cerr << "usage: gekkoaot-native-disc [--metadata] <disc-image> <cache-root>\n";
    return 2;
  }
  return DiscMain(fs::path(argv[1]), fs::path(argv[2]));
}
#else
int main(int argc, char** argv) {
  if (argc == 4 && std::string_view(argv[1]) == "--metadata")
    return DiscMain(fs::path(argv[2]), fs::path(argv[3]), true);
  if (argc != 3) {
    std::cerr << "usage: gekkoaot-native-disc [--metadata] <disc-image> <cache-root>\n";
    return 2;
  }
  return DiscMain(fs::path(argv[1]), fs::path(argv[2]));
}
#endif
