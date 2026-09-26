#pragma once
#include "input.h"
#include <SDL3/SDL.h>
#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <mutex>
#include <cstdio>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#if defined(HAVE_X11)
#include <X11/Xlib.h>
#include <X11/keysym.h>
// GEKKOAOT_SDL_X11_MACRO_GUARD_V22_3
// GEKKOAOT_SDL_X11_MACRO_GUARD_V22_4
// Xlib exposes legacy preprocessor macros that collide with ordinary C++
// identifiers used by both this header and Dolphin (Bool, Status, None,
// Success).  Strip them immediately after the X11 declarations so they never
// leak into the SI translation unit.
#ifdef Bool
#undef Bool
#endif
#ifdef Status
#undef Status
#endif
#ifdef None
#undef None
#endif
#ifdef Success
#undef Success
#endif
#endif

namespace GekkoAOT::Input {

inline bool NativeSelected() {
  static const bool selected = [] {
    const char* value = std::getenv("GEKKOAOT_PAD_BACKEND");
    return value && std::string_view(value) == "sdl3";
  }();
  return selected;
}

namespace detail {
inline std::string Trim(std::string value) {
  while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) value.erase(value.begin());
  while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) value.pop_back();
  return value;
}
inline std::string Lower(std::string value) {
  std::ranges::transform(value, value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return value;
}
inline bool ContainsNoCase(std::string_view haystack, std::string_view needle) {
  if (needle.empty()) return true;
  std::string h(haystack), n(needle);
  return Lower(h).find(Lower(n)) != std::string::npos;
}
inline bool ParseBool(std::string value, bool fallback = false) {
  value = Lower(Trim(std::move(value)));
  if (value.empty()) return fallback;
  return value == "1" || value == "true" || value == "yes" || value == "on";
}
inline int Integer(const std::string& value, int fallback) {
  const std::string trimmed = Trim(value);
  if (trimmed.empty()) return fallback;
  int parsed = 0;
  const char* const begin = trimmed.data();
  const char* const end = begin + trimmed.size();
  const auto [ptr, ec] = std::from_chars(begin, end, parsed, 10);
  return ec == std::errc{} && ptr == end ? parsed : fallback;
}

inline SDL_GamepadButton GamepadButton(std::string value) {
  value = Lower(Trim(std::move(value)));
  static const std::unordered_map<std::string, SDL_GamepadButton> map = {
      {"south", SDL_GAMEPAD_BUTTON_SOUTH}, {"east", SDL_GAMEPAD_BUTTON_EAST},
      {"west", SDL_GAMEPAD_BUTTON_WEST}, {"north", SDL_GAMEPAD_BUTTON_NORTH},
      {"start", SDL_GAMEPAD_BUTTON_START}, {"back", SDL_GAMEPAD_BUTTON_BACK},
      {"guide", SDL_GAMEPAD_BUTTON_GUIDE}, {"left_stick", SDL_GAMEPAD_BUTTON_LEFT_STICK},
      {"right_stick", SDL_GAMEPAD_BUTTON_RIGHT_STICK},
      {"left_shoulder", SDL_GAMEPAD_BUTTON_LEFT_SHOULDER},
      {"right_shoulder", SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER},
      {"dpad_up", SDL_GAMEPAD_BUTTON_DPAD_UP}, {"dpad_down", SDL_GAMEPAD_BUTTON_DPAD_DOWN},
      {"dpad_left", SDL_GAMEPAD_BUTTON_DPAD_LEFT}, {"dpad_right", SDL_GAMEPAD_BUTTON_DPAD_RIGHT},
  };
  const auto it = map.find(value);
  return it == map.end() ? SDL_GAMEPAD_BUTTON_INVALID : it->second;
}

inline SDL_GamepadAxis GamepadAxis(std::string value) {
  value = Lower(Trim(std::move(value)));
  static const std::unordered_map<std::string, SDL_GamepadAxis> map = {
      {"leftx", SDL_GAMEPAD_AXIS_LEFTX}, {"lefty", SDL_GAMEPAD_AXIS_LEFTY},
      {"rightx", SDL_GAMEPAD_AXIS_RIGHTX}, {"righty", SDL_GAMEPAD_AXIS_RIGHTY},
      {"left_trigger", SDL_GAMEPAD_AXIS_LEFT_TRIGGER},
      {"right_trigger", SDL_GAMEPAD_AXIS_RIGHT_TRIGGER},
  };
  const auto it = map.find(value);
  return it == map.end() ? SDL_GAMEPAD_AXIS_INVALID : it->second;
}

inline SDL_Scancode Key(std::string value) {
  value = Trim(std::move(value));
  if (value.empty() || Lower(value) == "none") return SDL_SCANCODE_UNKNOWN;
  return SDL_GetScancodeFromName(value.c_str());
}
}  // namespace detail

