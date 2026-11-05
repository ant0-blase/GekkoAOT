// SPDX-License-Identifier: GPL-3.0-or-later
#include "../../runtime/gx/graphics_settings.h"
#include "../../runtime/gx/graphics_presets.h"
#include "../../runtime/gx/projection_fov.h"
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
  CHECK(GraphicsSettingsPreEnhancedSize == 116 && GraphicsSettingsPreFovSize == 164 && GraphicsSettingsPreFragmentSize == 168 && GraphicsSettingsPreEnvironmentSize == 172 && GraphicsSettingsPreSunShadowsSize == 204 && GraphicsSettingsPreGiSize == 208 && GraphicsSettingsPreVsyncSize == 224 && GraphicsSettingsPreHdSize == 228 && GraphicsSettingsPreMaterialSize == 244 && GraphicsSettingsPreCompareSize == 260 && GraphicsSettingsPreGeometrySize == 264 && GraphicsSettingsPreTextureBackendSize == 268 && GraphicsSettingsPreDisplacementSize == 272 && GraphicsSettingsPreDofSize == 276 && GraphicsSettingsPreParticleLightsSize == 288 && GraphicsSettingsPreParticleRadiusSize == 296 && GraphicsSettingsPreSurfaceSize == 300 && sizeof(GraphicsSettings) == 308);
  CHECK(s.particle_lights==0 && ParseGraphicsSetting(s,"particle_lights","2") && s.particle_lights==2);
  CHECK(!ParseGraphicsSetting(s,"particle_lights","3"));
  CHECK(!ParseGraphicsSetting(s,"particle_light_strength","nan"));
  CHECK(s.particle_light_radius==2.f && ParseGraphicsSetting(s,"particle_light_radius","3.5") && s.particle_light_radius==3.5f);
  CHECK(ParseGraphicsSetting(s,"particle_light_radius","0") && s.particle_light_radius==.5f);
  CHECK(!ParseGraphicsSetting(s,"particle_light_radius","nan"));
  CHECK(s.dof_quality==0);
  CHECK(ParseGraphicsSetting(s,"dof_quality","2") && s.dof_quality==2);
  CHECK(!ParseGraphicsSetting(s,"dof_quality","1.5"));
  CHECK(!ParseGraphicsSetting(s,"dof_focus","nan"));
  CHECK(ParseGraphicsSetting(s,"dof_focus","30") && s.dof_focus==30);
  CHECK(s.texture_scale==1 && s.material_enhancement==0 && s.pom_quality==0);
  CHECK(ParseGraphicsSetting(s,"texture_scale","4") && s.texture_scale==4);
  CHECK(ParseGraphicsSetting(s,"texture_backend","1") && s.texture_backend==1);
  CHECK(ParseGraphicsSetting(s,"texture_backend","2") && s.texture_backend==2);
  CHECK(!ParseGraphicsSetting(s,"texture_backend","3"));
  CHECK(ParseGraphicsSetting(s,"displacement","1") && s.displacement==1);
  CHECK(!ParseGraphicsSetting(s,"displacement","2"));
  CHECK(ParseGraphicsSetting(s," displacement "," false ") && s.displacement==0);
  CHECK(ParseGraphicsSetting(s,"material_enhancement","on") && s.material_enhancement==1);
  CHECK(ParseGraphicsSetting(s,"material_compare","off") && s.material_compare==0);
  CHECK(!ParseGraphicsSetting(s,"texture_scale","3") && s.texture_scale==4);
  CHECK(ParseGraphicsSetting(s,"material_enhancement","1") && s.material_enhancement==1);
  CHECK(ParseGraphicsSetting(s,"pom_quality","2") && s.pom_quality==2);
  CHECK(!ParseGraphicsSetting(s,"material_normal_strength","nan"));
  CHECK(!ParseGraphicsSetting(s,"pom_quality","3"));
  CHECK(ParseGraphicsSetting(s,"material_compare","1") && s.material_compare==1);
  CHECK(!ParseGraphicsSetting(s,"material_compare","2"));
  CHECK(ParseGraphicsSetting(s,"geometry_refinement","2") && s.geometry_refinement==2);
  CHECK(!ParseGraphicsSetting(s,"geometry_refinement","3"));
  CHECK(!ParseGraphicsSetting(s,"hd_budget_mib","0"));
  CHECK(s.vsync==0 && ParseGraphicsSetting(s,"vsync","on") && s.vsync==1 && ParseGraphicsSetting(s,"vsync","0") && s.vsync==0);
  CHECK(!ParseGraphicsSetting(s,"vsync","2") && s.vsync==0);
  CHECK(ParseGraphicsSetting(s,"enhanced_renderer","true") && s.enhanced==1);
  CHECK(ParseGraphicsSetting(s,"ao_quality","3") && s.ao_quality==3);
  CHECK(!ParseGraphicsSetting(s,"ao_quality","1.5") && s.ao_quality==3);
  CHECK(ParseGraphicsSetting(s,"contact_shadows","on") && s.contact_shadows==1);
  CHECK(ParseGraphicsSetting(s,"shadow_distance","999999") && s.shadow_distance==10000);
  CHECK(!ParseGraphicsSetting(s,"ao_radius","nan") && s.ao_radius==1);
  CHECK(ParseGraphicsSetting(s,"light_x","-2") && s.light_x==-1);
  CHECK(s.fragment_lighting==1);
  CHECK(ParseGraphicsSetting(s,"fragment_lighting","off") && s.fragment_lighting==0);
  CHECK(!ParseGraphicsSetting(s,"fragment_lighting","2") && s.fragment_lighting==0);
  // Environment lighting: names or indices, presets load their sun values.
  CHECK(s.environment==0 && EnvironmentFromGraphics(s).mode==EnvironmentMode::Off);
  CHECK(ParseGraphicsSetting(s,"environment_lighting","auto") && s.environment==1);
  CHECK(ParseGraphicsSetting(s,"environment_lighting","custom") && s.environment==2);
  CHECK(!ParseGraphicsSetting(s,"environment_lighting","3") && s.environment==2);
  CHECK(ParseGraphicsSetting(s,"sun_preset","golden") && s.sun_preset==3 && s.sun_temperature==3300 && s.sun_elevation==12);
  CHECK(ParseGraphicsSetting(s,"sun_temperature","20000") && s.sun_temperature==12000);
  CHECK(!ParseGraphicsSetting(s,"sun_preset","noon") && !ParseGraphicsSetting(s,"sun_preset","4") && s.sun_preset==3);
  CHECK(ParseGraphicsSetting(s,"sun_elevation","-5") && s.sun_elevation==1);
  {const auto e=EnvironmentFromGraphics(s);
   CHECK(e.mode==EnvironmentMode::Custom && e.sun_temperature==12000 && e.sky_color[2]==ApplySunPreset({},SunPreset::GoldenHour).sky_color[2]);}
  CHECK(s.sun_shadows==0 && ParseGraphicsSetting(s,"sun_shadows","high") && s.sun_shadows==2 && SunShadowResolution(2)==2048);
  CHECK(!ParseGraphicsSetting(s,"sun_shadows","3") && s.sun_shadows==2 && SunShadowResolution(0)==0);
  CHECK(s.gi==0 && s.ssr==0 && ParseGraphicsSetting(s,"gi","on") && ParseGraphicsSetting(s,"ssr","true") && s.gi==1 && s.ssr==1);
  CHECK(ParseGraphicsSetting(s,"ssr_strength","9") && s.ssr_strength==4 && !ParseGraphicsSetting(s,"gi","2"));
  // Remastered presets: complete scene states, neutral unless Cinematic.
  for(const char* name:{"remastered","remastered_vivid","remastered_cinematic","remastered_performance"}){
    GraphicsSettings r;CHECK(ApplyGraphicsPreset(r,name) && SceneMatchesPreset(r,name));
    CHECK(r.enhanced==1 && r.environment==1 && r.sun_shadows>=1 && r.environment_strength<=.5f && r.sky_intensity>=.65f);
    CHECK(r.sun_temperature>=4700);
    CHECK(r.material_enhancement==1 && r.geometry_refinement==0 && r.texture_scale==1 && r.material_compare==0);
    CHECK(r.pom_quality==(std::string_view(name)=="remastered_performance"?0u:std::string_view(name)=="remastered_cinematic"?2u:1u));
  }
  {GraphicsSettings r;ApplyGraphicsPreset(r,"remastered_performance");CHECK(r.gi==0 && r.ssr==0 && r.sun_shadows==1);
   ApplyGraphicsPreset(r,"remastered_vivid");CHECK(r.gi==1 && r.saturation>1 && !SceneMatchesPreset(r,"remastered"));
   ApplyGraphicsPreset(r,"neutral");CHECK(r.environment==0 && r.sun_shadows==0 && r.gi==0 && r.material_enhancement==0 && r.geometry_refinement==0 && r.pom_quality==0);}
  CHECK(ParseGraphicsSetting(s,"fragment_lighting","on") && s.fragment_lighting==1);
  CHECK(s.fov_y==0);
  CHECK(ParseGraphicsSetting(s,"fov_y","110") && s.fov_y==110);
  CHECK(!ParseGraphicsSetting(s,"fov_y","10") && s.fov_y==110);
  CHECK(!ParseGraphicsSetting(s,"fov_y","nan"));
  CHECK(ParseGraphicsSetting(s,"fov_y","0") && s.fov_y==0);
  CHECK(std::abs(PerspectiveFovScale(1,90)-1)<1e-6f);
  CHECK(std::abs(PerspectiveFovScale(1,120)-.577350269f)<1e-6f);
  CHECK(PerspectiveFovScale(0,120)==1 && PerspectiveFovScale(1,0)==1);
  std::cout << "graphics settings validation passed\n";
}
