#include "stereokit.h"

#include "assets.h"
#include "render.h"
#include "d3d.h"
#include "win32.h"
#include "openxr.h"
#include "input.h"
#include "shader_builtin.h"
#include "physics.h"
#include "system.h"
#include "text.h"

#include <thread> // sleep_for

using namespace std;

const char   *sk_app_name;
void        (*sk_app_update_func)(void);
sk_runtime_   sk_runtime  = sk_runtime_flatscreen;
bool          sk_runtime_fallback = false;
sk_settings_t sk_settings = {100,100,800,480};
bool32_t sk_focused = true;
bool32_t sk_run     = true;

bool sk_d3d_initialized = false;

float  sk_timevf = 0;
double sk_timev  = 0;
double sk_time_start    = 0;
double sk_timev_elapsed  = 0;
float  sk_timev_elapsedf = 0;
int64_t sk_timev_raw = 0;

tex2d_t    sk_default_tex;
tex2d_t    sk_default_tex_black;
tex2d_t    sk_default_tex_gray;
tex2d_t    sk_default_tex_flat;
tex2d_t    sk_default_tex_rough;
shader_t   sk_default_shader;
shader_t   sk_default_shader_pbr;
shader_t   sk_default_shader_font;
material_t sk_default_material;

bool sk_create_defaults();
void sk_destroy_defaults();
void sk_update_timer();

void sk_set_settings(sk_settings_t &settings) {
	if (sk_d3d_initialized) {
		log_write(log_error, "Settings need set before initialization! Please call this -before- sk_init.");
		return;
	}
	sk_settings = settings;
}

bool platform_init() {
	// Create a runtime
	bool result = sk_runtime == sk_runtime_mixedreality ?
		openxr_init(sk_app_name) :
	#ifndef SK_NO_FLATSCREEN
		win32_init(sk_app_name);
	#else
		false;
	#endif

	if (!result)
		log_writef(log_warning, "Couldn't create StereoKit in %s mode!", sk_runtime == sk_runtime_mixedreality ? "MixedReality" : "Flatscreen");

	// Try falling back to flatscreen, if we didn't just try it
	#ifndef SK_NO_FLATSCREEN
	if (!result && sk_runtime_fallback && sk_runtime != sk_runtime_flatscreen) {
		log_writef(log_info, "Runtime falling back to Flatscreen");
		sk_runtime = sk_runtime_flatscreen;
		result     = win32_init (sk_app_name);
	}
	#endif
	return result;
}
void platform_shutdown() {
	switch (sk_runtime) {
	#ifndef SK_NO_FLATSCREEN
	case sk_runtime_flatscreen:   win32_shutdown (); break;
	#endif
	case sk_runtime_mixedreality: openxr_shutdown(); break;
	}
}

void platform_begin_frame() {
	switch (sk_runtime) {
#ifndef SK_NO_FLATSCREEN
	case sk_runtime_flatscreen:   win32_step_begin (); break;
#endif
	case sk_runtime_mixedreality: openxr_step_begin(); break;
	}

	sk_update_timer();
}

void platform_end_frame() {
	switch (sk_runtime) {
#ifndef SK_NO_FLATSCREEN
	case sk_runtime_flatscreen:   win32_step_end (); break;
#endif
	case sk_runtime_mixedreality: openxr_step_end(); break;
	}
}
void platform_present() {
	switch (sk_runtime) {
#ifndef SK_NO_FLATSCREEN
	case sk_runtime_flatscreen:   win32_vsync(); break;
#endif
	case sk_runtime_mixedreality: break;
	}
}

void sk_app_update() {
	if (sk_app_update_func != nullptr)
		sk_app_update_func();
}

bool32_t sk_init(const char *app_name, sk_runtime_ runtime_preference, bool32_t fallback) {
	sk_runtime          = runtime_preference;
	sk_runtime_fallback = fallback;
	sk_app_name         = app_name;

	systems_add("Graphics", nullptr, 0, nullptr, 0, d3d_init, d3d_update, d3d_shutdown);

	const char *default_deps[] = {"Graphics"};
	systems_add("Defaults", default_deps, _countof(default_deps), nullptr, 0, sk_create_defaults, nullptr, sk_destroy_defaults);

	const char *platform_deps[] = {"Graphics", "Defaults"};
	systems_add("Platform", platform_deps, _countof(platform_deps), nullptr, 0, platform_init, nullptr, platform_shutdown);

	const char *physics_deps[] = {"Defaults"};
	const char *physics_update_deps[] = {"Input", "FrameBegin"};
	systems_add("Physics",  
		physics_deps,        _countof(physics_deps), 
		physics_update_deps, _countof(physics_update_deps), 
		physics_init, physics_update, physics_shutdown);

	const char *renderer_deps[] = {"Graphics", "Defaults"};
	const char *renderer_update_deps[] = {"Physics", "FrameBegin"};
	systems_add("Renderer",  
		renderer_deps,        _countof(renderer_deps), 
		renderer_update_deps, _countof(renderer_update_deps),
		render_initialize, render_update, render_shutdown);

	const char *input_deps[] = {"Platform", "Defaults"};
	const char *input_update_deps[] = {"FrameBegin"};
	systems_add("Input",  
		input_deps,        _countof(input_deps), 
		input_update_deps, _countof(input_update_deps), 
		input_init, input_update, input_shutdown);

	const char *text_deps[] = {"Defaults"};
	const char *text_update_deps[] = {"FrameBegin", "App"};
	systems_add("Text",  
		text_deps,        _countof(text_deps), 
		text_update_deps, _countof(text_update_deps), 
		nullptr, text_update, text_shutdown);

	const char *app_deps[] = {"Input", "Defaults", "FrameBegin", "Graphics", "Physics", "Renderer"};
	systems_add("App", nullptr, 0, app_deps, _countof(app_deps), nullptr, sk_app_update, nullptr);

	systems_add("FrameBegin", nullptr, 0, nullptr, 0, nullptr, platform_begin_frame, nullptr);
	const char *platform_end_deps[] = {"App", "Text"};
	systems_add("FrameEnd",   nullptr, 0, platform_end_deps, _countof(platform_end_deps), nullptr, platform_end_frame,   nullptr);
	const char *platform_present_deps[] = {"FrameEnd"};
	systems_add("FramePresent", nullptr, 0, platform_present_deps, _countof(platform_present_deps), nullptr, platform_present,   nullptr);

	return systems_initialize();
}