enum class Source { Auto, Keyboard, Gamepad, Joystick, Disabled };

struct DigitalBinding {
  SDL_Scancode key = SDL_SCANCODE_UNKNOWN;
  SDL_GamepadButton button = SDL_GAMEPAD_BUTTON_INVALID;
  int joystick_button = -1;
};

struct AxisBinding {
  SDL_Scancode negative = SDL_SCANCODE_UNKNOWN;
  SDL_Scancode positive = SDL_SCANCODE_UNKNOWN;
  SDL_GamepadAxis axis = SDL_GAMEPAD_AXIS_INVALID;
  int joystick_axis = -1;
  bool invert = false;
  int deadzone = 4096;
};

struct TriggerBinding {
  SDL_Scancode key = SDL_SCANCODE_UNKNOWN;
  SDL_GamepadAxis axis = SDL_GAMEPAD_AXIS_INVALID;
  int joystick_axis = -1;
};

struct PortConfig {
  Source source = Source::Disabled;
  std::string device_match;
  DigitalBinding a, b, x, y, start, z, l, r, dpad_up, dpad_down, dpad_left, dpad_right;
  AxisBinding main_x, main_y, c_x, c_y;
  TriggerBinding l_analog, r_analog;
};

inline DigitalBinding Digital(const char* key, SDL_GamepadButton button, int joy_button) {
  return {detail::Key(key), button, joy_button};
}
inline AxisBinding AxisMap(const char* neg, const char* pos, SDL_GamepadAxis axis, int joy_axis,
                           bool invert = false) {
  AxisBinding out;
  out.negative = detail::Key(neg);
  out.positive = detail::Key(pos);
  out.axis = axis;
  out.joystick_axis = joy_axis;
  out.invert = invert;
  return out;
}

inline PortConfig DefaultPort(unsigned port) {
  PortConfig out;
  out.source = port == 0 ? Source::Auto : Source::Disabled;
  out.a = Digital("J", SDL_GAMEPAD_BUTTON_SOUTH, 0);
  out.b = Digital("K", SDL_GAMEPAD_BUTTON_EAST, 1);
  out.x = Digital("U", SDL_GAMEPAD_BUTTON_WEST, 2);
  out.y = Digital("I", SDL_GAMEPAD_BUTTON_NORTH, 3);
  out.start = Digital("Return", SDL_GAMEPAD_BUTTON_START, 9);
  out.z = Digital("R", SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, 5);
  out.l = Digital("Q", SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, 4);
  out.r = Digital("E", SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, 5);
  out.dpad_up = Digital("T", SDL_GAMEPAD_BUTTON_DPAD_UP, 12);
  out.dpad_down = Digital("G", SDL_GAMEPAD_BUTTON_DPAD_DOWN, 13);
  out.dpad_left = Digital("F", SDL_GAMEPAD_BUTTON_DPAD_LEFT, 14);
  out.dpad_right = Digital("H", SDL_GAMEPAD_BUTTON_DPAD_RIGHT, 15);
  out.main_x = AxisMap("A", "D", SDL_GAMEPAD_AXIS_LEFTX, 0);
  out.main_y = AxisMap("S", "W", SDL_GAMEPAD_AXIS_LEFTY, 1, true);
  out.c_x = AxisMap("Left", "Right", SDL_GAMEPAD_AXIS_RIGHTX, 2);
  out.c_y = AxisMap("Down", "Up", SDL_GAMEPAD_AXIS_RIGHTY, 3, true);
  out.l_analog = {detail::Key("Q"), SDL_GAMEPAD_AXIS_LEFT_TRIGGER, 4};
  out.r_analog = {detail::Key("E"), SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, 5};
  return out;
}

class SDLInput {
  bool initialized_ = false;
  std::mutex mutex_;
  SDL_ThreadID owner_ = 0;
  SDL_Window* input_window_ = nullptr;
  bool video_initialized_ = false;
  std::array<Pad, 4> snapshot_{};
  std::array<int, 4> pending_rumble_{{-1, -1, -1, -1}};
  std::array<PortConfig, 4> config_{};
  std::array<SDL_Gamepad*, 4> gamepads_{};
  std::array<SDL_Joystick*, 4> joysticks_{};
  std::filesystem::path config_path_;
  Uint64 last_device_scan_ms_ = 0;
#if defined(HAVE_X11)
  Display* x11_display_ = nullptr;
  std::array<KeyCode, SDL_SCANCODE_COUNT> x11_keycodes_{};
#endif

