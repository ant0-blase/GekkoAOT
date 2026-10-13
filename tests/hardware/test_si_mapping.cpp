// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/input.h"
#include <cmath>
#include <cstdlib>
#include <iostream>

#define CHECK(x) do { if (!(x)) { std::cerr << __LINE__ << ": " #x "\n"; std::exit(1); } } while (false)
int main() {
  using GekkoAOT::Input::MapStick;
  const auto center = MapStick(0, 0);
  CHECK(center.x == 0 && center.y == 0);
  CHECK(MapStick(32767, 0).x == 127);
  CHECK(MapStick(-32768, 0).x == -127);
  CHECK(MapStick(0, 32767).y == 127);
  CHECK(MapStick(0, -32768).y == -127);
  CHECK(MapStick(2000, -2000).x == 0 && MapStick(2000, -2000).y == 0);
  CHECK(MapStick(5000, 0).x > 0);
  const auto diagonal = MapStick(32767, 32767);
  CHECK(diagonal.x > 85 && diagonal.x < 95 && diagonal.x == diagonal.y);
  CHECK(std::hypot(float(diagonal.x), float(diagonal.y)) <= 128.f);
  CHECK(128 + diagonal.x <= 255 && 128 + diagonal.y <= 255);
  std::cout << "SI radial mapping passed\n";
}
