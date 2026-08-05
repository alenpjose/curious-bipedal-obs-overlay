#include <obs-frontend-api.h>
#include <obs-module.h>
#include <util/platform.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <mutex>
#include <string>
#include <vector>

OBS_DECLARE_MODULE()
OBS_MODULE_AUTHOR("Curious Bipedal")
OBS_MODULE_USE_DEFAULT_LOCALE("curious-bipedal-obs-overlay", "en-US")

namespace {

constexpr const char *SOURCE_ID = "curious_bipedal_session_overlay";
constexpr uint64_t NS_PER_SECOND = 1000000000ULL;

enum class Layout : int { Landscape = 0, Vertical = 1 };
enum class TimerBinding : int { MainObs = 0, AitumVertical = 1, Manual = 2 };

struct Overlay {
	obs_source_t *source = nullptr;
	obs_source_t *session_panel = nullptr;
	obs_source_t *telemetry_panel = nullptr;
	obs_source_t *accent = nullptr;
	obs_source_t *logo = nullptr;
	obs_source_t *logo_opacity_filter = nullptr;
	obs_source_t *label_text = nullptr;
	obs_source_t *title_text = nullptr;
	obs_source_t *log_text = nullptr;
	obs_source_t *time_text = nullptr;
	obs_source_t *date_text = nullptr;
	obs_source_t *timer_text = nullptr;

	std::mutex mutex;
	uint32_t width = 2560;
	uint32_t height = 1440;
	Layout layout = Layout::Landscape;
	TimerBinding binding = TimerBinding::MainObs;
	std::string session_title = "Gameplay Session";
	std::string log_number = "001";
	std::string manual_date = "2026-08-04";
	std::string manual_time = "20:00";
	std::string aitum_output_name = "YouTube";
	bool use_system_clock = true;
	bool show_date = true;
	bool show_time = true;
	bool show_timer = true;
	bool show_logo = true;
	int opacity = 82;
	int scale_percent = 100;
	int edge_margin = 0;
	uint32_t accent_color = 0xFF3D9BEF;

	bool timer_running = false;
	uint64_t timer_started_ns = 0;
	uint64_t elapsed_before_start_ns = 0;
	uint64_t last_clock_update_ns = 0;
	uint64_t last_aitum_retry_ns = 0;

	obs_output_t *vertical_output = nullptr;
	obs_hotkey_id start_pause_hotkey = OBS_INVALID_HOTKEY_ID;
	obs_hotkey_id reset_hotkey = OBS_INVALID_HOTKEY_ID;
};

std::mutex g_instances_mutex;
std::vector<Overlay *> g_instances;

uint32_t rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
	return (static_cast<uint32_t>(a) << 24U) | (static_cast<uint32_t>(b) << 16U) |
	       (static_cast<uint32_t>(g) << 8U) | static_cast<uint32_t>(r);
}

uint64_t elapsed_ns_locked(const Overlay *overlay, uint64_t now)
{
	return overlay->elapsed_before_start_ns +
	       (overlay->timer_running && now >= overlay->timer_started_ns ? now - overlay->timer_started_ns : 0);
}

void timer_start(Overlay *overlay)
{
	std::lock_guard<std::mutex> lock(overlay->mutex);
	if (!overlay->timer_running) {
		overlay->timer_started_ns = os_gettime_ns();
		overlay->timer_running = true;
	}
}

void timer_pause(Overlay *overlay)
{
	std::lock_guard<std::mutex> lock(overlay->mutex);
	if (overlay->timer_running) {
		const uint64_t now = os_gettime_ns();
		overlay->elapsed_before_start_ns = elapsed_ns_locked(overlay, now);
		overlay->timer_running = false;
	}
}

void timer_toggle(Overlay *overlay)
{
	bool running;
	{
		std::lock_guard<std::mutex> lock(overlay->mutex);
		running = overlay->timer_running;
	}
	if (running)
		timer_pause(overlay);
	else
		timer_start(overlay);
}

void timer_reset(Overlay *overlay)
{
	std::lock_guard<std::mutex> lock(overlay->mutex);
	overlay->elapsed_before_start_ns = 0;
	overlay->timer_started_ns = os_gettime_ns();
}

