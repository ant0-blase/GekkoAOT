// SPDX-License-Identifier: GPL-3.0-or-later
#include "../../runtime/gx/array_upload_policy.h"
#include <cstdio>
#include <cstdlib>
#include <limits>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "%d: %s\n", __LINE__, #x); std::exit(1); } } while (false)
int main() {
  using GekkoAOT::GX::ArrayUploadPolicy;
  ArrayUploadPolicy stable;
  CHECK(stable.UsePersistent(0)); stable.Uploaded(0);
  CHECK(stable.UsePersistent(100)); stable.Uploaded(100);
  CHECK(stable.UsePersistent(1000));
  ArrayUploadPolicy stream;
  CHECK(stream.UsePersistent(10)); stream.Uploaded(10);
  CHECK(!stream.UsePersistent(10)); // Multiple mutations inside one frame.
  for (unsigned f = 11; f < 74; ++f) CHECK(!stream.UsePersistent(f));
  CHECK(stream.UsePersistent(74)); stream.Uploaded(74);
  CHECK(!stream.UsePersistent(76)); // A two-frame streaming cadence.
  CHECK(stream.UsePersistent(140)); stream.Uploaded(140);
  CHECK(stream.UsePersistent(200)); // A formerly dynamic array can settle.
  ArrayUploadPolicy wrap;
  const auto end = std::numeric_limits<unsigned>::max();
  CHECK(wrap.UsePersistent(end - 1)); wrap.Uploaded(end - 1);
  CHECK(!wrap.UsePersistent(0));
  CHECK(!wrap.UsePersistent(63));
  CHECK(wrap.UsePersistent(64));
  ArrayUploadPolicy reset;
  CHECK(reset.UsePersistent(0)); // New game/context, independent of old state.
  std::puts("Array upload policy passed");
}
