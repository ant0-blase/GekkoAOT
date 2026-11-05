// SPDX-License-Identifier: GPL-3.0-or-later
#include "native/runtime.h"
#include <algorithm>
#include <cstring>
#include <cstdio>
#include <cstdlib>
namespace GekkoAOT::Native {
namespace {
std::uint32_t U32(const std::uint8_t *p) {
  return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) |
         (std::uint32_t(p[2]) << 8) | p[3];
}
bool TraceDvd() {
  static const bool enabled=[]{const auto* v=std::getenv("GEKKOAOT_TRACE_DVD");return v && std::string_view(v)=="1";}();
  return enabled;
}
void P32(std::uint8_t *p, std::uint32_t v) {
  p[0] = v >> 24;
  p[1] = v >> 16;
  p[2] = v >> 8;
  p[3] = v;
}
} // namespace
bool HostRuntime::ServiceDvdSdkEvents() {
  auto &d = devices_;
  // Respect callback order: don't start a successor until its predecessor's
  // callback has returned. No staging-buffer write can race cancellation.
  if (!(cpu_.msr & 0x8000u))
    return false;
  if (d.dvd_cancel_callback) {
    const auto callback = d.dvd_cancel_callback, block = d.dvd_cancel_block;
    d.dvd_cancel_callback = 0;
    return BeginDeviceCallback(callback, 0, block);
  }
  if (d.dvd.Empty())
    return false;
  DvdQueue::Completion completion;
  if (!d.dvd.Advance(disc_io_worker_, completion, d.dvd_bytes)) {
    if (const auto block = d.dvd.ActiveBlock())
      if (auto *p = memory_.Resolve(block, 48))
        P32(p + 12, 1);
    return false;
  }
  const auto &request = completion.request;
  int result = completion.result;
  if (result >= 0) {
    auto *destination = memory_.Resolve(request.destination, request.bytes);
    if (!destination || d.dvd_bytes.size() != request.bytes)
      result = -1;
    else {
      std::memcpy(destination, d.dvd_bytes.data(), request.bytes);
      native_gx_.NotifyMemoryWrite(request.destination, request.bytes);
    }
  }
  if (auto *block = memory_.Resolve(request.block, 48)) {
    P32(block + 12, result == -6 ? 10 : result < 0 ? 0xffffffffu : 0);
    P32(block + 28, 0);
    P32(block + 32, result >= 0 ? request.bytes : 0);
  }
  if(TraceDvd())std::fprintf(stderr,"GEKKOAOT_NATIVE_DVD_COMPLETE block=%08x dest=%08x offset=%llx bytes=%u result=%d callback=%08x pc=%08x\n",request.block,request.destination,(unsigned long long)request.offset,request.bytes,result,request.callback,cpu_.pc);
  d.dvd_bytes.clear();
  d.dvd_cancel_callback = request.cancel_callback;
  d.dvd_cancel_block = request.block;
  if (request.callback)
    return BeginDeviceCallback(request.callback, result, request.block);
  return false;
}
bool HostRuntime::DispatchDvdSdk(SimpleHleKind kind,std::uint32_t address) {
  return DispatchDvdSdkNative<-1>(kind,address);
}
template<int NativeKind>
bool HostRuntime::DispatchDvdSdkNative(SimpleHleKind dynamic_kind,[[maybe_unused]] std::uint32_t address) {
  const auto kind=NativeKind<0?dynamic_kind:static_cast<SimpleHleKind>(NativeKind);
  auto &d = devices_;
  const auto a = cpu_.gpr[3], b = cpu_.gpr[4], c = cpu_.gpr[5], e = cpu_.gpr[6],
             f = cpu_.gpr[7], g = cpu_.gpr[8];
  const auto done = [&](int value) {
    cpu_.gpr[3] = value;
    cpu_.pc = cpu_.lr;
    return true;
  };
  if (kind == SimpleHleKind::DvdOpen) {
    auto *out = memory_.Resolve(b, 60);
    const auto available =
        std::min<std::size_t>(memory_.ContiguousAvailable(a), 4096);
    const auto *path = memory_.Resolve(a, available);
    if (!out || !path || !available)
      return false;
    // The retail path resolver owns the current directory. Until that ABI
    // is proven, let relative paths use its compiled software-only traversal;
    // treating every relative path as root would silently open the wrong file.
    if (path[0] != '/') return false;
    const auto *end =
        static_cast<const std::uint8_t *>(std::memchr(path, 0, available));
    if (!end)
      return false;
    const auto entry = native_vfs_.FindPath(
        std::string_view(reinterpret_cast<const char *>(path), end - path));
    if (!entry)
      return done(0);
    P32(out + 12, 0);
    P32(out + 0x30, entry->disc_offset);
    P32(out + 0x34, entry->size);
    P32(out + 0x38, 0);
    return done(1);
  }
  auto *block =
      memory_.Resolve(a, (kind == SimpleHleKind::DvdReadAbsAsync ||
                          kind == SimpleHleKind::DvdGetStatus ||
                          kind == SimpleHleKind::DvdCancelAsync) ? 48 : 60);
  if (!block)
    return false;
  if (kind == SimpleHleKind::DvdGetStatus)
    return done(U32(block + 12));
  if (kind == SimpleHleKind::DvdCancelAsync) {
    if (b && (!d.disable_scheduler || !d.enable_scheduler))
      return false;
    // Cancellation of an idle command still has an asynchronous cancel
    // callback in the SDK. Keep one deferred host notification for that case.
    if (d.dvd.Cancel(a, b))
      return done(1);
    if (d.dvd.Status(a) != 0)
      return done(0);
    if (b) {
      if (d.dvd_cancel_callback)
        return done(0);
      d.dvd_cancel_callback = b;
      d.dvd_cancel_block = a;
    }
    return done(1);
  }
  const bool absolute = kind == SimpleHleKind::DvdReadAbsAsync;
  if (!absolute && kind != SimpleHleKind::DvdReadAsync) return false;
  const auto callback = f;
  const auto priority = g;
  if (callback && (!d.disable_scheduler || !d.enable_scheduler))
    return false;
  if (priority > 3 || (b & 31u) || (c & 31u) || (e & 3u) ||
      std::int32_t(c) < 0 || std::int32_t(e) < 0)
    return false;
  if (!memory_.Resolve(b, c))
    return false;
  std::uint64_t offset = e;
  if (!absolute) {
    const auto length = U32(block + 0x34);
    if (e > length || std::uint64_t(c) > std::uint64_t(length) + 31 - e)
      return false;
    offset += U32(block + 0x30);
  }
  if (offset > disc_image_size_ || c > disc_image_size_ - offset)
    return false;
  if(TraceDvd())std::fprintf(stderr,"GEKKOAOT_NATIVE_DVD_READ absolute=%u block=%08x dest=%08x offset=%llx bytes=%u callback=%08x lr=%08x\n",unsigned(absolute),a,b,(unsigned long long)offset,c,callback,cpu_.lr);
  // DVDReadPrio executes its compiled wait adapter; only its async request
  // reaches this queue. There is no blocking host read on the AOT CPU thread.
  DvdQueue::Request request;
  request.block = a;
  request.destination = b;
  request.offset = offset;
  request.bytes = c;
  request.callback = callback;
  request.priority = priority;
  if (!d.dvd.Submit(request))
    return done(0);
  // Begin the media read now; completion is still delivered in order
  // by ServiceDvdSdkEvents, exactly as before.
  (void)d.dvd.StartNext(disc_io_worker_);
  P32(block + 8, 1);
  P32(block + 12, 2);
  P32(block + 16, offset);
  P32(block + 20, c);
  P32(block + 24, b);
  P32(block + 28, 0);
  P32(block + 32, 0);
  P32(block + 40, callback);
  if (!absolute)
    P32(block + 56, callback);
  return done(1);
}
// Generate all specializations from the shared registry. Dead cases are
// removed inside each specialization, without relying on cross-DSO inlining.
#define SDK_SIMPLE(kind, name, abi) template bool HostRuntime::DispatchDvdSdkNative<static_cast<int>(SimpleHleKind::kind)>(SimpleHleKind,std::uint32_t);
#define SDK_OS(kind, name, abi)
#include "sdk/native_services.def"
#undef SDK_SIMPLE
#undef SDK_OS
} // namespace GekkoAOT::Native