void update_color_source(obs_source_t *source, uint32_t color, int width, int height)
{
	if (!source)
		return;
	obs_data_t *settings = obs_data_create();
	obs_data_set_int(settings, "color", color);
	obs_data_set_int(settings, "width", width);
	obs_data_set_int(settings, "height", height);
	obs_source_update(source, settings);
	obs_data_release(settings);
}

void update_text_source(obs_source_t *source, const std::string &text, int font_size, bool bold, int opacity,
			uint32_t color, int extents_width, int extents_height, const char *align = "left")
{
	if (!source)
		return;
	obs_data_t *settings = obs_data_create();
	obs_data_t *font = obs_data_create();
	obs_data_set_string(font, "face", "Segoe UI");
	obs_data_set_string(font, "style", bold ? "Bold" : "Regular");
	obs_data_set_int(font, "size", font_size);
	obs_data_set_int(font, "flags", 0);
	obs_data_set_obj(settings, "font", font);
	obs_data_set_string(settings, "text", text.c_str());
	obs_data_set_int(settings, "color", color);
	obs_data_set_int(settings, "opacity", opacity);
	obs_data_set_bool(settings, "gradient", false);
	obs_data_set_bool(settings, "outline", false);
	obs_data_set_string(settings, "align", align);
	obs_data_set_string(settings, "valign", "center");
	obs_data_set_bool(settings, "extents", true);
	obs_data_set_bool(settings, "extents_wrap", false);
	obs_data_set_int(settings, "extents_cx", extents_width);
	obs_data_set_int(settings, "extents_cy", extents_height);
	obs_source_update(source, settings);
	obs_data_release(font);
	obs_data_release(settings);
}

obs_source_t *make_private_source(const char *id, const char *name)
{
	obs_data_t *settings = obs_data_create();
	obs_source_t *source = obs_source_create_private(id, name, settings);
	obs_data_release(settings);
	if (!source)
		blog(LOG_WARNING, "[Curious Bipedal] Required OBS source '%s' is unavailable", id);
	return source;
}

void add_active_child(Overlay *overlay, obs_source_t *child)
{
	if (child && !obs_source_add_active_child(overlay->source, child))
		blog(LOG_WARNING, "[Curious Bipedal] Could not attach child source '%s'", obs_source_get_name(child));
}

void remove_active_child(Overlay *overlay, obs_source_t *child)
{
	if (child)
		obs_source_remove_active_child(overlay->source, child);
}

void render_source(obs_source_t *source, float x, float y, float scale_x = 1.0f, float scale_y = 1.0f)
{
	if (!source)
		return;
	gs_matrix_push();
	gs_matrix_translate3f(x, y, 0.0f);
	gs_matrix_scale3f(scale_x, scale_y, 1.0f);
	obs_source_video_render(source);
	gs_matrix_pop();
}

void vertical_output_started(void *data, calldata_t *)
{
	timer_start(static_cast<Overlay *>(data));
}

void vertical_output_stopped(void *data, calldata_t *)
{
	timer_pause(static_cast<Overlay *>(data));
}

void disconnect_vertical_output(Overlay *overlay)
{
	if (!overlay->vertical_output)
		return;
	signal_handler_t *handler = obs_output_get_signal_handler(overlay->vertical_output);
	signal_handler_disconnect(handler, "start", vertical_output_started, overlay);
	signal_handler_disconnect(handler, "stop", vertical_output_stopped, overlay);
	obs_output_release(overlay->vertical_output);
	overlay->vertical_output = nullptr;
}

