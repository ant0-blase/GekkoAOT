// SPDX-License-Identifier: GPL-3.0-or-later
#include "../../runtime/gx/graphics_settings.h"
#include <cstdlib>
#include <iostream>
#define CHECK(x) do { if(!(x)){std::cerr << __LINE__ << ": " #x "\n"; std::exit(1);} } while(false)
int main() {
  using namespace GekkoAOT::GX;
  GraphicsSettings s;
  CHECK(s.brightness==1 && s.contrast==1 && s.saturation==1 && s.gamma==1);
  CHECK(ParseGraphicsSetting(s,"post_saturation","0") && s.saturation==0);
  CHECK(ParseGraphicsSetting(s,"post_brightness","1.75") && s.brightness==1.75f);
  CHECK(ParseGraphicsSetting(s,"render_percent","99999") && s.render_percent==400);
  CHECK(ParseGraphicsSetting(s,"post_gamma","0") && s.gamma==.5f);
  CHECK(ParseGraphicsSetting(s,"post_exposure","-100") && s.exposure==-3);
  CHECK(!ParseGraphicsSetting(s,"post_contrast","nan") && s.contrast==1);
  CHECK(!ParseGraphicsSetting(s,"post_contrast","inf"));
  CHECK(!ParseGraphicsSetting(s,"post_contrast","1.2junk"));
  CHECK(!ParseGraphicsSetting(s,"post_contrast",""));
  CHECK(!ParseGraphicsSetting(s,"texture_anisotropy","3"));
  CHECK(ParseGraphicsSetting(s,"msaa_samples","4") && s.msaa==4);
  CHECK(ParseGraphicsSetting(s,"frame_interpolation","1") && s.interpolation==1);
  CHECK(!ParseGraphicsSetting(s,"frame_interpolation","2"));
  CHECK(!ParseGraphicsSetting(s,"unknown","1"));
  // v359 performance HUD: master switch plus one bit per row.
  CHECK(s.hud==0 && s.hud_items==HudDefaultItems);
  CHECK(ParseGraphicsSetting(s,"hud","true") && s.hud==1);
  CHECK(ParseGraphicsSetting(s,"hud_gpu","0") && (s.hud_items & HudGpu)==0);
  CHECK((s.hud_items & HudFps)!=0);
  CHECK(ParseGraphicsSetting(s,"hud_gpu","on") && (s.hud_items & HudGpu)!=0);
  CHECK(!ParseGraphicsSetting(s,"hud_cpu","2") && (s.hud_items & HudCpu)!=0);
  CHECK(GraphicsSettingsPreHudSize < GraphicsSettingsPreUnlockSize);
  CHECK(GraphicsSettingsPreUnlockSize < sizeof(GraphicsSettings));
  CHECK(s.unlock_predict==0 && ParseGraphicsSetting(s,"frame_unlock_predict","true") && s.unlock_predict==1);
  std::cout << "graphics settings validation passed\n";
}
