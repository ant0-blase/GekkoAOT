// SPDX-License-Identifier: GPL-3.0-or-later
#include "../../runtime/gx/cached_scanout.h"
#include "../../runtime/NativeHW/VI/native_vi.h"
#include <cstdio>
#include <cstdlib>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "%d: %s\n", __LINE__, #x); std::exit(1); } } while (false)
int main() {
  using namespace GekkoAOT::GX;
  using GekkoAOT::NativeHW::VI::ShouldPresent;
  CHECK(ShouldPresent(true, 0, 0)); // CPU XFB before any GX copy.
  CHECK(ShouldPresent(true, 20, 20)); // RAM writes after a GX frame.
  CHECK(!ShouldPresent(true, 20, 20, true));
  CHECK(ShouldPresent(true, 21, 20, true));
  CHECK(ShouldPresent(false, 20, 20, true));
  CachedScanout state;
  CachedScanoutKey key{1, 2, 0x1000, 0x1000, 1920, 1080, 0};
  CHECK(!state.Duplicate(key)); state.Queued(key); CHECK(state.Duplicate(key));
  auto changed = key; changed.generation++; CHECK(!state.Duplicate(changed));
  changed = key; changed.texture++; CHECK(!state.Duplicate(changed));
  changed = key; changed.top += 1280; CHECK(!state.Duplicate(changed)); // Other field.
  changed = key; changed.bottom += 1280; CHECK(!state.Duplicate(changed));
  changed = key; changed.width = 1280; CHECK(!state.Duplicate(changed));
  changed = key; changed.height = 720; CHECK(!state.Duplicate(changed));
  changed = key; changed.aspect = 1; CHECK(!state.Duplicate(changed));
  state.Reset(); CHECK(!state.Duplicate(key)); // RAM scanout or surface resume.
  std::puts("Cached scanout and RAM VI observation passed");
}
