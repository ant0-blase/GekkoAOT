// SPDX-License-Identifier: GPL-3.0-or-later
#include "native/atomic_file.h"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <string>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#undef ReplaceFile // Win32 maps it to ReplaceFileW; ours is GekkoAOT::Native::ReplaceFile
#else
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#endif
namespace GekkoAOT::Native {
bool ReplaceFile(const std::filesystem::path &path,
                 std::span<const std::uint8_t> bytes,
                 const std::atomic<bool> *cancel) {
  if (cancel && cancel->load(std::memory_order_acquire))
    return false;
  if (path.empty())
    return false;
  std::error_code ec;
  if (!path.parent_path().empty())
    std::filesystem::create_directories(path.parent_path(), ec);
  if (ec)
    return false;
  static std::atomic<unsigned> sequence{0};
#if defined(_WIN32)
  auto temporary = path;
  temporary += L".native-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
               std::to_wstring(sequence.fetch_add(1)) + L".tmp";
  HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr,
                            CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE)
    return false;
  bool ok = true;
  std::size_t done = 0;
  while (done < bytes.size()) {
    DWORD n = 0;
    const DWORD count = static_cast<DWORD>(
        std::min<std::size_t>(bytes.size() - done, 0x40000000));
    if (!WriteFile(file, bytes.data() + done, count, &n, nullptr) || !n) {
      ok = false;
      break;
    }
    done += n;
  }
  if (ok)
    ok = FlushFileBuffers(file) != 0;
  if (!CloseHandle(file))
    ok = false;
  if (ok && cancel && cancel->load(std::memory_order_acquire))
    ok = false;
  if (ok)
    ok = MoveFileExW(temporary.c_str(), path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
  if (!ok)
    DeleteFileW(temporary.c_str());
#else
  auto temporary = path;
  temporary += ".native-" + std::to_string(getpid()) + "-" +
               std::to_string(sequence.fetch_add(1)) + ".tmp";
  const int file =
      open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
  if (file < 0)
    return false;
  bool ok = true;
  std::size_t done = 0;
  while (done < bytes.size()) {
    const auto n = write(file, bytes.data() + done, bytes.size() - done);
    if (n < 0 && errno == EINTR)
      continue;
    if (n <= 0) {
      ok = false;
      break;
    }
    done += static_cast<std::size_t>(n);
  }
  if (ok)
    ok = fsync(file) == 0;
  if (close(file) != 0)
    ok = false;
  if (ok && cancel && cancel->load(std::memory_order_acquire))
    ok = false;
  if (ok)
    ok = rename(temporary.c_str(), path.c_str()) == 0;
  if (!ok)
    unlink(temporary.c_str());
  if (ok) {
    const auto parent = path.parent_path().empty() ? std::filesystem::path(".")
                                                   : path.parent_path();
    const int dir = open(parent.c_str(), O_RDONLY | O_CLOEXEC | O_DIRECTORY);
    if (dir >= 0) {
      fsync(dir);
      close(dir);
    }
  }
#endif
  return ok;
}
} // namespace GekkoAOT::Native