bool connect_vertical_output(Overlay *overlay)
{
	disconnect_vertical_output(overlay);
	if (overlay->binding != TimerBinding::AitumVertical || overlay->aitum_output_name.empty())
		return false;

	calldata_t call;
	calldata_init(&call);
	calldata_set_int(&call, "width", overlay->width);
	calldata_set_int(&call, "height", overlay->height);
	calldata_set_string(&call, "name", overlay->aitum_output_name.c_str());
	const bool called = proc_handler_call(obs_get_proc_handler(), "aitum_vertical_get_stream_output", &call);
	if (called)
		overlay->vertical_output = static_cast<obs_output_t *>(calldata_ptr(&call, "output"));
	calldata_free(&call);

	if (!overlay->vertical_output)
		return false;

	signal_handler_t *handler = obs_output_get_signal_handler(overlay->vertical_output);
	signal_handler_connect(handler, "start", vertical_output_started, overlay);
	signal_handler_connect(handler, "stop", vertical_output_stopped, overlay);
	if (obs_output_active(overlay->vertical_output))
		timer_start(overlay);
	return true;
}

void refresh_static_children(Overlay *overlay)
{
	const uint8_t alpha = static_cast<uint8_t>((overlay->opacity * 255) / 100);
	update_color_source(overlay->session_panel, rgba(14, 22, 31, alpha), 820, 170);
	update_color_source(overlay->telemetry_panel, rgba(14, 22, 31, alpha), 460, 148);
	update_color_source(overlay->accent, (overlay->accent_color & 0x00FFFFFFU) | (static_cast<uint32_t>(alpha) << 24U),
			    8, 170);

	const std::string log_line = "LOG " + overlay->log_number;
	update_text_source(overlay->label_text, "CURIOUS BIPEDAL  /  FIELD SESSION", 17, true, overlay->opacity,
			   rgba(239, 155, 61, 255), 580, 30);
	update_text_source(overlay->title_text, overlay->session_title, 38, true, overlay->opacity,
			   rgba(244, 241, 233, 255), 580, 58);
	update_text_source(overlay->log_text, log_line, 18, false, overlay->opacity, rgba(174, 183, 191, 255), 580, 34);

	if (overlay->logo_opacity_filter) {
		obs_data_t *filter_settings = obs_data_create();
		obs_data_set_double(filter_settings, "opacity", static_cast<double>(overlay->opacity) / 100.0);
		obs_source_update(overlay->logo_opacity_filter, filter_settings);
		obs_data_release(filter_settings);
	}
}

void update_clock_children(Overlay *overlay, uint64_t now)
{
	std::string time_string = overlay->manual_time;
	std::string date_string = overlay->manual_date;
	if (overlay->use_system_clock) {
		std::time_t raw_time = std::time(nullptr);
		std::tm local_time{};
#ifdef _WIN32
		localtime_s(&local_time, &raw_time);
#else
		localtime_r(&raw_time, &local_time);
#endif
		char time_buffer[32];
		char date_buffer[32];
		std::strftime(time_buffer, sizeof(time_buffer), "%H:%M", &local_time);
		std::strftime(date_buffer, sizeof(date_buffer), "%Y-%m-%d", &local_time);
		time_string = time_buffer;
		date_string = date_buffer;
	}

	const uint64_t total_seconds = elapsed_ns_locked(overlay, now) / NS_PER_SECOND;
	const uint64_t hours = total_seconds / 3600;
	const uint64_t minutes = (total_seconds / 60) % 60;
	const uint64_t seconds = total_seconds % 60;
	char timer_buffer[32];
	std::snprintf(timer_buffer, sizeof(timer_buffer), "%02llu:%02llu:%02llu",
		      static_cast<unsigned long long>(hours), static_cast<unsigned long long>(minutes),
		      static_cast<unsigned long long>(seconds));

	update_text_source(overlay->time_text, overlay->show_time ? time_string : "", 42, true, overlay->opacity,
			   rgba(244, 241, 233, 255), 250, 58, "right");
	update_text_source(overlay->date_text, overlay->show_date ? date_string : "", 17, false, overlay->opacity,
			   rgba(174, 183, 191, 255), 250, 30, "right");
	update_text_source(overlay->timer_text, overlay->show_timer ? std::string("ELAPSED  ") + timer_buffer : "", 18,
			   true, overlay->opacity, rgba(239, 155, 61, 255), 380, 36, "right");
}