  static Source ParseSource(std::string value) {
    value = detail::Lower(detail::Trim(std::move(value)));
    if (value == "keyboard") return Source::Keyboard;
    if (value == "gamepad") return Source::Gamepad;
    if (value == "joystick") return Source::Joystick;
    if (value == "disabled" || value == "none") return Source::Disabled;
    return Source::Auto;
  }

  static std::string KeyName(const std::unordered_map<std::string, std::string>& section,
                             const std::string& key, const std::string& fallback = {}) {
    const auto it = section.find(key);
    return it == section.end() ? fallback : it->second;
  }

  static void LoadDigital(const std::unordered_map<std::string, std::string>& s,
                          const char* name, DigitalBinding* out) {
    out->key = detail::Key(KeyName(s, std::string(name) + "_key"));
    const auto button = KeyName(s, std::string(name) + "_button");
    if (!button.empty()) out->button = detail::GamepadButton(button);
    const auto joy = KeyName(s, std::string(name) + "_joy_button");
    if (!joy.empty()) out->joystick_button = detail::Integer(joy, -1);
  }

  static void LoadAxis(const std::unordered_map<std::string, std::string>& s,
                       const char* name, AxisBinding* out) {
    out->negative = detail::Key(KeyName(s, std::string(name) + "_negative_key"));
    out->positive = detail::Key(KeyName(s, std::string(name) + "_positive_key"));
    const auto axis = KeyName(s, std::string(name) + "_axis");
    if (!axis.empty()) out->axis = detail::GamepadAxis(axis);
    const auto joy = KeyName(s, std::string(name) + "_joy_axis");
    if (!joy.empty()) out->joystick_axis = detail::Integer(joy, -1);
    const auto invert = KeyName(s, std::string(name) + "_invert");
    if (!invert.empty()) out->invert = detail::ParseBool(invert);
    const auto deadzone = KeyName(s, std::string(name) + "_deadzone");
    if (!deadzone.empty()) out->deadzone = std::clamp(detail::Integer(deadzone, 4096), 0, 30000);
  }

  static void LoadTrigger(const std::unordered_map<std::string, std::string>& s,
                          const char* name, TriggerBinding* out) {
    out->key = detail::Key(KeyName(s, std::string(name) + "_key"));
    const auto axis = KeyName(s, std::string(name) + "_axis");
    if (!axis.empty()) out->axis = detail::GamepadAxis(axis);
    const auto joy = KeyName(s, std::string(name) + "_joy_axis");
    if (!joy.empty()) out->joystick_axis = detail::Integer(joy, -1);
  }

