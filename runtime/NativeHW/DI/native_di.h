#pragma once
// SPDX-License-Identifier: GPL-3.0-or-later

namespace GekkoAOT::NativeHW::DI {
// The existing NativeVfs owns FST translation and the nod/raw reader owns the
// exact fallback. No second image parser or guest DMA implementation is used.
inline bool PreferVfs(bool native_hw_di, bool configured_vfs) {
  return native_hw_di || configured_vfs;
}
} // namespace GekkoAOT::NativeHW::DI