void overlay_update(void *data, obs_data_t *settings)
{
	auto *overlay = static_cast<Overlay *>(data);
	const auto previous_binding = overlay->binding;
	const std::string previous_output = overlay->aitum_output_name;
	const uint32_t previous_width = overlay->width;
	const uint32_t previous_height = overlay->height;

	{
		std::lock_guard<std::mutex> lock(overlay->mutex);
		overlay->layout = static_cast<Layout>(obs_data_get_int(settings, "layout"));
		overlay->width = static_cast<uint32_t>(std::max<int64_t>(320, obs_data_get_int(settings, "canvas_width")));
		overlay->height = static_cast<uint32_t>(std::max<int64_t>(320, obs_data_get_int(settings, "canvas_height")));
		overlay->session_title = obs_data_get_string(settings, "session_title");
		overlay->log_number = obs_data_get_string(settings, "log_number");
		overlay->use_system_clock = obs_data_get_bool(settings, "use_system_clock");
		overlay->manual_date = obs_data_get_string(settings, "manual_date");
		overlay->manual_time = obs_data_get_string(settings, "manual_time");
		overlay->show_date = obs_data_get_bool(settings, "show_date");
		overlay->show_time = obs_data_get_bool(settings, "show_time");
		overlay->show_timer = obs_data_get_bool(settings, "show_timer");
		overlay->show_logo = obs_data_get_bool(settings, "show_logo");
		overlay->opacity = static_cast<int>(obs_data_get_int(settings, "opacity"));
		overlay->scale_percent = static_cast<int>(obs_data_get_int(settings, "scale_percent"));
		overlay->edge_margin = static_cast<int>(obs_data_get_int(settings, "edge_margin"));
		overlay->accent_color = static_cast<uint32_t>(obs_data_get_int(settings, "accent_color"));
		overlay->binding = static_cast<TimerBinding>(obs_data_get_int(settings, "timer_binding"));
		overlay->aitum_output_name = obs_data_get_string(settings, "aitum_output_name");
		refresh_static_children(overlay);
		update_clock_children(overlay, os_gettime_ns());
	}

	if (previous_binding != overlay->binding || previous_output != overlay->aitum_output_name ||
	    previous_width != overlay->width || previous_height != overlay->height) {
		if (overlay->binding == TimerBinding::AitumVertical)
			connect_vertical_output(overlay);
		else
			disconnect_vertical_output(overlay);
	}
	if (overlay->binding == TimerBinding::MainObs && obs_frontend_streaming_active())
		timer_start(overlay);
}

void overlay_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, "layout", static_cast<int>(Layout::Landscape));
	obs_data_set_default_int(settings, "canvas_width", 2560);
	obs_data_set_default_int(settings, "canvas_height", 1440);
	obs_data_set_default_string(settings, "session_title", "Gameplay Session");
	obs_data_set_default_string(settings, "log_number", "001");
	obs_data_set_default_bool(settings, "use_system_clock", true);
	obs_data_set_default_string(settings, "manual_date", "2026-08-04");
	obs_data_set_default_string(settings, "manual_time", "20:00");
	obs_data_set_default_bool(settings, "show_date", true);
	obs_data_set_default_bool(settings, "show_time", true);
	obs_data_set_default_bool(settings, "show_timer", true);
	obs_data_set_default_bool(settings, "show_logo", true);
	obs_data_set_default_int(settings, "opacity", 82);
	obs_data_set_default_int(settings, "scale_percent", 100);
	obs_data_set_default_int(settings, "edge_margin", 0);
	obs_data_set_default_int(settings, "accent_color", rgba(239, 155, 61, 255));
	obs_data_set_default_int(settings, "timer_binding", static_cast<int>(TimerBinding::MainObs));
	obs_data_set_default_string(settings, "aitum_output_name", "YouTube");
}

bool layout_modified(obs_properties_t *properties, obs_property_t *, obs_data_t *settings)
{
	const auto layout = static_cast<Layout>(obs_data_get_int(settings, "layout"));
	if (layout == Layout::Vertical) {
		obs_data_set_int(settings, "canvas_width", 1440);
		obs_data_set_int(settings, "canvas_height", 2560);
		obs_data_set_int(settings, "timer_binding", static_cast<int>(TimerBinding::AitumVertical));
	} else {
		obs_data_set_int(settings, "canvas_width", 2560);
		obs_data_set_int(settings, "canvas_height", 1440);
		obs_data_set_int(settings, "timer_binding", static_cast<int>(TimerBinding::MainObs));
	}
	if (obs_property_t *property = obs_properties_get(properties, "aitum_output_name"))
		obs_property_set_visible(property, layout == Layout::Vertical);
	return true;
}

