#pragma once
// SPDX-License-Identifier: GPL-3.0-or-later

namespace GekkoAOT::NativeHW::DI {
// The existing NativeVfs owns FST translation and the nod/raw reader owns the
// exact fallback. No second image parser or guest DMA implementation is used.
inline bool PreferVfs(bool native_hw_di, bool configured_vfs) {
  return native_hw_di || configured_vfs;
}

// Native reads complete when the host service has their bytes, including raw
// ranges outside the FST. Optical seek/rotation timing belongs only to the
// explicitly selected compatibility path.
constexpr bool HostCompletion(bool native_hw_di, bool vfs_ready,
                              bool configured_vfs, bool optical_timing) {
  return native_hw_di || (vfs_ready && configured_vfs && !optical_timing);
}
} // namespace GekkoAOT::NativeHW::DI
