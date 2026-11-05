// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cerrno>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <thread>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX 1
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace GekkoAOT::Build {
// Only mutable source reset/patching and shared toolchain builds are serialized.
// Game AOT uses immutable toolchain snapshots and runs without this lock, so
// separate GekkoAOT processes may compile different titles concurrently.
class PipelineLock {
public:
  explicit PipelineLock(const std::filesystem::path& state) {
    std::filesystem::create_directories(state);
    const auto path = state / "pipeline.lock";
#ifdef _WIN32
    bool reported = false;
    for (;;) {
      handle_ = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0,
                            nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
      if (handle_ != INVALID_HANDLE_VALUE) break;
      const auto error = GetLastError();
      if (error != ERROR_SHARING_VIOLATION && error != ERROR_LOCK_VIOLATION)
        throw std::runtime_error("cannot open pipeline lock: " + path.string());
      if (!reported) { ReportWait(path); reported = true; }
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
#else
    fd_ = open(path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0666);
    if (fd_ < 0) throw std::runtime_error("cannot open pipeline lock: " + path.string());
    if (flock(fd_, LOCK_EX | LOCK_NB) != 0) {
      if (errno != EWOULDBLOCK && errno != EAGAIN && errno != EINTR) {
        Release();
        throw std::runtime_error("cannot acquire pipeline lock: " + path.string());
      }
      ReportWait(path);
      while (flock(fd_, LOCK_EX) != 0) {
        if (errno == EINTR) continue;
        Release();
        throw std::runtime_error("cannot acquire pipeline lock: " + path.string());
      }
    }
#endif
  }
  PipelineLock(const PipelineLock&) = delete;
  PipelineLock& operator=(const PipelineLock&) = delete;
  ~PipelineLock() { Release(); }
  void Release() {
#ifdef _WIN32
    if (handle_ != INVALID_HANDLE_VALUE) { CloseHandle(handle_); handle_ = INVALID_HANDLE_VALUE; }
#else
    if (fd_ >= 0) { close(fd_); fd_ = -1; }
#endif
  }

private:
  static void ReportWait(const std::filesystem::path& path) {
    std::cout << "GEKKOAOT_PIPELINE_LOCK_WAIT=1 path=\"" << path.string()
              << "\" reason=another-native-build\n" << std::flush;
  }
#ifdef _WIN32
  HANDLE handle_ = INVALID_HANDLE_VALUE;
#else
  int fd_ = -1;
#endif
};
}
