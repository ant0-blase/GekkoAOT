// SPDX-License-Identifier: GPL-3.0-or-later
#include "../../runtime/gx/color_snapshot_layout.h"
#include <cstdio>
#include <cstdlib>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "%d: %s\n", __LINE__, #x); std::exit(1); } } while (false)
int main() {
  using GekkoAOT::GX::ColorSnapshotExtent;
  // Width 65 exercises padding on every row, not just the aligned 64/640
  // fixtures. Last-row padding must also be covered by the mapped byte range.
  const auto a = ColorSnapshotExtent(65,3);
  CHECK(a && a->row_bytes == 260 && a->row_pitch == 512 && a->byte_size == 1536);
  const auto b = ColorSnapshotExtent(640,480);
  CHECK(b && b->row_pitch == 2560 && b->byte_size == 1228800);
  CHECK(ColorSnapshotExtent(1,1)->byte_size == 256);
  CHECK(ColorSnapshotExtent(64,1)->row_pitch == 256);
  CHECK(!ColorSnapshotExtent(0,1));CHECK(!ColorSnapshotExtent(1,0));
  CHECK(!ColorSnapshotExtent(0xffffffff,1));CHECK(!ColorSnapshotExtent(1,0xffffffff));
  CHECK(!ColorSnapshotExtent(0x40000000,1));CHECK(!ColorSnapshotExtent(16384,65536));
  std::puts("EFB snapshot row padding, exact map extent, zero dimensions and overflow guards passed");
}