  void LoadConfig() {
    for (unsigned i = 0; i < 4; ++i) config_[i] = DefaultPort(i);
    const char* configured = std::getenv("GEKKOAOT_INPUT_CONFIG");
    if (!configured || !*configured) return;
    config_path_ = configured;
    std::ifstream file(config_path_);
    if (!file) return;
    std::array<std::unordered_map<std::string, std::string>, 4> sections;
    int current = -1;
    std::string line;
    while (std::getline(file, line)) {
      line = detail::Trim(line);
      if (line.empty() || line[0] == '#' || line[0] == ';') continue;
      if (line.front() == '[' && line.back() == ']') {
        const auto section = detail::Lower(line.substr(1, line.size() - 2));
        current = -1;
        for (int i = 0; i < 4; ++i)
          if (section == "port" + std::to_string(i + 1)) current = i;
        continue;
      }
      if (current < 0) continue;
      const auto eq = line.find('=');
      if (eq == std::string::npos) continue;
      sections[current][detail::Lower(detail::Trim(line.substr(0, eq)))] = detail::Trim(line.substr(eq + 1));
    }
    for (unsigned i = 0; i < 4; ++i) {
      auto& cfg = config_[i];
      const auto& s = sections[i];
      if (const auto it = s.find("source"); it != s.end()) cfg.source = ParseSource(it->second);
      if (const auto it = s.find("device"); it != s.end()) cfg.device_match = it->second;
      LoadDigital(s, "a", &cfg.a); LoadDigital(s, "b", &cfg.b);
      LoadDigital(s, "x", &cfg.x); LoadDigital(s, "y", &cfg.y);
      LoadDigital(s, "start", &cfg.start); LoadDigital(s, "z", &cfg.z);
      LoadDigital(s, "l", &cfg.l); LoadDigital(s, "r", &cfg.r);
      LoadDigital(s, "dpad_up", &cfg.dpad_up); LoadDigital(s, "dpad_down", &cfg.dpad_down);
      LoadDigital(s, "dpad_left", &cfg.dpad_left); LoadDigital(s, "dpad_right", &cfg.dpad_right);
      LoadAxis(s, "main_x", &cfg.main_x); LoadAxis(s, "main_y", &cfg.main_y);
      LoadAxis(s, "c_x", &cfg.c_x); LoadAxis(s, "c_y", &cfg.c_y);
      LoadTrigger(s, "l_analog", &cfg.l_analog); LoadTrigger(s, "r_analog", &cfg.r_analog);
    }
  }

#if defined(HAVE_X11)
  static KeySym X11KeySym(SDL_Scancode scancode) {
    if (scancode >= SDL_SCANCODE_A && scancode <= SDL_SCANCODE_Z)
      return XK_a + (scancode - SDL_SCANCODE_A);
    if (scancode >= SDL_SCANCODE_1 && scancode <= SDL_SCANCODE_9)
      return XK_1 + (scancode - SDL_SCANCODE_1);
    if (scancode == SDL_SCANCODE_0) return XK_0;
    if (scancode >= SDL_SCANCODE_F1 && scancode <= SDL_SCANCODE_F12)
      return XK_F1 + (scancode - SDL_SCANCODE_F1);
    switch (scancode) {
    case SDL_SCANCODE_RETURN: return XK_Return;
    case SDL_SCANCODE_ESCAPE: return XK_Escape;
    case SDL_SCANCODE_BACKSPACE: return XK_BackSpace;
    case SDL_SCANCODE_TAB: return XK_Tab;
    case SDL_SCANCODE_SPACE: return XK_space;
    case SDL_SCANCODE_LEFT: return XK_Left;
    case SDL_SCANCODE_RIGHT: return XK_Right;
    case SDL_SCANCODE_UP: return XK_Up;
    case SDL_SCANCODE_DOWN: return XK_Down;
    case SDL_SCANCODE_INSERT: return XK_Insert;
    case SDL_SCANCODE_DELETE: return XK_Delete;
    case SDL_SCANCODE_HOME: return XK_Home;
    case SDL_SCANCODE_END: return XK_End;
    case SDL_SCANCODE_PAGEUP: return XK_Page_Up;
    case SDL_SCANCODE_PAGEDOWN: return XK_Page_Down;
    case SDL_SCANCODE_LSHIFT: return XK_Shift_L;
    case SDL_SCANCODE_RSHIFT: return XK_Shift_R;
    case SDL_SCANCODE_LCTRL: return XK_Control_L;
    case SDL_SCANCODE_RCTRL: return XK_Control_R;
    case SDL_SCANCODE_LALT: return XK_Alt_L;
    case SDL_SCANCODE_RALT: return XK_Alt_R;
    case SDL_SCANCODE_LGUI: return XK_Super_L;
    case SDL_SCANCODE_RGUI: return XK_Super_R;
    default: break;
    }
    const char* name = SDL_GetScancodeName(scancode);
    return name && *name ? XStringToKeysym(name) : NoSymbol;
  }

  void InitializeX11Keyboard() {
    if (x11_display_ || !std::getenv("DISPLAY")) return;
    x11_display_ = XOpenDisplay(nullptr);
    if (!x11_display_) return;
    for (int i = 0; i < SDL_SCANCODE_COUNT; ++i) {
      const KeySym sym = X11KeySym(static_cast<SDL_Scancode>(i));
      if (sym != NoSymbol)
        x11_keycodes_[static_cast<std::size_t>(i)] = XKeysymToKeycode(x11_display_, sym);
    }
    std::fprintf(stderr,
                 "GEKKOAOT_SDL_KEYBOARD_V22=1 backend=x11-query-keymap profile=input.ini focus-independent=1\n");
  }
#endif

  void ClosePort(unsigned port) {
    if (gamepads_[port]) { SDL_CloseGamepad(gamepads_[port]); gamepads_[port] = nullptr; }
    if (joysticks_[port]) { SDL_CloseJoystick(joysticks_[port]); joysticks_[port] = nullptr; }
  }

