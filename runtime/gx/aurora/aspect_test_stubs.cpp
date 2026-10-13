extern "C" float gekkoaot_native_gx_widescreen_x_scale() noexcept { return 1.0f; }
extern "C" float gekkoaot_native_gx_widescreen_target_aspect() noexcept { return 4.0f / 3.0f; }
extern "C" float gekkoaot_native_gx_widescreen_projection_scale(float, float, float, float) noexcept { return 1.f; }
extern "C" bool gekkoaot_native_gx_screen_copy(int, int, float, float) noexcept { return false; }
extern "C" bool gekkoaot_native_gx_texture_viewport(float, float, float, float) noexcept { return false; }
extern "C" bool gekkoaot_native_gx_screen_fill() noexcept { return false; }
extern "C" float gekkoaot_native_gx_ui_clip_x(float x, float, float, float) noexcept { return x; }
extern "C" void gekkoaot_native_gx_widescreen_diag(int, float, float, float, float,
    int, int, int, int, bool, bool, float) noexcept {}
