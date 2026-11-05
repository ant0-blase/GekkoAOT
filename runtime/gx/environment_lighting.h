// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#ifndef GEKKOAOT_ENVIRONMENT_LIGHTING_H
#define GEKKOAOT_ENVIRONMENT_LIGHTING_H
// NativeEnvironmentLighting: sun/sky parameters and the world frame used to
// relight captured scene receivers. GX supplies only model-view matrices, so
// the world frame is recovered from the scene itself and classified:
//
// * GameLight  - the game's own directional GX light (world-stable by
//                construction: the game moves it into view space each frame);
// * WorldFrame - the dominant model-view rotation of a pass (static world
//                geometry is drawn with an identity model matrix) plus a
//                vertical axis voted over many frames;
// * None       - neither is reliable: no sun is added, and the reason is kept.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <utility>

namespace GekkoAOT::GX {
using EnvVec3 = std::array<float, 3>;

inline float EnvDot(const EnvVec3& a, const EnvVec3& b) noexcept { return a[0]*b[0]+a[1]*b[1]+a[2]*b[2]; }
inline EnvVec3 EnvCross(const EnvVec3& a, const EnvVec3& b) noexcept {
  return {a[1]*b[2]-a[2]*b[1], a[2]*b[0]-a[0]*b[2], a[0]*b[1]-a[1]*b[0]};
}
inline EnvVec3 EnvNormalize(const EnvVec3& v) noexcept {
  const float length = std::sqrt(EnvDot(v, v));
  return length > 1e-20f ? EnvVec3{v[0]/length, v[1]/length, v[2]/length} : EnvVec3{0, 0, 0};
}
inline float EnvLuminance(const EnvVec3& c) noexcept { return .2126f*c[0]+.7152f*c[1]+.0722f*c[2]; }

// Black-body approximation (1000-40000 K), normalised to unit luminance so
// temperature changes tint the light without changing its intensity.
inline EnvVec3 ColorTemperature(float kelvin) noexcept {
  const double t = std::clamp(double(kelvin), 1000.0, 40000.0) / 100.0;
  double r, g, b;
  if (t <= 66) {
    r = 255;
    g = 99.4708025861 * std::log(t) - 161.1195681661;
    b = t <= 19 ? 0 : 138.5177312231 * std::log(t - 10) - 305.0447927307;
  } else {
    r = 329.698727446 * std::pow(t - 60, -0.1332047592);
    g = 288.1221695283 * std::pow(t - 60, -0.0755148492);
    b = 255;
  }
  EnvVec3 color{float(std::clamp(r, 0.0, 255.0) / 255), float(std::clamp(g, 0.0, 255.0) / 255),
                float(std::clamp(b, 0.0, 255.0) / 255)};
  const float luminance = std::max(EnvLuminance(color), 1e-6f);
  return {color[0]/luminance, color[1]/luminance, color[2]/luminance};
}

enum class EnvironmentMode : std::uint8_t { Off, Auto, Custom };
enum class SunSource : std::uint8_t { None, GameLight, WorldFrame };
enum class SunPreset : std::uint8_t { NeutralDaylight, WarmDaylight, Overcast, GoldenHour, Count };

struct EnvironmentSettings {
  EnvironmentMode mode = EnvironmentMode::Off;
  float strength = .65f;        // blend of the relit result with the original lighting
  float sun_intensity = 1.f;    // relative to the sky
  float sun_temperature = 5800.f;
  float sun_azimuth = 35.f, sun_elevation = 55.f; // degrees, WorldFrame sun only
  float sky_intensity = .55f;
  EnvVec3 sky_color{.62f, .76f, 1.f}, ground_color{.50f, .45f, .38f};
};

inline EnvironmentSettings ApplySunPreset(EnvironmentSettings s, SunPreset preset) noexcept {
  switch (preset) {
  case SunPreset::NeutralDaylight:
    s.sun_temperature = 5800; s.sun_intensity = 1.f; s.sun_elevation = 55; s.sky_intensity = .55f;
    s.sky_color = {.62f, .76f, 1.f}; s.ground_color = {.50f, .45f, .38f}; break;
  case SunPreset::WarmDaylight:
    s.sun_temperature = 4700; s.sun_intensity = 1.1f; s.sun_elevation = 42; s.sky_intensity = .5f;
    s.sky_color = {.66f, .76f, .98f}; s.ground_color = {.56f, .46f, .36f}; break;
  case SunPreset::Overcast:
    s.sun_temperature = 6800; s.sun_intensity = .25f; s.sun_elevation = 65; s.sky_intensity = .95f;
    s.sky_color = {.80f, .84f, .90f}; s.ground_color = {.55f, .54f, .52f}; break;
  case SunPreset::GoldenHour:
    s.sun_temperature = 3300; s.sun_intensity = 1.25f; s.sun_elevation = 12; s.sky_intensity = .45f;
    s.sky_color = {.55f, .62f, .92f}; s.ground_color = {.52f, .40f, .30f}; break;
  default: break;
  }
  return s;
}
inline bool ParseSunPreset(std::string_view name, SunPreset& preset) noexcept {
  constexpr std::pair<std::string_view, SunPreset> names[]{
      {"neutral", SunPreset::NeutralDaylight}, {"warm", SunPreset::WarmDaylight},
      {"overcast", SunPreset::Overcast}, {"golden", SunPreset::GoldenHour}};
  for (const auto& [key, value] : names)
    if (name == key) { preset = value; return true; }
  return false;
}

// Rotation part of a GX position matrix (rows of a 3x4 model-view matrix).
struct EnvRotation {
  std::array<EnvVec3, 3> axes{}; // model axes (columns) expressed in view space
  static bool FromRows(const float rows[3][4], EnvRotation& out) noexcept {
    for (unsigned c = 0; c < 3; ++c) {
      out.axes[c] = EnvNormalize({rows[0][c], rows[1][c], rows[2][c]});
      if (EnvDot(out.axes[c], out.axes[c]) < .5f) return false;
    }
    // Only rigid (possibly scaled) transforms describe a world frame. Some
    // engines use a left-handed (mirrored) world; its axes are just as stable.
    if (std::abs(EnvDot(out.axes[0], out.axes[1])) > .02f || std::abs(EnvDot(out.axes[0], out.axes[2])) > .02f ||
        std::abs(EnvDot(out.axes[1], out.axes[2])) > .02f) return false;
    return std::abs(EnvDot(EnvCross(out.axes[0], out.axes[1]), out.axes[2])) > .9f;
  }
  bool Near(const EnvRotation& o, float tolerance) const noexcept {
    for (unsigned c = 0; c < 3; ++c)
      if (EnvDot(axes[c], o.axes[c]) < 1.f - tolerance) return false;
    return true;
  }
};

// Why the environment changed in one Finish (diagnostics: every discontinuity
// is explained by one of these, never by what happens to be on screen).
enum EnvironmentEvent : std::uint32_t {
  EnvSceneReset = 1u << 0,  // no main-camera receivers for SceneGapFrames
  EnvCameraCut = 1u << 1,   // the dominant world rotation jumped
  EnvUpLocked = 1u << 2, EnvUpSwitched = 1u << 3,
  EnvSunLocked = 1u << 4, EnvSunSwitched = 1u << 5,
  EnvSunRejected = 1u << 6, // the game light turned with the camera
  EnvMainView = 1u << 7,    // another scene camera draws most receivers
};

struct EnvironmentFrame {
  bool valid = false;           // world vertical known (locked)
  int up_axis = -1;             // world axis locked as vertical (0..2), -1 unknown
  EnvVec3 horizontal{1, 0, 0}; // world grid direction, expressed in view space
  EnvVec3 up{0, 1, 0};          // view space
  EnvVec3 sun{0, 1, 0};         // view space direction towards the sun
  EnvVec3 sun_world{0, 1, 0};   // locked game sun in world coordinates
  EnvVec3 game_light_color{0, 0, 0};
  SunSource source = SunSource::None;
  float up_confidence = 0;
  // The game light turned with the camera / kept its world direction while
  // the camera turned. Only a proven world-stable light becomes the sun.
  bool light_camera_relative = false, light_world_stable = false;
  unsigned main_view = 0;       // scene camera id defining the world
  std::uint32_t events = 0;     // EnvironmentEvent bits
  // View-space position of a world-fixed point (the origin of static scenery
  // followed from pass to pass): sun cascades snap their texel grid to it.
  bool anchored = false;
  EnvVec3 anchor{0, 0, 0};
};

// Sun and vertical of the main scene camera, recovered without
// reconstructing a world frame (camera cuts, replays and object changes never
// carry a stale direction over):
// * the sun is the game's own directional light, read in view space every
//   pass (the game moves it into view space itself, so it is right after any
//   cut). Its identity is the GX light slot and color; a light shown to turn
//   with the camera is rejected for good;
// * the vertical is one model axis (locked from long-term evidence) of the
//   upright objects drawn in the pass: static scenery and characters stand
//   upright in their world.
class WorldFrameTracker {
public:
  static constexpr unsigned Candidates = 16, Lights = 16;
  // A different light becomes the sun once the sun went unseen for
  // UnseenPasses and that light then stayed the brightest for SwitchPasses.
  static constexpr unsigned UnseenPasses = 600, SwitchPasses = 60;
  // Vertical axis: first lock after AcquireUpdates passes; another axis must
  // stay clearly better for RelockUpdates to replace it.
  static constexpr unsigned AcquireUpdates = 8, RelockUpdates = 300;
  // Frames without main-camera receivers that end a scene (loading, menus).
  static constexpr std::uint64_t SceneGapFrames = 120;
  void BeginPass() noexcept { count_ = 0; sheared_ = 0; light_count_ = 0; same_ = false; }
  // Model-view rows of a receiver draw of scene camera `view` (1-based).
  void Observe(const float rows[3][4], float weight, unsigned view = 1) noexcept {
    // Consecutive draws mostly share their matrix (world chunks, one mesh).
    if (same_ && view == rows_view_ && std::memcmp(rows, rows_, sizeof(rows_)) == 0) {
      if (last_ < count_ && weight > 0) candidates_[last_].weight += weight;
      return;
    }
    std::memcpy(rows_, rows, sizeof(rows_)); rows_view_ = view; same_ = true; last_ = Candidates;
    EnvRotation rotation;
    if (!(weight > 0)) return;
    if (!EnvRotation::FromRows(rows, rotation)) { ++sheared_; return; }
    for (unsigned i = 0; i < count_; ++i)
      if (candidates_[i].view == view && candidates_[i].rotation.Near(rotation, 1e-4f)) {
        candidates_[i].weight += weight; last_ = i; AddOrigin(candidates_[i], rows); return;
      }
    if (count_ < Candidates) { candidates_[count_] = {rotation, weight, view}; AddOrigin(candidates_[count_], rows); last_ = count_++; }
  }
  // Receiver weight recorded since BeginPass (all cameras).
  float PendingWeight() const noexcept {
    float sum = 0;
    for (unsigned i = 0; i < count_; ++i) sum += candidates_[i].weight;
    return sum;
  }
  // A directional GX light (position at "infinity") lighting receivers, in
  // view space; `slot` is its GX light index.
  void ObserveDirectionalLight(const EnvVec3& view_position, const EnvVec3& color, unsigned slot = 0) noexcept {
    const float weight = EnvLuminance(color);
    if (!(weight > .05f)) return;
    const EnvVec3 dir = EnvNormalize(view_position);
    const std::uint32_t id = LightId(slot, color);
    for (unsigned i = 0; i < light_count_; ++i)
      if (lights_[i].id == id && EnvDot(lights_[i].dir, dir) > .985f) { ++lights_[i].uses; return; }
    if (light_count_ < Lights) lights_[light_count_++] = {id, dir, color, weight, 1};
  }
  // `frame` is the recording frame id (scene gaps are measured in frames).
  EnvironmentFrame Finish(const EnvironmentSettings& settings, std::uint64_t frame = 0) noexcept {
    EnvironmentFrame result;
    // A pass with a small fraction of the scene's receivers (a 3D icon, a
    // portrait in another camera) never redefines the environment: its
    // camera and lights would move the world's sun and vertical.
    float pass_weight = 0;
    for (unsigned i = 0; i < count_; ++i) pass_weight += candidates_[i].weight;
    // The reference decays (a smaller new scene, a menu, takes over within
    // a few seconds).
    if (count_) {
      scene_weight_ *= .995f;
      if (pass_weight < .05f * scene_weight_) { count_ = 0; light_count_ = 0; }
      else scene_weight_ = std::max(scene_weight_, pass_weight);
    }
    std::array<float, 16> per_view{};
    for (unsigned i = 0; i < count_; ++i) per_view[candidates_[i].view & 15] += candidates_[i].weight;
    unsigned main = 0;
    for (unsigned v = 1; v < per_view.size(); ++v) if (per_view[v] > per_view[main]) main = v;
    // The main camera changes only for one with twice its receivers: games
    // alternating a full-screen and a partial view keep one environment.
    if (main_view_ && main != main_view_ && per_view[main_view_ & 15] > 0 && 2 * per_view[main_view_ & 15] >= per_view[main])
      main = main_view_;
    const float total = per_view[main];
    const Candidate* heaviest = nullptr;
    for (unsigned i = 0; i < count_; ++i)
      if (candidates_[i].view == main && (!heaviest || candidates_[i].weight > heaviest->weight)) heaviest = &candidates_[i];
    evidence_ = {count_, heaviest ? heaviest->weight : 0.f, total, heaviest ? heaviest->rotation.axes : std::array<EnvVec3, 3>{},
                 sheared_};
    if (count_) {
      if (have_frame_ && frame > last_frame_ + SceneGapFrames) {
        // A new scene: its lights are unknown again (the vertical axis is the
        // game's convention and stays locked).
        sun_id_ = 0; rejected_ = {}; rejected_count_ = 0; light_score_ = 0; have_reference_ = false; have_sun_view_=false;have_anchor_=false;scene_weight_ = pass_weight;
        unseen_ = switch_count_ = 0; result.events |= EnvSceneReset;
      }
      have_frame_ = true; last_frame_ = frame;
      if (main && main != main_view_) { if (main_view_) result.events |= EnvMainView; main_view_ = main; }
    }
    // Vertical evidence from the main object of the pass.
    const bool updated = heaviest && heaviest->weight >= .35f * total && heaviest->weight >= 8;
    if (updated) {
      // Transport retained world directions through the actual camera delta.
      // When a light is absent from a pass, keeping its OLD view vector makes
      // it rotate with the camera. The grid and vertical need the same update.
      if(have_world_){
        const auto transport=[&](const EnvVec3& v){
          const EnvVec3 local{EnvDot(world_.axes[0],v),EnvDot(world_.axes[1],v),EnvDot(world_.axes[2],v)};
          EnvVec3 next{};for(unsigned a=0;a<3;++a)for(unsigned k=0;k<3;++k)next[k]+=heaviest->rotation.axes[a][k]*local[a];return EnvNormalize(next);
        };
        if(have_sun_view_)sun_view_=transport(sun_view_);
        if(have_grid_)grid_=transport(grid_);
        if(have_up_view_)up_view_=transport(up_view_);
      }
      world_ = heaviest->rotation; have_world_ = true; ++updates_;
      // Game cameras keep the world vertical upright on screen: a consistent
      // positive (or negative) view Y and no view X (no roll). A camera
      // circling above the scene averages the forward axis out.
      const float rate = std::max(.02f, 1.f / float(updates_));
      // Per image, a camera looking 60 degrees down on a Z-up world equals one
      // looking 30 degrees up on a Y-up world. Game cameras look level or
      // down: an axis that is upright on screen only if the camera looks up
      // (it points into the screen) is penalized.
      for (unsigned a = 0; a < 3; ++a) {
        const float x = std::abs(world_.axes[a][0]), y = world_.axes[a][1];
        const float towards = std::min(0.f, (y < 0 ? -world_.axes[a][2] : world_.axes[a][2]) * std::min(1.f, 4.f * std::abs(y)));
        sign_[a] += rate * (y - sign_[a]);
        slant_[a] += rate * (x - slant_[a]);
        facing_[a] += rate * (towards - facing_[a]);
        score_[a] = std::abs(sign_[a]) - 2.f * slant_[a] + 1.5f * facing_[a];
        swing_[a] = std::max(swing_[a], x);
      }
      UpdateVertical(result);
    }
    // The vertical of this pass: the locked axis of ONE group of objects that
    // agree on it within 2.5 degrees, never an average: animated rigid parts
    // (limbs, weapons, effects) would drag an average every frame and the
    // sun and shadows with it. Static scenery outweighs props: a rotation
    // shared by chunks at several world positions counts once per position.
    // Game cameras do not roll and do not look up for long, so a vertical
    // pointing sideways or into the screen is implausible.
    if (up_axis_ >= 0 && count_) {
      const unsigned a = unsigned(up_axis_);
      const EnvVec3 previous = have_up_view_ ? up_view_ : EnvVec3{0, sign_[a] < 0 ? -1.f : 1.f, 0};
      // A group's vertical is its dominant member's (static scenery chunks
      // share one exact rotation): members joining or leaving never move it.
      struct Group { EnvVec3 sum{}, axis{}; float score = 0, member = 0; };
      std::array<Group, Candidates> groups{};
      unsigned group_count = 0;
      for (unsigned i = 0; i < count_; ++i) {
        const auto& c = candidates_[i];
        if (c.view != main) continue;
        EnvVec3 axis = c.rotation.axes[a];
        if (EnvDot(axis, {0, previous[1] < 0 ? -1.f : 1.f, 0}) < 0) axis = {-axis[0], -axis[1], -axis[2]};
        const float weight = c.weight * float(std::clamp(c.origins, 1u, 8u));
        Group* g = nullptr;
        for (unsigned j = 0; j < group_count && !g; ++j)
          if (EnvDot(EnvNormalize(groups[j].sum), axis) > .999f) g = &groups[j];
        if (!g) g = &groups[group_count++];
        for (unsigned k = 0; k < 3; ++k) g->sum[k] += weight * axis[k];
        g->score += weight;
        if (weight > g->member) { g->member = weight; g->axis = axis; }
      }
      // Only the group nearest the last vertical (within 10 degrees) is kept.
      int nearest = -1;
      float nearest_dot = .985f;
      for (unsigned j = 0; j < group_count && have_up_view_; ++j)
        if (const float d = EnvDot(groups[j].axis, previous); d > nearest_dot) { nearest_dot = d; nearest = int(j); }
      const Group* best = nullptr;
      float best_score = 0;
      bool best_kept = false;
      for (unsigned j = 0; j < group_count; ++j) {
        const EnvVec3 up = groups[j].axis;
        const bool kept = int(j) == nearest;
        const float plausible = (std::abs(up[0]) < .5f ? 1.f : .1f) * (up[2] > -.5f ? 1.f : .25f);
        const float score = groups[j].score * plausible * (kept ? 4.f : 1.f);
        if (score > best_score) { best_score = score; best = &groups[j]; best_kept = kept; }
      }
      if (best) {
        if (have_up_view_ && !best_kept) result.events |= EnvCameraCut;
        up_view_ = EnvNormalize(best->axis); have_up_view_ = true;
      }
    }
    // The camera-relative test: while the main object turned (5 to 25
    // degrees since the reference), a world light kept its direction in that
    // object's coordinates; a camera-relative light did not.
    const Light* sun = Selected();
    if (sun && updated) {
      const EnvVec3 world = ToWorld(sun->dir);
      if (!have_reference_ || reference_id_ != sun->id || !world_.Near(reference_, .1f)) {
        have_reference_ = true; reference_ = world_; reference_id_ = sun->id; light_view_ = sun->dir; light_world_ = world;
      } else if (!world_.Near(reference_, 4e-3f)) {
        const float view_change = 1 - EnvDot(sun->dir, light_view_), world_change = 1 - EnvDot(world, light_world_);
        if (world_change < .5f * view_change) light_score_ = std::min(light_score_ + 1, 8);
        else if (view_change < .5f * world_change) light_score_ = std::max(light_score_ - 1, -8);
        reference_ = world_; light_view_ = sun->dir; light_world_ = world;
      }
      if (light_score_ <= -3) {
        if (rejected_count_ < rejected_.size()) rejected_[rejected_count_++] = sun_id_;
        sun_id_ = 0; light_score_ = 0; have_reference_ = false; result.events |= EnvSunRejected;
      }
    }
    // Sun selection by identity: the brightest light at first, kept while
    // lit draws use it; another light only after the sun went unseen for
    // UnseenPasses and it stayed the brightest for SwitchPasses.
    const Light* brightest = nullptr;
    for (unsigned i = 0; i < light_count_; ++i)
      if (!Rejected(lights_[i].id) && AboveHorizon(lights_[i].dir) && (!brightest || lights_[i].weight > brightest->weight))
        brightest = &lights_[i];
    if (Selected()) unseen_ = 0;
    else if (sun_id_) unseen_ = std::min(unseen_ + 1, UnseenPasses);
    if (!sun_id_ && brightest) {
      sun_id_ = brightest->id; light_score_ = 0; have_reference_ = false; switch_count_ = 0; result.events |= EnvSunLocked;
    } else if (sun_id_ && unseen_ >= UnseenPasses && brightest) {
      if (switch_count_ && brightest->id == switch_id_) ++switch_count_;
      else { switch_id_ = brightest->id; switch_count_ = 1; }
      if (switch_count_ >= SwitchPasses) {
        sun_id_ = brightest->id; light_score_ = 0; have_reference_ = false; unseen_ = switch_count_ = 0;
        result.events |= EnvSunSwitched;
      }
    }
    if (const Light* l = Selected()) { sun_view_ = l->dir; sun_color_ = l->color; have_sun_view_ = true; }
    result.light_camera_relative = rejected_count_ > 0;
    result.light_world_stable = light_score_ >= 1;
    if (have_up_view_ && up_axis_ >= 0 && count_) UpdateGrid(main);
    if (have_up_view_ && up_axis_ >= 0 && have_grid_) {
      result.valid = true; result.up_axis = up_axis_; result.up = up_view_;result.horizontal=grid_;
      result.up_confidence = std::clamp(score_[up_axis_], 0.f, 1.f);
      // World sun (Custom, or Auto without a game light): azimuth from the
      // level grid, elevation from the vertical.
      const EnvVec3 h1 = grid_, h2 = EnvNormalize(EnvCross(up_view_, h1));
      const float az = settings.sun_azimuth * 0.01745329252f, el = settings.sun_elevation * 0.01745329252f;
      for (unsigned i = 0; i < 3; ++i)
        result.sun[i] = std::cos(el) * (std::cos(az) * h1[i] + std::sin(az) * h2[i]) + std::sin(el) * up_view_[i];
      result.sun = EnvNormalize(result.sun);
      result.source = SunSource::WorldFrame;
    }
    if (sun_id_ && have_sun_view_) {
      result.game_light_color = sun_color_;
      result.sun_world = sun_view_;
      // Auto follows the game's own sun; Custom prefers the user direction.
      if (settings.mode != EnvironmentMode::Custom || result.source == SunSource::None) {
        result.sun = sun_view_; result.source = SunSource::GameLight;
      }
    }
    // Auto without a game light keeps the world sun: games with baked lighting
    // still get sun shadows from a fixed direction in their world.
    result.main_view = main_view_;
    if (updated) UpdateAnchor(*heaviest);
    result.anchored = have_anchor_ && updated;
    result.anchor = anchor_view_;
    return result;
  }
  void Reset() noexcept { const int prior = prior_; *this = {}; prior_ = prior; }
  // The game's vertical axis remembered from an earlier session (default Y).
  void SetPriorAxis(int axis) noexcept { prior_ = axis >= 0 && axis < 3 ? axis : 1; }
  unsigned Frames() const noexcept { return updates_; }
  // Internal state for traces.
  struct State { bool have_world, have_sun; int up_axis, light_score; unsigned unseen, switches;
                 EnvVec3 sun_world; std::array<EnvVec3, 3> world_axes; std::array<float, 3> score, sign, swing; };
  State Debug() const noexcept {
    return {have_world_, sun_id_ != 0, up_axis_, light_score_, unseen_, switch_count_, sun_view_, world_.axes, score_, sign_, swing_};
  }
  // Rotation candidates, dominant weight and total weight of the last pass.
  struct Evidence { unsigned candidates = 0; float dominant = 0, total = 0; std::array<EnvVec3, 3> heaviest{}; unsigned sheared = 0; };
  Evidence LastEvidence() const noexcept { return evidence_; }
  // Candidates of the pass being recorded (traces): weight, view, axes, first origin.
  template <class F> void ForEachCandidate(F&& f) const {
    for (unsigned i = 0; i < count_; ++i) f(candidates_[i].weight, candidates_[i].view, candidates_[i].rotation.axes,
                                            candidates_[i].origins ? candidates_[i].origin[0] : EnvVec3{}, candidates_[i].origins);
  }

private:
  static constexpr unsigned Origins = 8;
  struct Candidate {
    EnvRotation rotation; float weight = 0; unsigned view = 0;
    unsigned origins = 0; std::array<EnvVec3, Origins> origin{};
  };
  // Static scenery shares one view rotation; its chunks keep their own
  // world-fixed origins (model translations).
  static void AddOrigin(Candidate& c, const float rows[3][4]) noexcept {
    const EnvVec3 o{rows[0][3], rows[1][3], rows[2][3]};
    for (unsigned i = 0; i < c.origins; ++i)
      if (std::abs(c.origin[i][0] - o[0]) + std::abs(c.origin[i][1] - o[1]) + std::abs(c.origin[i][2] - o[2]) <
          1e-4f * (1.f + std::abs(o[0]) + std::abs(o[1]) + std::abs(o[2]))) return;
    if (c.origins < Origins) c.origin[c.origins++] = o;
  }
  // The anchor follows one world point: of the main object's origins, the one
  // whose camera offset (in world axes) moved least since the last pass.
  void UpdateAnchor(const Candidate& c) noexcept {
    if (!c.origins) return;
    unsigned best = 0; float best_distance = 0;
    for (unsigned i = 0; i < c.origins; ++i) {
      const EnvVec3 w = ToWorld(c.origin[i]), d{w[0] - anchor_world_[0], w[1] - anchor_world_[1], w[2] - anchor_world_[2]};
      const float distance = EnvDot(d, d);
      if (i == 0 || distance < best_distance) { best = i; best_distance = distance; }
    }
    anchor_view_ = c.origin[best]; anchor_world_ = ToWorld(anchor_view_); have_anchor_ = true;
  }
  struct Light { std::uint32_t id = 0; EnvVec3 dir{}, color{}; float weight = 0; unsigned uses = 0; };
  static std::uint32_t LightId(unsigned slot, const EnvVec3& color) noexcept {
    const float maximum=std::max({color[0],color[1],color[2],.0001f});
    const auto q = [maximum](float v) { return std::uint32_t(std::clamp(v/maximum, 0.f, 1.f) * 63.f + .5f); };
    return 1u + ((slot & 15u) | q(color[0]) << 4 | q(color[1]) << 10 | q(color[2]) << 16);
  }
  // A sun lights the scene from above: a game light pointing below the
  // horizon (a fill or rim light, or one set from below) is never the sun.
  bool AboveHorizon(const EnvVec3& dir) const noexcept {
    return !(have_up_view_ && up_axis_ >= 0) || EnvDot(dir, up_view_) > .05f;
  }
  bool Rejected(std::uint32_t id) const noexcept {
    for (unsigned i = 0; i < rejected_count_; ++i) if (rejected_[i] == id) return true;
    return false;
  }
  // This pass's observation of the sun: its most used direction.
  // The sun light seen this pass. A game can set the same light in several
  // directions within a frame (per object or per view): the direction near
  // the last one is kept unless another is used four times as often.
  const Light* Selected() const noexcept {
    const Light* best = nullptr;
    float best_score = 0;
    for (unsigned i = 0; i < light_count_; ++i) {
      if (!sun_id_ || lights_[i].id != sun_id_ || !AboveHorizon(lights_[i].dir)) continue;
      const bool kept = have_sun_view_ && EnvDot(lights_[i].dir, sun_view_) > .9f;
      const float score = float(lights_[i].uses) * (kept ? 4.f : 1.f);
      if (!best || score > best_score) { best = &lights_[i]; best_score = score; }
    }
    return best;
  }
  // The level grid: the dominant horizontal orientation, modulo 90 degrees,
  // of the upright objects (static geometry and axis-aligned props agree),
  // in view space. Unwrapped against the previous pass so it never jumps
  // by a quarter turn while the camera turns.
  void UpdateGrid(unsigned main) noexcept {
    EnvVec3 b1 = {1, 0, 0};
    float along = EnvDot(b1, up_view_);
    if (std::abs(along) > .9f) { b1 = {0, 0, 1}; along = EnvDot(b1, up_view_); }
    b1 = EnvNormalize({b1[0] - along * up_view_[0], b1[1] - along * up_view_[1], b1[2] - along * up_view_[2]});
    const EnvVec3 b2 = EnvNormalize(EnvCross(up_view_, b1));
    // Upright objects grouped by yaw (modulo a quarter turn, as 4 x yaw).
    // The grid is ONE group, not an average: props and characters at their
    // own yaw coming into view never turn it. The group of the last grid
    // keeps it unless another outweighs it four times.
    struct Group { float c = 0, s = 0, weight = 0; };
    std::array<Group, Candidates> groups{};
    unsigned group_count = 0;
    const unsigned a = unsigned(up_axis_), k = (a + 1) % 3;
    for (unsigned i = 0; i < count_; ++i) {
      const auto& c = candidates_[i];
      if (c.view != main || std::abs(EnvDot(c.rotation.axes[a], up_view_)) < .95f) continue;
      const float angle = 4.f * std::atan2(EnvDot(c.rotation.axes[k], b2), EnvDot(c.rotation.axes[k], b1));
      const float cs = std::cos(angle), sn = std::sin(angle);
      Group* g = nullptr;
      for (unsigned j = 0; j < group_count && !g; ++j)
        if ((groups[j].c * cs + groups[j].s * sn) > .99f * groups[j].weight) g = &groups[j]; // within 2 degrees
      if (!g) g = &groups[group_count++];
      const float w = c.weight * float(std::clamp(c.origins, 1u, 8u));
      g->c += w * cs; g->s += w * sn; g->weight += w;
    }
    if (!group_count) return;
    const float quarter = 1.5707963f;
    const float previous = have_grid_ ? std::atan2(EnvDot(grid_, b2), EnvDot(grid_, b1)) : 0.f;
    const Group* best = nullptr;
    float best_score = 0;
    for (unsigned j = 0; j < group_count; ++j) {
      const auto& g = groups[j];
      // Within 8 degrees of the last grid (the camera turned at most that).
      const bool kept = have_grid_ && (g.c * std::cos(4.f * previous) + g.s * std::sin(4.f * previous)) > .85f * g.weight;
      const float score = g.weight * (kept ? 4.f : 1.f);
      if (score > best_score) { best_score = score; best = &g; }
    }
    float phi = std::atan2(best->s, best->c) / 4.f;
    // The quarter turn closest to the previous grid direction.
    if (have_grid_) phi += quarter * std::round((previous - phi) / quarter);
    for (unsigned i = 0; i < 3; ++i) grid_[i] = std::cos(phi) * b1[i] + std::sin(phi) * b2[i];
    grid_ = EnvNormalize(grid_); have_grid_ = true;
  }
  void UpdateVertical(EnvironmentFrame& result) noexcept {
    if (updates_ < AcquireUpdates) return;
    unsigned best = 0, second = 1;
    if (score_[second] > score_[best]) std::swap(best, second);
    if (score_[2] > score_[best]) { second = best; best = 2; } else if (score_[2] > score_[second]) second = 2;
    // The game's known convention (Y, or the axis remembered from an earlier
    // session) locks at once; another axis needs a clearer margin and
    // horizontal axes that swung sideways: per image, a level camera in a
    // Z-up world and a camera circling above a Y-up scene are identical.
    const bool known = int(best) == prior_;
    const float margin = known ? .15f : .3f;
    // A still camera gives no swing; the default convention is still refuted
    // when it would mean the camera always looks up (a steep town camera
    // over a Z-up world).
    const bool refuted = prior_ >= 0 && int(best) != prior_ && facing_[prior_] < -.3f && facing_[best] > -.05f;
    const bool swung = known || refuted || std::min(swing_[(best + 1) % 3], swing_[(best + 2) % 3]) > .3f;
    if (up_axis_ < 0) {
      if (swung && score_[best] > .3f && score_[best] - score_[second] > margin) {
        up_axis_ = int(best); have_up_view_ = have_grid_ = false; result.events |= EnvUpLocked;
      }
      return;
    }
    if (int(best) != up_axis_ && swung && score_[best] > .3f && score_[best] > score_[up_axis_] + margin + .15f) {
      if (++relock_ >= RelockUpdates) { up_axis_ = int(best); have_up_view_ = have_grid_ = false; relock_ = 0; result.events |= EnvUpSwitched; }
    } else relock_ = 0;
  }
  EnvVec3 ToWorld(const EnvVec3& v) const noexcept {
    return {EnvDot(world_.axes[0], v), EnvDot(world_.axes[1], v), EnvDot(world_.axes[2], v)};
  }
  std::array<Candidate, Candidates> candidates_{};
  std::array<Light, Lights> lights_{};
  unsigned count_ = 0, sheared_ = 0, last_ = 0, light_count_ = 0, updates_ = 0, rows_view_ = 0, main_view_ = 0, relock_ = 0;
  float rows_[3][4]{}, scene_weight_ = 0;
  Evidence evidence_{};
  bool same_ = false, have_frame_ = false, have_world_ = false, have_reference_ = false, have_up_view_ = false,
       have_sun_view_ = false, have_grid_ = false, have_anchor_ = false;
  EnvVec3 grid_{1, 0, 0};
  std::uint64_t last_frame_ = 0;
  EnvRotation world_{}, reference_{};
  int light_score_ = 0, up_axis_ = -1, prior_ = 1;
  std::uint32_t sun_id_ = 0, switch_id_ = 0, reference_id_ = 0;
  std::array<std::uint32_t, 8> rejected_{};
  unsigned rejected_count_ = 0, switch_count_ = 0, unseen_ = 0;
  EnvVec3 anchor_view_{}, anchor_world_{}, light_view_{}, light_world_{}, up_view_{0, 1, 0}, sun_view_{0, 1, 0}, sun_color_{0, 0, 0};
  std::array<float, 3> score_{}, sign_{}, slant_{}, facing_{}, swing_{};
};

// Confidence (0..1) in the vertical axis and in the sun, filtered over frames
// by the renderer: a legitimate change fades in or out, it never switches.
struct EnvironmentConfidence { float sky = 0, sun = 0; };

// Uniform data of the deferred relight, std140 friendly. The relight is
//   (sky term * sky weight + sun term * sun weight) / (sky + sun weight)
// with unit-mean sky colors and a unit-luminance sun, so a fading sun or sky
// never changes the average exposure, only the directional shading.
struct EnvironmentUniforms {
  std::array<float, 4> sun{0, 1, 0, 0};       // view-space direction (also for shadows), weight
  std::array<float, 4> sun_color{1, 1, 1, 0}; // rgb, strength (0: environment light off)
  std::array<float, 4> sky{1, 1, 1, 0};       // rgb / mean luminance, weight
  std::array<float, 4> ground{1, 1, 1, 0};    // rgb / mean luminance, sky confidence
  std::array<float, 4> up{0, 1, 0, 0};        // view-space up, 1 when locked
};
inline EnvironmentUniforms MakeEnvironmentUniforms(const EnvironmentSettings& s, const EnvironmentFrame& f,
                                                   const EnvironmentConfidence& c = {1, 1}) noexcept {
  EnvironmentUniforms u;
  u.sun = {f.sun[0], f.sun[1], f.sun[2], 0};
  u.up = {f.up[0], f.up[1], f.up[2], f.valid ? 1.f : 0.f};
  if (s.mode == EnvironmentMode::Off) return u;
  const EnvVec3 color = ColorTemperature(s.sun_temperature);
  // Auto keeps the game's hues: the sky only shades by orientation. Custom
  // uses the preset's sky and ground colors.
  constexpr EnvVec3 neutral_sky{1, 1, 1}, neutral_ground{.78f, .78f, .78f};
  const bool custom = s.mode == EnvironmentMode::Custom;
  const auto& sc = custom ? s.sky_color : neutral_sky; const auto& gc = custom ? s.ground_color : neutral_ground;
  const float mean = std::max(EnvLuminance({.5f*(sc[0]+gc[0]), .5f*(sc[1]+gc[1]), .5f*(sc[2]+gc[2])}), 1e-4f);
  const bool sun = f.source != SunSource::None;
  u.sun[3] = sun ? std::max(s.sun_intensity, 0.f) * std::clamp(c.sun, 0.f, 1.f) : 0.f;
  u.sun_color = {color[0], color[1], color[2], std::clamp(s.strength, 0.f, 1.f)};
  u.sky = {sc[0] / mean, sc[1] / mean, sc[2] / mean, std::max(s.sky_intensity, 0.f)};
  u.ground = {gc[0] / mean, gc[1] / mean, gc[2] / mean, f.valid ? std::clamp(c.sky, 0.f, 1.f) : 0.f};
  return u;
}
} // namespace GekkoAOT::GX
#endif