  void RefreshDevices(bool force = false) {
    // GEKKOAOT_SDL_AUTODETECT_V22: connection state is cheap to check every
    // poll; the global SDL device enumeration is throttled unless a port needs
    // a device. This also makes hot-plug work independently of PADRead HLE.
    bool needs_device = false;
    std::unordered_set<SDL_JoystickID> used;
    for (unsigned p = 0; p < 4; ++p) {
      if (gamepads_[p] && SDL_GamepadConnected(gamepads_[p])) used.insert(SDL_GetGamepadID(gamepads_[p]));
      else if (gamepads_[p]) { std::fprintf(stderr, "GEKKOAOT_SDL_DISCONNECT port=%u\n", p+1); SDL_CloseGamepad(gamepads_[p]); gamepads_[p] = nullptr; }
      if (joysticks_[p] && SDL_JoystickConnected(joysticks_[p])) used.insert(SDL_GetJoystickID(joysticks_[p]));
      else if (joysticks_[p]) { std::fprintf(stderr, "GEKKOAOT_SDL_DISCONNECT port=%u\n", p+1); SDL_CloseJoystick(joysticks_[p]); joysticks_[p] = nullptr; }
      const auto source = config_[p].source;
      if (source != Source::Disabled && source != Source::Keyboard &&
          !gamepads_[p] && !joysticks_[p])
        needs_device = true;
    }

    const Uint64 now = SDL_GetTicks();
    if (!force && (!needs_device || (last_device_scan_ms_ && now - last_device_scan_ms_ < 250)))
      return;
    last_device_scan_ms_ = now;

    int count = 0;
    SDL_JoystickID* ids = SDL_GetJoysticks(&count);
    if (!ids) return;
    for (unsigned port = 0; port < 4; ++port) {
      const auto source = config_[port].source;
      if (source == Source::Disabled || source == Source::Keyboard) continue;
      if (gamepads_[port] || joysticks_[port]) continue;
      // Prefer SDL's standardized Gamepad API (Xbox, DualShock/DualSense,
      // Switch Pro, etc.). A saved device name is a preference, not a hard
      // requirement: USB/Bluetooth and SDL mapping layers can expose different
      // names for the same physical pad. Try the requested name first, then the
      // first compatible device of the requested class.
      for (int pass = 0; pass < 4 && !gamepads_[port] && !joysticks_[port]; ++pass) {
        const bool want_gamepad = pass < 2;
        const bool require_match = (pass % 2) == 0 && !config_[port].device_match.empty();
        if (source == Source::Gamepad && !want_gamepad) continue;
        if (source == Source::Joystick && want_gamepad) continue;
        for (int i = 0; i < count; ++i) {
          const SDL_JoystickID id = ids[i];
          if (used.contains(id)) continue;
          const bool is_gamepad = SDL_IsGamepad(id);
          if (is_gamepad != want_gamepad) continue;
          const char* name = is_gamepad ? SDL_GetGamepadNameForID(id) : SDL_GetJoystickNameForID(id);
          if (require_match && !detail::ContainsNoCase(name ? name : "", config_[port].device_match))
            continue;
          if (is_gamepad) gamepads_[port] = SDL_OpenGamepad(id);
          else joysticks_[port] = SDL_OpenJoystick(id);
          if (gamepads_[port] || joysticks_[port]) {
            std::fprintf(stderr,
                         "GEKKOAOT_SDL_DEVICE port=%u id=%u type=%s name=%s mapping=%s\n",
                         port+1, unsigned(id), is_gamepad ? "gamepad" : "joystick",
                         name ? name : "unknown", require_match ? "profile" : "auto-fallback");
            used.insert(id);
            break;
          }
        }
      }
    }
    SDL_free(ids);
  }

  static bool KeyDown(const std::uint8_t* keys, SDL_Scancode key) {
    return keys && key != SDL_SCANCODE_UNKNOWN && keys[key];
  }
  static bool ButtonDown(SDL_Gamepad* pad, SDL_GamepadButton button) {
    return pad && button != SDL_GAMEPAD_BUTTON_INVALID && SDL_GetGamepadButton(pad, button);
  }
  static bool JoyButtonDown(SDL_Joystick* joy, int button) {
    return joy && button >= 0 && button < SDL_GetNumJoystickButtons(joy) && SDL_GetJoystickButton(joy, button);
  }

  static bool DigitalDown(const DigitalBinding& binding, const std::uint8_t* keys, SDL_Gamepad* pad,
                          SDL_Joystick* joy, bool keyboard, bool device) {
    return (keyboard && KeyDown(keys, binding.key)) ||
           (device && ButtonDown(pad, binding.button)) ||
           (device && JoyButtonDown(joy, binding.joystick_button));
  }

