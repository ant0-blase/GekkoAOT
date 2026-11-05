// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/sdl_input.h"
#include <chrono>
#include <cstdlib>
#include <iostream>
namespace GekkoAOT::Input {
struct SDLInputTestAccess {
  static auto Load(SDLInput& input) { input.LoadConfig(); return input.config_; }
};
}
#define CHECK(x) do { if (!(x)) { std::cerr << __LINE__ << ": " #x "\n"; std::exit(1); } } while (false)
int main() {
  using namespace GekkoAOT::Input;
  const auto file = std::filesystem::temp_directory_path() /
      ("gekkoaot-input-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".ini");
#ifdef _WIN32
  _putenv_s("GEKKOAOT_INPUT_CONFIG", file.string().c_str());
#else
  setenv("GEKKOAOT_INPUT_CONFIG", file.string().c_str(), 1);
#endif
  SDLInput input;
  // A source-only launcher profile must leave keyboard buttons, both sticks,
  // and analog shoulders usable. Overrides apply to the specified field only.
  { std::ofstream out(file); out << "[port1]\nsource=keyboard\na_key=Space\nmain_x_positive_key=Right\n[port2]\nsource=disabled\n"; }
  auto ports = SDLInputTestAccess::Load(input);
  CHECK(ports[0].source == Source::Keyboard);
  CHECK(ports[0].a.key == SDL_SCANCODE_SPACE);
  CHECK(ports[0].start.key == SDL_SCANCODE_RETURN);
  CHECK(ports[0].b.key == SDL_SCANCODE_K);
  CHECK(ports[0].main_x.negative == SDL_SCANCODE_A);
  CHECK(ports[0].main_x.positive == SDL_SCANCODE_RIGHT);
  CHECK(ports[0].c_x.negative == SDL_SCANCODE_LEFT);
  CHECK(ports[0].l_analog.key == SDL_SCANCODE_Q);
  CHECK(ports[0].r_analog.key == SDL_SCANCODE_E);
  CHECK(ports[1].source == Source::Disabled);
  // Explicitly unbound controls remain unbound, including an empty value.
  { std::ofstream out(file); out << "[port1]\nstart_key=none\na_key=\nmain_x_negative_key=none\nl_analog_key=none\na_button=\na_joy_button=\nmain_x_axis=\nmain_x_joy_axis=\nl_analog_axis=\nl_analog_joy_axis=\n"; }
  ports = SDLInputTestAccess::Load(input);
  CHECK(ports[0].start.key == SDL_SCANCODE_UNKNOWN);
  CHECK(ports[0].a.key == SDL_SCANCODE_UNKNOWN);
  CHECK(ports[0].a.button == SDL_GAMEPAD_BUTTON_INVALID);
  CHECK(ports[0].a.joystick_button == -1);
  CHECK(ports[0].main_x.axis == SDL_GAMEPAD_AXIS_INVALID);
  CHECK(ports[0].main_x.joystick_axis == -1);
  CHECK(ports[0].l_analog.axis == SDL_GAMEPAD_AXIS_INVALID);
  CHECK(ports[0].l_analog.joystick_axis == -1);
  CHECK(ports[0].main_x.negative == SDL_SCANCODE_UNKNOWN);
  CHECK(ports[0].l_analog.key == SDL_SCANCODE_UNKNOWN);
  CHECK(ports[0].b.key == SDL_SCANCODE_K);
  std::filesystem::remove(file);
  ports = SDLInputTestAccess::Load(input);
  CHECK(ports[0].start.key == SDL_SCANCODE_RETURN);
  CHECK(ports[0].a.key == SDL_SCANCODE_J);
  std::cout << "SDL partial profiles, explicit unbinding and reload defaults PASS\n";
}