void sk_shutdown() {
	systems_shutdown();
}

void sk_update_timer() {
	FILETIME time;
	GetSystemTimePreciseAsFileTime(&time);
	sk_timev_raw = (int64_t)time.dwLowDateTime + ((int64_t)(time.dwHighDateTime) << 32LL);
	double time_curr = sk_timev_raw / 10000000.0;

	if (sk_time_start == 0)
		sk_time_start = time_curr;
	double new_time = time_curr - sk_time_start;
	sk_timev_elapsed  = new_time - sk_timev;
	sk_timev          = new_time;
	sk_timev_elapsedf = (float)sk_timev_elapsed;
	sk_timevf         = (float)sk_timev;
}

bool32_t sk_step(void (*app_update)(void)) {
	sk_app_update_func = app_update;

	systems_update();

	if (!sk_focused)
		this_thread::sleep_for(chrono::milliseconds(sk_focused ? 1 : 250));
	return sk_run;
}

float  sk_timef        (){ return sk_timevf; };
double sk_time         (){ return sk_timev; };
float  sk_time_elapsedf(){ return sk_timev_elapsedf; };
double sk_time_elapsed (){ return sk_timev_elapsed; };
sk_runtime_ sk_active_runtime() { return sk_runtime;  }

bool sk_create_defaults() {
	// Default white texture
	sk_default_tex = tex2d_create("default/tex2d");
	if (sk_default_tex == nullptr) {
		return false;
	}
	color32 tex_colors[2*2];
	memset(tex_colors, 255, sizeof(color32) * 2 * 2);
	tex2d_set_colors(sk_default_tex, 2, 2, tex_colors);

	// Default black texture, for use with shader defaults
	sk_default_tex_black = tex2d_create("default/tex2d_black");
	if (sk_default_tex_black == nullptr) {
		return false;
	}
	for (size_t i = 0; i < 2 * 2; i++) 
		tex_colors[i] = { 0,0,0,255 };
	tex2d_set_colors(sk_default_tex_black, 2, 2, tex_colors);

	// Default middle gray texture, for use with shader defaults
	sk_default_tex_gray = tex2d_create("default/tex2d_gray");
	if (sk_default_tex_gray == nullptr) {
		return false;
	}
	for (size_t i = 0; i < 2 * 2; i++) 
		tex_colors[i] = { 128,128,128,255 };
	tex2d_set_colors(sk_default_tex_gray, 2, 2, tex_colors);

	// Default normal map, for use with shader defaults
	sk_default_tex_flat = tex2d_create("default/tex2d_flat");
	if (sk_default_tex_flat == nullptr) {
		return false;
	}
	for (size_t i = 0; i < 2 * 2; i++) 
		tex_colors[i] = { 128,128,255,255 };
	tex2d_set_colors(sk_default_tex_flat, 2, 2, tex_colors);

	// Default metal/roughness map, for use with shader defaults
	sk_default_tex_rough = tex2d_create("default/tex2d_rough");
	if (sk_default_tex_rough == nullptr) {
		return false;
	}
	for (size_t i = 0; i < 2 * 2; i++) 
		tex_colors[i] = { 0,0,255,255 };
	tex2d_set_colors(sk_default_tex_rough, 2, 2, tex_colors);

	sk_default_shader = shader_create("default/shader", sk_shader_builtin_default);
	if (sk_default_shader == nullptr) {
		return false;
	}
	sk_default_shader_pbr = shader_create("default/shader_pbr", sk_shader_builtin_pbr);
	if (sk_default_shader_pbr == nullptr) {
		return false;
	}
	sk_default_shader_font = shader_create("default/shader_font", sk_shader_builtin_font);
	if (sk_default_shader_font == nullptr) {
		return false;
	}

	sk_default_material = material_create("default/material", sk_default_shader_pbr);
	if (sk_default_material == nullptr) {
		return false;
	}

	material_set_texture(sk_default_material, "diffuse", sk_default_tex);

	return true;
}

void sk_destroy_defaults() {
	material_release(sk_default_material);
	shader_release  (sk_default_shader_font);
	shader_release  (sk_default_shader_pbr);
	shader_release  (sk_default_shader);
	tex2d_release   (sk_default_tex);
	tex2d_release   (sk_default_tex_black);
	tex2d_release   (sk_default_tex_gray);
	tex2d_release   (sk_default_tex_flat);
	tex2d_release   (sk_default_tex_rough);
}