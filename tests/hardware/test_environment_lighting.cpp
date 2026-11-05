// SPDX-License-Identifier: GPL-3.0-or-later
#include "../../runtime/gx/environment_lighting.h"
#include <cstdio>
#include <cstdlib>
#define CHECK(x) do{if(!(x)){std::fprintf(stderr,"%d: %s\n",__LINE__,#x);std::exit(1);}}while(false)
using namespace GekkoAOT::GX;
namespace {
using M3 = std::array<std::array<float,3>,3>;
M3 Mul(const M3& a,const M3& b){M3 r{};for(int i=0;i<3;++i)for(int j=0;j<3;++j)for(int k=0;k<3;++k)r[i][j]+=a[i][k]*b[k][j];return r;}
M3 RotX(float a){return {{{1,0,0},{0,std::cos(a),-std::sin(a)},{0,std::sin(a),std::cos(a)}}};}
M3 RotY(float a){return {{{std::cos(a),0,std::sin(a)},{0,1,0},{-std::sin(a),0,std::cos(a)}}};}
M3 RotZ(float a){return {{{std::cos(a),-std::sin(a),0},{std::sin(a),std::cos(a),0},{0,0,1}}};}
const M3 Identity{{{1,0,0},{0,1,0},{0,0,1}}};
void Rows(const M3& m,float scale,float rows[3][4]){for(int r=0;r<3;++r){for(int c=0;c<3;++c)rows[r][c]=m[r][c]*scale;rows[r][3]=10.f*r;}}
EnvVec3 Apply(const M3& m,const EnvVec3& v){return {m[0][0]*v[0]+m[0][1]*v[1]+m[0][2]*v[2],m[1][0]*v[0]+m[1][1]*v[1]+m[1][2]*v[2],m[2][0]*v[0]+m[2][1]*v[1]+m[2][2]*v[2]};}
EnvVec3 ApplyT(const M3& m,const EnvVec3& v){return {m[0][0]*v[0]+m[1][0]*v[1]+m[2][0]*v[2],m[0][1]*v[0]+m[1][1]*v[1]+m[2][1]*v[2],m[0][2]*v[0]+m[1][2]*v[1]+m[2][2]*v[2]};}
EnvVec3 Scaled(const EnvVec3& v,float s){return {v[0]*s,v[1]*s,v[2]*s};}
enum class Light { None, World, Camera };
// A game frame: world chunks drawn with the view matrix, characters with
// their own (upright, yawing) rotations, optionally the game's light.
struct Scene {
  WorldFrameTracker tracker;
  bool zUp = false; // characters turn about the world vertical
  float propWeight = 0, propYaw = 0; // a large prop standing at its own yaw
  const EnvVec3 sunWorld = EnvNormalize({.3f, .9f, .2f});
  EnvironmentFrame Pass(const M3& view, const EnvironmentSettings& s, unsigned k, Light light = Light::None,
                        bool characters = true) {
    tracker.BeginPass(); float rows[3][4];
    for (int chunk = 0; chunk < 40; ++chunk) { Rows(view, 1.f, rows); tracker.Observe(rows, 1.f, 1); }
    if (characters)
      for (int c = 0; c < 6; ++c) {
        const float a = .3f * c + k;
        Rows(Mul(view, zUp ? RotZ(a) : RotY(a)), 2.f, rows); tracker.Observe(rows, 4.f, 1);
      }
    if (propWeight > 0) { Rows(Mul(view, zUp ? RotZ(propYaw) : RotY(propYaw)), 1.f, rows); tracker.Observe(rows, propWeight, 1); }
    if (light == Light::World) tracker.ObserveDirectionalLight(Scaled(Apply(view, sunWorld), 1e9f), {1, 1, 1}, 0);
    if (light == Light::Camera) tracker.ObserveDirectionalLight({0, 6e8f, 8e8f}, {1, 1, 1}, 0);
    return tracker.Finish(s, k);
  }
};
M3 Camera(unsigned k, float pan = .11f, float pitch = -.25f, float roll = 0, const M3& world = Identity) {
  // Negative pitch looks down (the world's up then points towards the camera).
  return Mul(Mul(Mul(RotZ(roll), RotX(-pitch + .2f * std::sin(k * .07f))), RotY(k * pan)), world);
}
}
int main() {
  // Temperature tints keep unit luminance; warm light is red-heavy.
  const auto warm = ColorTemperature(3300), cool = ColorTemperature(9000);
  CHECK(std::abs(EnvLuminance(warm) - 1) < 1e-4f && std::abs(EnvLuminance(cool) - 1) < 1e-4f);
  CHECK(warm[0] > warm[2] && cool[2] > cool[0]);
  SunPreset preset;CHECK(ParseSunPreset("golden", preset) && preset == SunPreset::GoldenHour && !ParseSunPreset("noon", preset));
  EnvironmentSettings custom = ApplySunPreset({}, SunPreset::NeutralDaylight);custom.mode = EnvironmentMode::Custom;
  CHECK(ApplySunPreset(custom, SunPreset::Overcast).sun_intensity < custom.sun_intensity);
  EnvironmentSettings automatic = custom; automatic.mode = EnvironmentMode::Auto;

  // Y-up, Z-up and left-handed worlds: the vertical is the world's own axis
  // in view space every pass, and the Custom sun keeps its world elevation.
  const M3 zUp = RotX(-1.5707963f), mirror{{{-1,0,0},{0,1,0},{0,0,1}}};
  for (const M3* world : {&Identity, &zUp}) for (bool mirrored : {false, true}) {
    const M3 w = mirrored ? Mul(*world, mirror) : *world;
    const EnvVec3 upWorld = world == &zUp ? EnvVec3{0, 0, 1} : EnvVec3{0, 1, 0};
    Scene scene; scene.zUp = world == &zUp; EnvironmentFrame f;
    for (unsigned k = 0; k < 120; ++k) {
      const M3 view = Camera(k, .11f, -.25f, 0, w);
      scene.propWeight = (k / 7) % 2 ? 30.f : 0.f; scene.propYaw = .35f;
      f = scene.Pass(view, custom, k);
      if (k >= 20) {
        CHECK(f.valid && f.up_axis == (world == &zUp ? 2 : 1) && f.source == SunSource::WorldFrame);
        CHECK(EnvDot(f.up, Apply(view, upWorld)) > .999f);
        CHECK(f.anchored && f.anchor[0] == 0 && f.anchor[1] == 10 && f.anchor[2] == 20); // scenery origin
        // Characters turning in place never move the Custom sun in the world.
        static EnvVec3 first; const EnvVec3 world_sun = ApplyT(view, f.sun);
        if (k == 20) first = world_sun;
        CHECK(EnvDot(world_sun, first) > .9995f);
        CHECK(std::abs(EnvDot(ApplyT(view, f.sun), upWorld) - std::sin(custom.sun_elevation * .01745329f)) < 1e-3f);
      }
    }
  }
  // A steep top-down camera still finds the vertical; a rolling camera, a
  // camera circling above the scene never lock a wrong one; a static steep
  // camera in a Z-up world is corrected once the camera turns.
  {
    Scene steep; EnvironmentFrame f;
    for (unsigned k = 0; k < 120; ++k) f = steep.Pass(Camera(k, .11f, -1.1f), custom, k);
    CHECK(f.valid && f.up_axis == 1 && EnvDot(f.up, Apply(Camera(119, .11f, -1.1f), {0, 1, 0})) > .999f);
    Scene rolling; rolling.zUp = true;
    for (unsigned k = 0; k < 120; ++k) CHECK(!rolling.Pass(Camera(k, .11f, -.25f, .4f, zUp), custom, k).valid);
    Scene still;
    // (Y is assumed while nothing tells it apart; turning corrects it.)
    for (unsigned k = 0; k < 120; ++k) still.Pass(Camera(k, 0.f, -1.1f, 0, zUp), custom, k, Light::None, false);
    EnvironmentFrame corrected;
    for (unsigned k = 120; k < 900; ++k) corrected = still.Pass(Camera(k, .11f, -.3f, 0, zUp), custom, k, Light::None, false);
    CHECK(corrected.valid && corrected.up_axis == 2);
    Scene circling;
    for (unsigned k = 0; k < 400; ++k) CHECK(!circling.Pass(Mul(RotX(1.45f), RotY(k * .05f)), custom, k, Light::None, false).valid);
    // A still camera looking 60 degrees down on a Z-up world (Tales of
    // Symphonia towns): Y would mean a camera looking up, Z locks.
    Scene town; town.zUp = true; EnvironmentFrame t;
    const M3 townView = Mul(RotX(1.05f), zUp);
    for (unsigned k = 0; k < 40; ++k) t = town.Pass(townView, automatic, k, Light::None, false);
    CHECK(t.valid && t.up_axis == 2 && EnvDot(t.up, Apply(townView, {0, 0, 1})) > .999f);
  }
  // A level camera in a Z-up world that never turned: with the axis
  // remembered from an earlier session, the vertical and the world sun are
  // there within ten passes.
  {
    Scene scene; scene.zUp = true; scene.tracker.SetPriorAxis(2); EnvironmentFrame f;
    for (unsigned k = 0; k < 10; ++k) f = scene.Pass(Camera(0, 0.f, -.1f, 0, zUp), automatic, k, Light::None, false);
    CHECK(f.valid && f.up_axis == 2 && f.source == SunSource::WorldFrame);
  }
  // Auto without a game light (baked lighting): a sun fixed in the world
  // from the level grid while the camera turns; neutral sky, unit mean.
  {
    Scene scene; EnvironmentFrame f; EnvVec3 first{};
    for (unsigned k = 0; k < 200; ++k) {
      const M3 view = Camera(k);
      f = scene.Pass(view, automatic, k);
      if (k == 20) first = ApplyT(view, f.sun);
      if (k > 20) CHECK(f.source == SunSource::WorldFrame && EnvDot(ApplyT(view, f.sun), first) > .995f);
    }
    const auto u = MakeEnvironmentUniforms(automatic, f);
    CHECK(u.sun[3] > 0 && u.up[3] == 1 && u.ground[3] == 1);
    const float mean = EnvLuminance({.5f*(u.sky[0]+u.ground[0]), .5f*(u.sky[1]+u.ground[1]), .5f*(u.sky[2]+u.ground[2])});
    CHECK(std::abs(mean - 1) < 1e-4f && u.sky[2] == u.sky[0]);
    CHECK(MakeEnvironmentUniforms(custom, f).sky[2] > MakeEnvironmentUniforms(custom, f).sky[0]);
    CHECK(MakeEnvironmentUniforms(automatic, f, {.25f, 1}).ground[3] == .25f);
    EnvironmentFrame unknown; CHECK(MakeEnvironmentUniforms(automatic, unknown).ground[3] == 0);
  }
  // The game's light is the sun from the first pass, and its view direction
  // is the game's own every pass: it stays fixed in the world while the
  // camera turns, and is right at once after a camera cut.
  {
    Scene scene; EnvironmentFrame f;
    for (unsigned k = 0; k < 200; ++k) {
      const M3 view = k < 100 ? Camera(k) : Camera(k + 1000, .11f, -.6f); // a cut at k = 100
      f = scene.Pass(view, automatic, k, Light::World);
      CHECK(f.source == SunSource::GameLight && EnvDot(f.sun, Apply(view, scene.sunWorld)) > .9999f);
      if (k >= 10) CHECK(f.valid && EnvDot(f.up, Apply(view, {0, 1, 0})) > .999f);
    }
    auto u = MakeEnvironmentUniforms(automatic, f);
    CHECK(u.sun[3] > 0 && u.sun_color[3] > 0);
    CHECK(MakeEnvironmentUniforms(automatic, f, {1, .5f}).sun[3] == .5f * u.sun[3]);
    // Off: no relight, but the direction stays available to the shadows.
    const auto off = MakeEnvironmentUniforms(EnvironmentSettings{}, f);
    CHECK(off.sun_color[3] == 0 && off.sun[3] == 0 && EnvDot({off.sun[0], off.sun[1], off.sun[2]}, f.sun) > .9999f);
  }
  // Draws may omit the selected light or vary its brightness. Neither may
  // rotate the retained sun with a moving camera or change its identity.
  {
    Scene scene; EnvironmentFrame f;
    for(unsigned k=0;k<20;++k)f=scene.Pass(Camera(k),automatic,k,Light::World);
    for(unsigned k=20;k<100;++k){
      const auto view=Camera(k);
      f=scene.Pass(view,automatic,k,Light::None);
      CHECK(f.source==SunSource::GameLight);
      CHECK(EnvDot(ApplyT(view,f.sun),scene.sunWorld)>.9999f);
    }
    for(unsigned k=100;k<140;++k){
      const auto view=Camera(k);float rows[3][4];Rows(view,1,rows);
      scene.tracker.BeginPass();scene.tracker.Observe(rows,40,1);
      const float brightness=k%2?.25f:1.f;
      scene.tracker.ObserveDirectionalLight(Scaled(Apply(view,scene.sunWorld),1e9f),{brightness,brightness,brightness},0);
      f=scene.tracker.Finish(automatic,k);
      CHECK(f.source==SunSource::GameLight && !(f.events&EnvSunSwitched));
      CHECK(EnvDot(ApplyT(view,f.sun),scene.sunWorld)>.9999f);
    }
  }
  // A light fixed to the camera is rejected once turning shows it.
  {
    Scene scene; EnvironmentFrame f; bool rejected = false;
    for (unsigned k = 0; k < 120; ++k) { f = scene.Pass(Camera(k), automatic, k, Light::Camera); rejected |= (f.events & EnvSunRejected) != 0; }
    CHECK(rejected && f.source != SunSource::GameLight && f.light_camera_relative);
  }
  // Sun identity: a brighter light used by some draws while the sun is still
  // in use never takes over; once the sun went unseen, a light that stays
  // the brightest replaces it.
  {
    Scene scene; EnvironmentFrame f;
    for (unsigned k = 0; k < 20; ++k) f = scene.Pass(Camera(k), automatic, k, Light::World);
    for (unsigned k = 20; k < 2000; ++k) {
      const M3 view = Camera(k);
      scene.tracker.BeginPass(); float rows[3][4]; Rows(view, 1, rows); scene.tracker.Observe(rows, 40, 1);
      if (k % 3 == 1) scene.tracker.ObserveDirectionalLight({0, 3e9f, 4e9f}, {1, 1, 1}, 2);
      if (k % 3 == 2) scene.tracker.ObserveDirectionalLight(Scaled(Apply(view, scene.sunWorld), 1e9f), {1, 1, 1}, 0);
      f = scene.tracker.Finish(automatic, k);
      CHECK(f.source == SunSource::GameLight && !(f.events & EnvSunSwitched));
    }
    bool switched = false;
    for (unsigned k = 2000; k < 2000 + WorldFrameTracker::UnseenPasses + WorldFrameTracker::SwitchPasses; ++k) {
      scene.tracker.BeginPass(); float rows[3][4]; Rows(Camera(k), 1, rows); scene.tracker.Observe(rows, 40, 1);
      scene.tracker.ObserveDirectionalLight({0, 3e9f, 4e9f}, {1, 1, 1}, 2);
      f = scene.tracker.Finish(automatic, k); switched |= (f.events & EnvSunSwitched) != 0;
    }
    CHECK(switched && std::abs(f.sun[1] - .6f) < 1e-4f && std::abs(f.sun[2] - .8f) < 1e-4f);
  }
  // Another scene camera (an inset, a texture view) never defines the world,
  // and a gap without receivers ends the scene and its sun.
  {
    WorldFrameTracker t; float world[3][4], inset[3][4]; EnvironmentFrame f;
    const M3 view = Mul(RotX(-.3f), RotY(.4f)), other = RotZ(1.f);
    for (unsigned pass = 0; pass < 10; ++pass) {
      t.BeginPass(); Rows(view, 1, world); Rows(other, 1, inset);
      t.Observe(world, 100, 1); t.Observe(inset, 60, 2); t.Observe(inset, 60, 2);
      f = t.Finish(automatic, pass);
    }
    CHECK(f.main_view == 2); // 120 vertices beat 100
    t.BeginPass(); t.Observe(world, 300, 1); t.ObserveDirectionalLight({0, 1e9f, 0}, {1, 1, 1}); f = t.Finish(automatic, 11);
    CHECK(f.main_view == 1 && (f.events & EnvMainView) && f.source == SunSource::GameLight);
    t.BeginPass(); t.Observe(world, 300, 1);
    f = t.Finish(automatic, 11 + WorldFrameTracker::SceneGapFrames + 1);
    CHECK((f.events & EnvSceneReset) && f.source != SunSource::GameLight);
  }
  // Sheared matrices never define an object frame; mirrored ones may.
  float sheared[3][4]{{1, .5f, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}}; EnvRotation rotation;
  CHECK(!EnvRotation::FromRows(sheared, rotation));
  float mirrored[3][4]{{-1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}};
  CHECK(EnvRotation::FromRows(mirrored, rotation));
  // Tales of Symphonia battle: the ground is 8 chunks (428 vertices) of a
  // Z-up world; animated rigid pieces (weapons, limbs, effects) at arbitrary
  // tilts weigh more. After the cut into the battle, the vertical is the
  // ground's every pass and does not wobble while the pieces animate.
  {
    const M3 zUpWorld = RotX(-1.5707963f);
    Scene village; village.zUp = true; EnvironmentFrame v;
    for (unsigned k = 0; k < 80; ++k) v = village.Pass(Camera(k, .11f, -.6f, 0, zUpWorld), automatic, k);
    CHECK(v.valid && v.up_axis == 2);
    auto& tracker = village.tracker;
    const float pieces[]{1008, 540, 540, 464, 464, 312, 256, 208};
    for (unsigned k = 0; k < 120; ++k) {
      const M3 view = Mul(Mul(RotX(.3f), RotY(.4f + .004f * k)), zUpWorld);
      tracker.BeginPass(); float rows[3][4];
      for (unsigned chunk = 0; chunk < 8; ++chunk) {
        Rows(view, 1.f, rows); rows[0][3] = 40.f * chunk; rows[2][3] = -2000.f - 30.f * chunk;
        tracker.Observe(rows, 53.5f, 1);
      }
      for (unsigned i = 0; i < 8; ++i) {
        const float t = .7f * i + .05f * k;
        Rows(Mul(view, Mul(RotX(1.1f * std::sin(t)), RotY(2.f * t))), 1.f, rows);
        tracker.Observe(rows, pieces[i], 1);
      }
      const EnvironmentFrame b = tracker.Finish(automatic, 200 + k);
      if (k >= 1) CHECK(b.valid && EnvDot(b.up, Apply(view, {0, 0, 1})) > .9999f);
    }
  }
  // Animaniacs: the game sets its sun light in two directions within one
  // frame, the per-frame majority alternating. The sun keeps its direction.
  {
    Scene scene; EnvironmentFrame f, first;
    const EnvVec3 a = EnvNormalize({.6f, .7f, .3f}), b{0, -1, 0};
    for (unsigned k = 0; k < 60; ++k) {
      const M3 view = Camera(k, .02f, -.25f);
      scene.tracker.BeginPass(); float rows[3][4];
      for (int chunk = 0; chunk < 40; ++chunk) { Rows(view, 1.f, rows); scene.tracker.Observe(rows, 1.f, 1); }
      const unsigned na = 10, nb = k % 2 ? 14 : 6;
      for (unsigned u = 0; u < na; ++u) scene.tracker.ObserveDirectionalLight(Scaled(Apply(view, a), 1e9f), {1, 1, 1}, 0);
      for (unsigned u = 0; u < nb; ++u) scene.tracker.ObserveDirectionalLight(Scaled(Apply(view, b), 1e9f), {1, 1, 1}, 0);
      f = scene.tracker.Finish(automatic, k);
      if (k == 5) first = f;
      if (k >= 5) CHECK(f.source == SunSource::GameLight && EnvDot(ApplyT(view, f.sun), ApplyT(Camera(5, .02f, -.25f), first.sun)) > .999f);
    }
  }
  // Animaniacs: a pass drawing only a small object in another camera (164
  // vertices against 40000) with its own light below the horizon leaves the
  // world's vertical and sun untouched.
  {
    Scene scene; EnvironmentFrame f;
    for (unsigned k = 0; k < 40; ++k) f = scene.Pass(Camera(k, .02f, -.25f), automatic, k, Light::World);
    const EnvVec3 up = f.up, sun = f.sun;
    scene.tracker.BeginPass(); float rows[3][4];
    Rows(RotZ(1.2f), 1.f, rows); scene.tracker.Observe(rows, 164.f / 40000.f * 64.f, 6);
    scene.tracker.ObserveDirectionalLight({0, -1e9f, 0}, {1, 1, 1}, 0);
    const EnvironmentFrame icon = scene.tracker.Finish(automatic, 40);
    CHECK(icon.valid && EnvDot(icon.up, up) > .9999f && EnvDot(icon.sun, sun) > .9999f && !(icon.events & EnvMainView));
  }
  std::puts("Environment lighting: temperature, presets, Y/Z-up and left-handed verticals, live game sun across camera cuts, "
            "camera-relative rejection, sun identity, main camera, scene reset, battle vertical, two-direction sun, minor pass passed");
}