  static std::int8_t MappedAxis(const AxisBinding& binding, const std::uint8_t* keys, SDL_Gamepad* pad,
                                SDL_Joystick* joy, bool keyboard, bool device) {
    int keyboard_value = 0;
    if (keyboard) {
      if (KeyDown(keys, binding.negative)) keyboard_value -= 80;
      if (KeyDown(keys, binding.positive)) keyboard_value += 80;
    }
    int raw = 0;
    if (device && pad && binding.axis != SDL_GAMEPAD_AXIS_INVALID)
      raw = SDL_GetGamepadAxis(pad, binding.axis);
    else if (device && joy && binding.joystick_axis >= 0 && binding.joystick_axis < SDL_GetNumJoystickAxes(joy))
      raw = SDL_GetJoystickAxis(joy, binding.joystick_axis);
    if (binding.invert) raw = -raw;
    int device_value = 0;
    if (std::abs(raw) > binding.deadzone) {
      const int sign = raw < 0 ? -1 : 1;
      const int magnitude = std::min(32767, std::abs(raw));
      device_value = sign * (magnitude - binding.deadzone) * 80 / std::max(1, 32767 - binding.deadzone);
    }
    const int chosen = std::abs(device_value) > std::abs(keyboard_value) ? device_value : keyboard_value;
    return static_cast<std::int8_t>(std::clamp(chosen, -80, 80));
  }

  static std::uint8_t MappedTrigger(const TriggerBinding& binding, const std::uint8_t* keys, SDL_Gamepad* pad,
                                    SDL_Joystick* joy, bool keyboard, bool device) {
    int value = keyboard && KeyDown(keys, binding.key) ? 255 : 0;
    int raw = 0;
    if (device && pad && binding.axis != SDL_GAMEPAD_AXIS_INVALID)
      raw = SDL_GetGamepadAxis(pad, binding.axis);
    else if (device && joy && binding.joystick_axis >= 0 && binding.joystick_axis < SDL_GetNumJoystickAxes(joy))
      raw = SDL_GetJoystickAxis(joy, binding.joystick_axis);
    if (raw > 0) value = std::max(value, raw * 255 / 32767);
    return static_cast<std::uint8_t>(std::clamp(value, 0, 255));
  }

public:
  bool Initialize() {
    std::lock_guard lock(mutex_);
    if (initialized_) return true;
    owner_ = SDL_GetCurrentThreadID();
    // Prefer SDL's standardized HID/gamepad paths. These hints are harmless
    // when a backend is unavailable and improve Xbox + DualShock/DualSense
    // discovery across USB/Bluetooth on Linux and the other desktop targets.
    SDL_SetHint("SDL_JOYSTICK_HIDAPI", "1");
    SDL_SetHint("SDL_JOYSTICK_HIDAPI_XBOX", "1");
    SDL_SetHint("SDL_JOYSTICK_HIDAPI_PS4", "1");
    SDL_SetHint("SDL_JOYSTICK_HIDAPI_PS5", "1");
    SDL_SetHint("SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS", "1");
    initialized_ = SDL_InitSubSystem(SDL_INIT_GAMEPAD | SDL_INIT_HAPTIC | SDL_INIT_EVENTS);
    if (!initialized_) return false;
#if defined(HAVE_X11)
    InitializeX11Keyboard();
#endif
    LoadConfig();
    RefreshDevices(true);
    std::fprintf(stderr, "GEKKOAOT_SDL_AUTODETECT_V22=1 policy=gamepad-first saved-name=preferred hidapi=on hotplug-ms=250 si-bridge=1\n");
    return true;
  }

  void ReloadConfig() {
    std::lock_guard lock(mutex_);
    if (SDL_GetCurrentThreadID() != owner_) return;
    for (unsigned i = 0; i < 4; ++i) ClosePort(i);
    LoadConfig();
    RefreshDevices(true);
  }