bool timer_binding_modified(obs_properties_t *properties, obs_property_t *, obs_data_t *settings)
{
	const auto binding = static_cast<TimerBinding>(obs_data_get_int(settings, "timer_binding"));
	if (obs_property_t *property = obs_properties_get(properties, "aitum_output_name"))
		obs_property_set_visible(property, binding == TimerBinding::AitumVertical);
	return true;
}

bool system_clock_modified(obs_properties_t *properties, obs_property_t *, obs_data_t *settings)
{
	const bool automatic = obs_data_get_bool(settings, "use_system_clock");
	if (obs_property_t *property = obs_properties_get(properties, "manual_date"))
		obs_property_set_enabled(property, !automatic);
	if (obs_property_t *property = obs_properties_get(properties, "manual_time"))
		obs_property_set_enabled(property, !automatic);
	return true;
}

bool button_start_pause(obs_properties_t *, obs_property_t *, void *data)
{
	if (data)
		timer_toggle(static_cast<Overlay *>(data));
	return true;
}

bool button_reset(obs_properties_t *, obs_property_t *, void *data)
{
	if (data)
		timer_reset(static_cast<Overlay *>(data));
	return true;
}

bool button_reconnect(obs_properties_t *, obs_property_t *, void *data)
{
	return data ? connect_vertical_output(static_cast<Overlay *>(data)) : false;
}