  void Shutdown() {
    std::lock_guard lock(mutex_);
    for (unsigned i = 0; i < 4; ++i) ClosePort(i);
    // Destroy any optional SDL window before tearing down the video/events
    // subsystems. The normal v11 path never creates one.
    if (input_window_) { SDL_DestroyWindow(input_window_); input_window_ = nullptr; }
    if (video_initialized_) { SDL_QuitSubSystem(SDL_INIT_VIDEO); video_initialized_ = false; }
    if (initialized_) SDL_QuitSubSystem(SDL_INIT_GAMEPAD | SDL_INIT_HAPTIC | SDL_INIT_EVENTS);
#if defined(HAVE_X11)
    if (x11_display_) { XCloseDisplay(x11_display_); x11_display_ = nullptr; }
    x11_keycodes_.fill(0);
#endif
    snapshot_ = {}; pending_rumble_.fill(-1);
    initialized_ = false;
  }

  // Attach SDL input to the host's existing window. SDL does not own the foreign
  // native window; rendering and presentation remain with the selected backend.
  bool AttachWindow(const char* driver, const char* property, void* window, bool numeric) {
    std::lock_guard lock(mutex_);
    if (!initialized_ || SDL_GetCurrentThreadID() != owner_ || !window) return false;
    if (input_window_) return true;

    // GEKKOAOT_SDL_FOREIGN_WINDOW_V11: wrapping a window owned by Dolphin is
    // unsafe on X11/XWayland. SDL may call XChangeWindowAttributes from its own
    // Display connection, which can raise BadAccess and leave the process heap
    // in a corrupted state. Native PAD/gamepad input does not require a foreign
    // SDL_Window, so keep this experimental path disabled by default.
    const char* allow_foreign = std::getenv("GEKKOAOT_SDL_ATTACH_FOREIGN_WINDOW");
    const bool allow = allow_foreign &&
                       (std::string_view(allow_foreign) == "1" ||
                        std::string_view(allow_foreign) == "true");
    if (!allow) {
      static bool reported = false;
      if (!reported) {
        std::fprintf(stderr,
                     "GEKKOAOT_SDL_FOREIGN_WINDOW_V11=disabled driver=%s "
                     "reason=foreign-window-wrap-unsafe\n",
                     driver ? driver : "unknown");
        reported = true;
      }
      return false;
    }

    if (driver) SDL_SetHint(SDL_HINT_VIDEO_DRIVER, driver);
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) return false;
    video_initialized_ = true;
    const auto props = SDL_CreateProperties();
    if (!props) {
      SDL_QuitSubSystem(SDL_INIT_VIDEO);
      video_initialized_ = false;
      return false;
    }
    if (numeric)
      SDL_SetNumberProperty(props, property,
                            static_cast<Sint64>(reinterpret_cast<uintptr_t>(window)));
    else
      SDL_SetPointerProperty(props, property, window);
    input_window_ = SDL_CreateWindowWithProperties(props);
    SDL_DestroyProperties(props);
    if (!input_window_) {
      std::fprintf(stderr, "GEKKOAOT_SDL_WINDOW error=%s\n", SDL_GetError());
      SDL_QuitSubSystem(SDL_INIT_VIDEO);
      video_initialized_ = false;
    }
    return input_window_ != nullptr;
  }

  void Pump() {
    std::lock_guard lock(mutex_);
    if (!initialized_ || SDL_GetCurrentThreadID() != owner_) return;
    UpdateUnlocked(snapshot_);
    for (unsigned i=0; i<4; ++i) if (pending_rumble_[i] >= 0) {
      RumbleUnlocked(i, unsigned(pending_rumble_[i])); pending_rumble_[i] = -1;
    }
  }

  bool Poll(std::array<Pad, 4>& output) {
    std::lock_guard lock(mutex_);
    if (!initialized_) { output = {}; return false; }
    if (SDL_GetCurrentThreadID() == owner_) UpdateUnlocked(snapshot_);
    output = snapshot_;
    return true;
  }

private:
  bool UpdateUnlocked(std::array<Pad, 4>& output) {
    if (!initialized_) return false;
    SDL_PumpEvents();
    SDL_UpdateGamepads();
    if (std::ranges::any_of(joysticks_, [](SDL_Joystick* joy) { return joy != nullptr; }))
      SDL_UpdateJoysticks();
    RefreshDevices();
    int key_count = 0;
    const bool* sdl_keys = SDL_GetKeyboardState(&key_count);
    std::array<std::uint8_t, SDL_SCANCODE_COUNT> merged_keys{};
    for (int i = 0; sdl_keys && i < key_count && i < SDL_SCANCODE_COUNT; ++i)
      merged_keys[static_cast<std::size_t>(i)] = sdl_keys[i] ? 1 : 0;
#if defined(HAVE_X11)
    if (x11_display_)
    {
      char x11_keys[32]{};
      XQueryKeymap(x11_display_, x11_keys);
      for (int i = 0; i < SDL_SCANCODE_COUNT; ++i)
      {
        const KeyCode code = x11_keycodes_[static_cast<std::size_t>(i)];
        if (code && (static_cast<unsigned char>(x11_keys[code >> 3]) & (1u << (code & 7))))
          merged_keys[static_cast<std::size_t>(i)] = 1;
      }
    }
#endif
    const std::uint8_t* keys = merged_keys.data();
    output = {};
    for (unsigned i = 0; i < 4; ++i) {
      const auto& cfg = config_[i];
      if (cfg.source == Source::Disabled) continue;
      const bool keyboard = cfg.source == Source::Auto || cfg.source == Source::Keyboard;
      const bool device = cfg.source == Source::Auto || cfg.source == Source::Gamepad || cfg.source == Source::Joystick;
      SDL_Gamepad* gp = gamepads_[i];
      SDL_Joystick* joy = joysticks_[i];
      auto& pad = output[i];
      pad.connected = keyboard || gp || joy;
      if (!pad.connected) continue;
      const auto set = [&](const DigitalBinding& b, std::uint16_t mask) {
        if (DigitalDown(b, keys, gp, joy, keyboard, device)) pad.buttons |= mask;
      };
      set(cfg.a, 0x0100); set(cfg.b, 0x0200); set(cfg.x, 0x0400); set(cfg.y, 0x0800);
      set(cfg.start, 0x1000); set(cfg.z, 0x0010); set(cfg.l, 0x0040); set(cfg.r, 0x0020);
      set(cfg.dpad_left, 0x0001); set(cfg.dpad_right, 0x0002);
      set(cfg.dpad_down, 0x0004); set(cfg.dpad_up, 0x0008);
      pad.stick_x = MappedAxis(cfg.main_x, keys, gp, joy, keyboard, device);
      pad.stick_y = MappedAxis(cfg.main_y, keys, gp, joy, keyboard, device);
      pad.sub_x = MappedAxis(cfg.c_x, keys, gp, joy, keyboard, device);
      pad.sub_y = MappedAxis(cfg.c_y, keys, gp, joy, keyboard, device);
      pad.left = MappedTrigger(cfg.l_analog, keys, gp, joy, keyboard, device);
      pad.right = MappedTrigger(cfg.r_analog, keys, gp, joy, keyboard, device);
      if (pad.left >= 230) pad.buttons |= 0x0040;
      if (pad.right >= 230) pad.buttons |= 0x0020;
      pad.analog_a = (pad.buttons & 0x0100) ? 255 : 0;
      pad.analog_b = (pad.buttons & 0x0200) ? 255 : 0;
      if (gp)
        pad.rumble = SDL_GetBooleanProperty(SDL_GetGamepadProperties(gp), SDL_PROP_GAMEPAD_CAP_RUMBLE_BOOLEAN, false);
      else if (joy)
        pad.rumble = SDL_GetBooleanProperty(SDL_GetJoystickProperties(joy), SDL_PROP_JOYSTICK_CAP_RUMBLE_BOOLEAN, false);
    }
    return true;
  }

  bool RumbleUnlocked(unsigned port, unsigned command) {
    if (!initialized_ || port >= 4 || command > 2) return false;
    const auto strength = static_cast<Uint16>(command == 1 ? 0xffff : 0);
    if (gamepads_[port])
      return SDL_RumbleGamepad(gamepads_[port], strength, strength, command == 1 ? 0xffffffffu : 0);
    if (joysticks_[port])
      return SDL_RumbleJoystick(joysticks_[port], strength, strength, command == 1 ? 0xffffffffu : 0);
    // Keyboard-only ports have no haptic device; treat stop/start as handled so
    // PADControlMotor does not fall back into the compatibility SI path.
    return config_[port].source == Source::Keyboard || config_[port].source == Source::Auto;
  }
public:
  bool Rumble(unsigned port, unsigned command) {
    std::lock_guard lock(mutex_);
    if (!initialized_ || port >= 4 || command > 2) return false;
    // PADControlMotor is void: a disconnected/non-haptic device is not a reason
    // to execute a second, guest SI operation after the native request.
    pending_rumble_[port] = static_cast<int>(command);
    if (SDL_GetCurrentThreadID() == owner_) {
      RumbleUnlocked(port, command); pending_rumble_[port] = -1;
    }
    return true;
  }
};

inline SDLInput& HostInput() { static SDLInput input; return input; }
}  // namespace GekkoAOT::Input