obs_properties_t *overlay_properties(void *)
{
	obs_properties_t *properties = obs_properties_create();
	obs_properties_add_text(properties, "setup_note", obs_module_text("SetupNote"), OBS_TEXT_INFO);

	obs_properties_t *identity = obs_properties_create();
	obs_properties_add_text(identity, "session_title", obs_module_text("SessionTitle"), OBS_TEXT_DEFAULT);
	obs_properties_add_text(identity, "log_number", obs_module_text("LogNumber"), OBS_TEXT_DEFAULT);
	obs_properties_add_group(properties, "identity_group", obs_module_text("IdentityGroup"), OBS_GROUP_NORMAL, identity);

	obs_properties_t *layout_group = obs_properties_create();
	obs_property_t *layout = obs_properties_add_list(layout_group, "layout", obs_module_text("Layout"),
						       OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(layout, obs_module_text("Landscape"), static_cast<int>(Layout::Landscape));
	obs_property_list_add_int(layout, obs_module_text("Vertical"), static_cast<int>(Layout::Vertical));
	obs_property_set_modified_callback(layout, layout_modified);
	obs_properties_add_int(layout_group, "canvas_width", obs_module_text("CanvasWidth"), 320, 7680, 2);
	obs_properties_add_int(layout_group, "canvas_height", obs_module_text("CanvasHeight"), 320, 7680, 2);
	obs_properties_add_int_slider(layout_group, "scale_percent", obs_module_text("OverlayScale"), 50, 180, 1);
	obs_properties_add_int_slider(layout_group, "edge_margin", obs_module_text("EdgeMargin"), 0, 200, 1);
	obs_properties_add_group(properties, "layout_group", obs_module_text("LayoutGroup"), OBS_GROUP_NORMAL,
				 layout_group);

	obs_properties_t *display = obs_properties_create();
	obs_properties_add_bool(display, "show_logo", obs_module_text("ShowLogo"));
	obs_properties_add_bool(display, "show_time", obs_module_text("ShowTime"));
	obs_properties_add_bool(display, "show_date", obs_module_text("ShowDate"));
	obs_properties_add_bool(display, "show_timer", obs_module_text("ShowTimer"));
	obs_properties_add_int_slider(display, "opacity", obs_module_text("Opacity"), 10, 100, 1);
	obs_properties_add_color(display, "accent_color", obs_module_text("AccentColor"));
	obs_properties_add_group(properties, "display_group", obs_module_text("DisplayGroup"), OBS_GROUP_NORMAL, display);

	obs_properties_t *clock = obs_properties_create();
	obs_property_t *system_clock = obs_properties_add_bool(clock, "use_system_clock", obs_module_text("UseSystemClock"));
	obs_property_set_modified_callback(system_clock, system_clock_modified);
	obs_properties_add_text(clock, "manual_date", obs_module_text("ManualDate"), OBS_TEXT_DEFAULT);
	obs_properties_add_text(clock, "manual_time", obs_module_text("ManualTime"), OBS_TEXT_DEFAULT);
	obs_property_t *binding = obs_properties_add_list(clock, "timer_binding", obs_module_text("TimerBinding"),
							OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(binding, obs_module_text("MainObsBinding"), static_cast<int>(TimerBinding::MainObs));
	obs_property_list_add_int(binding, obs_module_text("AitumBinding"), static_cast<int>(TimerBinding::AitumVertical));
	obs_property_list_add_int(binding, obs_module_text("ManualBinding"), static_cast<int>(TimerBinding::Manual));
	obs_property_set_modified_callback(binding, timer_binding_modified);
	obs_properties_add_text(clock, "aitum_output_name", obs_module_text("AitumOutputName"), OBS_TEXT_DEFAULT);
	obs_properties_add_button(clock, "reconnect_aitum", obs_module_text("ReconnectAitum"), button_reconnect);
	obs_properties_add_button(clock, "start_pause", obs_module_text("StartPause"), button_start_pause);
	obs_properties_add_button(clock, "reset", obs_module_text("ResetTimer"), button_reset);
	obs_properties_add_group(properties, "clock_group", obs_module_text("ClockGroup"), OBS_GROUP_NORMAL, clock);
	return properties;
}

void hotkey_start_pause(void *data, obs_hotkey_id, obs_hotkey_t *, bool pressed)
{
	if (pressed)
		timer_toggle(static_cast<Overlay *>(data));
}

void hotkey_reset(void *data, obs_hotkey_id, obs_hotkey_t *, bool pressed)
{
	if (pressed)
		timer_reset(static_cast<Overlay *>(data));
}

void *overlay_create(obs_data_t *settings, obs_source_t *source)
{
	auto *overlay = new Overlay;
	overlay->source = source;
	overlay->session_panel = make_private_source("color_source_v3", "Curious Bipedal session panel");
	overlay->telemetry_panel = make_private_source("color_source_v3", "Curious Bipedal telemetry panel");
	overlay->accent = make_private_source("color_source_v3", "Curious Bipedal accent");
	overlay->label_text = make_private_source("text_gdiplus_v3", "Curious Bipedal label");
	overlay->title_text = make_private_source("text_gdiplus_v3", "Curious Bipedal title");
	overlay->log_text = make_private_source("text_gdiplus_v3", "Curious Bipedal log");
	overlay->time_text = make_private_source("text_gdiplus_v3", "Curious Bipedal time");
	overlay->date_text = make_private_source("text_gdiplus_v3", "Curious Bipedal date");
	overlay->timer_text = make_private_source("text_gdiplus_v3", "Curious Bipedal timer");

	char *logo_path = obs_module_file("assets/curious-bipedal-primary-glyph-safe.png");
	if (logo_path) {
		obs_data_t *logo_settings = obs_data_create();
		obs_data_set_string(logo_settings, "file", logo_path);
		obs_data_set_bool(logo_settings, "unload", false);
		overlay->logo = obs_source_create_private("image_source", "Curious Bipedal approved logo", logo_settings);
		obs_data_release(logo_settings);
		bfree(logo_path);
	}
	if (overlay->logo) {
		obs_data_t *filter_settings = obs_data_create();
		obs_data_set_double(filter_settings, "opacity", 0.82);
		overlay->logo_opacity_filter =
			obs_source_create_private("color_filter_v2", "Curious Bipedal shared opacity", filter_settings);
		obs_data_release(filter_settings);
		if (overlay->logo_opacity_filter)
			obs_source_filter_add(overlay->logo, overlay->logo_opacity_filter);
	}
	add_active_child(overlay, overlay->session_panel);
	add_active_child(overlay, overlay->telemetry_panel);
	add_active_child(overlay, overlay->accent);
	add_active_child(overlay, overlay->logo);
	add_active_child(overlay, overlay->label_text);
	add_active_child(overlay, overlay->title_text);
	add_active_child(overlay, overlay->log_text);
	add_active_child(overlay, overlay->time_text);
	add_active_child(overlay, overlay->date_text);
	add_active_child(overlay, overlay->timer_text);

	overlay->start_pause_hotkey = obs_hotkey_register_source(source, "curious_bipedal.timer.start_pause",
								  obs_module_text("HotkeyStartPause"), hotkey_start_pause, overlay);
	overlay->reset_hotkey = obs_hotkey_register_source(source, "curious_bipedal.timer.reset",
							obs_module_text("HotkeyReset"), hotkey_reset, overlay);
	{
		std::lock_guard<std::mutex> lock(g_instances_mutex);
		g_instances.push_back(overlay);
	}
	overlay_update(overlay, settings);
	return overlay;
}

void release_source(obs_source_t *&source)
{
	if (source) {
		obs_source_release(source);
		source = nullptr;
	}
}

void overlay_destroy(void *data)
{
	auto *overlay = static_cast<Overlay *>(data);
	{
		std::lock_guard<std::mutex> lock(g_instances_mutex);
		g_instances.erase(std::remove(g_instances.begin(), g_instances.end(), overlay), g_instances.end());
	}
	disconnect_vertical_output(overlay);
	remove_active_child(overlay, overlay->session_panel);
	remove_active_child(overlay, overlay->telemetry_panel);
	remove_active_child(overlay, overlay->accent);
	remove_active_child(overlay, overlay->logo);
	remove_active_child(overlay, overlay->label_text);
	remove_active_child(overlay, overlay->title_text);
	remove_active_child(overlay, overlay->log_text);
	remove_active_child(overlay, overlay->time_text);
	remove_active_child(overlay, overlay->date_text);
	remove_active_child(overlay, overlay->timer_text);
	if (overlay->logo && overlay->logo_opacity_filter)
		obs_source_filter_remove(overlay->logo, overlay->logo_opacity_filter);
	release_source(overlay->logo_opacity_filter);
	release_source(overlay->logo);
	release_source(overlay->session_panel);
	release_source(overlay->telemetry_panel);
	release_source(overlay->accent);
	release_source(overlay->label_text);
	release_source(overlay->title_text);
	release_source(overlay->log_text);
	release_source(overlay->time_text);
	release_source(overlay->date_text);
	release_source(overlay->timer_text);
	delete overlay;
}

uint32_t overlay_width(void *data) { return static_cast<Overlay *>(data)->width; }
uint32_t overlay_height(void *data) { return static_cast<Overlay *>(data)->height; }

void overlay_tick(void *data, float)
{
	auto *overlay = static_cast<Overlay *>(data);
	const uint64_t now = os_gettime_ns();
	{
		std::lock_guard<std::mutex> lock(overlay->mutex);
		if (now - overlay->last_clock_update_ns >= NS_PER_SECOND / 4) {
			update_clock_children(overlay, now);
			overlay->last_clock_update_ns = now;
		}
	}
	if (overlay->binding == TimerBinding::AitumVertical && !overlay->vertical_output &&
	    now - overlay->last_aitum_retry_ns >= 3 * NS_PER_SECOND) {
		overlay->last_aitum_retry_ns = now;
		connect_vertical_output(overlay);
	}
}

void overlay_render(void *data, gs_effect_t *)
{
	auto *overlay = static_cast<Overlay *>(data);
	std::lock_guard<std::mutex> lock(overlay->mutex);
	const float unit = (overlay->layout == Layout::Vertical)
				   ? std::min(static_cast<float>(overlay->width) / 1440.0f,
					      static_cast<float>(overlay->height) / 2560.0f)
				   : std::min(static_cast<float>(overlay->width) / 2560.0f,
					      static_cast<float>(overlay->height) / 1440.0f);
	const float scale = unit * static_cast<float>(overlay->scale_percent) / 100.0f;
	const float margin = static_cast<float>(overlay->edge_margin) * unit;
	const float session_h = 170.0f * scale;
	const float telemetry_w = 460.0f * scale;
	const float session_x = margin;
	const float session_y = static_cast<float>(overlay->height) - margin - session_h;
	const float telemetry_x = static_cast<float>(overlay->width) - margin - telemetry_w;
	const float telemetry_y = margin;

	render_source(overlay->session_panel, session_x, session_y, scale, scale);
	render_source(overlay->accent, session_x, session_y, scale, scale);
	if (overlay->show_logo && overlay->logo) {
		const float logo_native = static_cast<float>(std::max(obs_source_get_width(overlay->logo), 1U));
		const float logo_size = 145.0f * scale;
		render_source(overlay->logo, session_x + 18.0f * scale, session_y + 12.5f * scale,
			      logo_size / logo_native, logo_size / logo_native);
	}
	const float text_x = session_x + (overlay->show_logo ? 176.0f : 30.0f) * scale;
	render_source(overlay->label_text, text_x, session_y + 18.0f * scale, scale, scale);
	render_source(overlay->title_text, text_x, session_y + 52.0f * scale, scale, scale);
	render_source(overlay->log_text, text_x, session_y + 119.0f * scale, scale, scale);

	if (overlay->show_date || overlay->show_time || overlay->show_timer) {
		render_source(overlay->telemetry_panel, telemetry_x, telemetry_y, scale, scale);
		render_source(overlay->time_text, telemetry_x + 178.0f * scale, telemetry_y + 13.0f * scale, scale, scale);
		render_source(overlay->date_text, telemetry_x + 178.0f * scale, telemetry_y + 69.0f * scale, scale, scale);
		render_source(overlay->timer_text, telemetry_x + 48.0f * scale, telemetry_y + 103.0f * scale, scale, scale);
	}
}

const char *overlay_name(void *) { return obs_module_text("SourceName"); }

void frontend_event(enum obs_frontend_event event, void *)
{
	if (event != OBS_FRONTEND_EVENT_STREAMING_STARTED && event != OBS_FRONTEND_EVENT_STREAMING_STOPPED)
		return;
	std::lock_guard<std::mutex> lock(g_instances_mutex);
	for (Overlay *overlay : g_instances) {
		if (overlay->binding != TimerBinding::MainObs)
			continue;
		if (event == OBS_FRONTEND_EVENT_STREAMING_STARTED)
			timer_start(overlay);
		else
			timer_pause(overlay);
	}
}

obs_source_info overlay_info = {};

} // namespace

bool obs_module_load(void)
{
	overlay_info.id = SOURCE_ID;
	overlay_info.type = OBS_SOURCE_TYPE_INPUT;
	overlay_info.output_flags =
		OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW | OBS_SOURCE_COMPOSITE | OBS_SOURCE_SRGB;
	overlay_info.get_name = overlay_name;
	overlay_info.create = overlay_create;
	overlay_info.destroy = overlay_destroy;
	overlay_info.get_width = overlay_width;
	overlay_info.get_height = overlay_height;
	overlay_info.get_defaults = overlay_defaults;
	overlay_info.get_properties = overlay_properties;
	overlay_info.update = overlay_update;
	overlay_info.video_tick = overlay_tick;
	overlay_info.video_render = overlay_render;
	overlay_info.icon_type = OBS_ICON_TYPE_TEXT;
	obs_register_source(&overlay_info);
	obs_frontend_add_event_callback(frontend_event, nullptr);
	blog(LOG_INFO, "[Curious Bipedal] Native session overlay loaded");
	return true;
}

void obs_module_unload(void)
{
	obs_frontend_remove_event_callback(frontend_event, nullptr);
	blog(LOG_INFO, "[Curious Bipedal] Native session overlay unloaded");
}

const char *obs_module_description(void)
{
	return "Native Curious Bipedal session overlay with independent OBS and Aitum Vertical timer bindings.";
}
