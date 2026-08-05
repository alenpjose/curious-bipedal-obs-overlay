#include <obs-frontend-api.h>
#include <obs-module.h>
#include <util/platform.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <memory>
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

struct Overlay;

struct OverlayLifetime {
	std::mutex mutex;
	Overlay *overlay = nullptr;
};

struct Overlay {
	obs_source_t *source = nullptr;
	obs_source_t *session_panel_left = nullptr;
	obs_source_t *session_panel = nullptr;
	obs_source_t *session_panel_right = nullptr;
	obs_source_t *session_panel_left_opacity_filter = nullptr;
	obs_source_t *session_panel_opacity_filter = nullptr;
	obs_source_t *session_panel_right_opacity_filter = nullptr;
	obs_source_t *telemetry_panel = nullptr;
	obs_source_t *telemetry_panel_opacity_filter = nullptr;
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
	std::mutex connection_mutex;
	std::mutex output_mutex;
	std::atomic_bool destroying{false};
	std::atomic_bool aitum_task_queued{false};
	std::shared_ptr<OverlayLifetime> lifetime = std::make_shared<OverlayLifetime>();
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
	int scale_percent = 80;
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

void timer_set_running(Overlay *overlay, bool running)
{
	if (running)
		timer_start(overlay);
	else
		timer_pause(overlay);
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
			uint32_t color, int extents_width, int extents_height, const char *align = "left",
			bool use_extents = true)
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
	obs_data_set_bool(settings, "extents", use_extents);
	obs_data_set_bool(settings, "extents_wrap", false);
	if (use_extents) {
		obs_data_set_int(settings, "extents_cx", extents_width);
		obs_data_set_int(settings, "extents_cy", extents_height);
	}
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

obs_source_t *make_private_image_source(const char *name, const char *relative_path)
{
	char *path = obs_module_file(relative_path);
	if (!path)
		return nullptr;
	obs_data_t *settings = obs_data_create();
	obs_data_set_string(settings, "file", path);
	obs_data_set_bool(settings, "unload", false);
	obs_source_t *source = obs_source_create_private("image_source", name, settings);
	obs_data_release(settings);
	bfree(path);
	if (!source)
		blog(LOG_WARNING, "[Curious Bipedal] Image asset '%s' is unavailable", relative_path);
	return source;
}

obs_source_t *attach_opacity_filter(obs_source_t *source, const char *name)
{
	if (!source)
		return nullptr;
	obs_data_t *settings = obs_data_create();
	obs_data_set_double(settings, "opacity", 0.82);
	obs_source_t *filter = obs_source_create_private("color_filter_v2", name, settings);
	obs_data_release(settings);
	if (filter)
		obs_source_filter_add(source, filter);
	return filter;
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
	auto *overlay = static_cast<Overlay *>(data);
	std::lock_guard<std::mutex> lock(overlay->mutex);
	if (!overlay->destroying.load(std::memory_order_acquire) && overlay->binding == TimerBinding::AitumVertical &&
	    !overlay->timer_running) {
		overlay->timer_started_ns = os_gettime_ns();
		overlay->timer_running = true;
	}
}

void vertical_output_stopped(void *data, calldata_t *)
{
	auto *overlay = static_cast<Overlay *>(data);
	std::lock_guard<std::mutex> lock(overlay->mutex);
	if (!overlay->destroying.load(std::memory_order_acquire) && overlay->binding == TimerBinding::AitumVertical &&
	    overlay->timer_running) {
		const uint64_t now = os_gettime_ns();
		overlay->elapsed_before_start_ns = elapsed_ns_locked(overlay, now);
		overlay->timer_running = false;
	}
}

void disconnect_vertical_output_locked(Overlay *overlay)
{
	obs_output_t *output = nullptr;
	{
		std::lock_guard<std::mutex> lock(overlay->output_mutex);
		output = overlay->vertical_output;
		overlay->vertical_output = nullptr;
	}
	if (!output)
		return;
	signal_handler_t *handler = obs_output_get_signal_handler(output);
	signal_handler_disconnect(handler, "start", vertical_output_started, overlay);
	signal_handler_disconnect(handler, "stop", vertical_output_stopped, overlay);
	obs_output_release(output);
}

void disconnect_vertical_output(Overlay *overlay)
{
	std::lock_guard<std::mutex> lock(overlay->connection_mutex);
	disconnect_vertical_output_locked(overlay);
}

bool connect_vertical_output(Overlay *overlay)
{
	std::lock_guard<std::mutex> connection_lock(overlay->connection_mutex);
	disconnect_vertical_output_locked(overlay);
	if (overlay->destroying.load(std::memory_order_acquire))
		return false;

	uint32_t width;
	uint32_t height;
	std::string output_name;
	{
		std::lock_guard<std::mutex> lock(overlay->mutex);
		if (overlay->binding != TimerBinding::AitumVertical || overlay->aitum_output_name.empty())
			return false;
		width = overlay->width;
		height = overlay->height;
		output_name = overlay->aitum_output_name;
	}

	calldata_t call;
	calldata_init(&call);
	calldata_set_int(&call, "width", width);
	calldata_set_int(&call, "height", height);
	calldata_set_string(&call, "name", output_name.c_str());
	const bool called = proc_handler_call(obs_get_proc_handler(), "aitum_vertical_get_stream_output", &call);
	obs_output_t *output = called ? static_cast<obs_output_t *>(calldata_ptr(&call, "output")) : nullptr;
	calldata_free(&call);

	if (!output) {
		timer_pause(overlay);
		return false;
	}

	const bool active = obs_output_active(output);
	signal_handler_t *handler = obs_output_get_signal_handler(output);
	signal_handler_connect(handler, "start", vertical_output_started, overlay);
	signal_handler_connect(handler, "stop", vertical_output_stopped, overlay);
	{
		std::lock_guard<std::mutex> lock(overlay->output_mutex);
		if (!overlay->destroying.load(std::memory_order_acquire)) {
			overlay->vertical_output = output;
			output = nullptr;
		}
	}
	if (output) {
		signal_handler_disconnect(handler, "start", vertical_output_started, overlay);
		signal_handler_disconnect(handler, "stop", vertical_output_stopped, overlay);
		obs_output_release(output);
		return false;
	}
	timer_set_running(overlay, active);
	return true;
}

struct AitumConnectTask {
	std::shared_ptr<OverlayLifetime> lifetime;
};

void connect_vertical_output_on_ui(void *data)
{
	std::unique_ptr<AitumConnectTask> task(static_cast<AitumConnectTask *>(data));
	std::lock_guard<std::mutex> lifetime_lock(task->lifetime->mutex);
	Overlay *overlay = task->lifetime->overlay;
	if (!overlay)
		return;
	connect_vertical_output(overlay);
	overlay->aitum_task_queued.store(false, std::memory_order_release);
}

bool request_vertical_output_connection(Overlay *overlay)
{
	if (overlay->destroying.load(std::memory_order_acquire))
		return false;
	if (obs_in_task_thread(OBS_TASK_UI))
		return connect_vertical_output(overlay);

	bool expected = false;
	if (!overlay->aitum_task_queued.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
		return true;
	auto *task = new AitumConnectTask{overlay->lifetime};
	obs_queue_task(OBS_TASK_UI, connect_vertical_output_on_ui, task, false);
	return true;
}

void refresh_static_children(Overlay *overlay)
{
	const uint8_t alpha = static_cast<uint8_t>((overlay->opacity * 255) / 100);
	update_color_source(overlay->accent, (overlay->accent_color & 0x00FFFFFFU) | (static_cast<uint32_t>(alpha) << 24U),
			    8, 92);

	const std::string log_line = "LOG " + overlay->log_number;
	update_text_source(overlay->label_text, "CURIOUS BIPEDAL  /  FIELD SESSION", 17, true, overlay->opacity,
			   rgba(239, 155, 61, 255), 0, 0, "left", false);
	update_text_source(overlay->title_text, overlay->session_title, 38, true, overlay->opacity,
			   rgba(244, 241, 233, 255), 0, 0, "left", false);
	update_text_source(overlay->log_text, log_line, 18, false, overlay->opacity,
			   rgba(174, 183, 191, 255), 0, 0, "left", false);

	if (overlay->logo_opacity_filter) {
		obs_data_t *filter_settings = obs_data_create();
		obs_data_set_double(filter_settings, "opacity", static_cast<double>(overlay->opacity) / 100.0);
		obs_source_update(overlay->logo_opacity_filter, filter_settings);
		obs_data_release(filter_settings);
	}
	for (obs_source_t *filter : {overlay->session_panel_left_opacity_filter,
				     overlay->session_panel_opacity_filter,
				     overlay->session_panel_right_opacity_filter,
				     overlay->telemetry_panel_opacity_filter}) {
		if (!filter)
			continue;
		obs_data_t *filter_settings = obs_data_create();
		obs_data_set_double(filter_settings, "opacity", static_cast<double>(overlay->opacity) / 100.0);
		obs_source_update(filter, filter_settings);
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

	update_text_source(overlay->timer_text, overlay->show_timer ? std::string("ELAPSED  ") + timer_buffer : "", 36,
			   true, overlay->opacity, rgba(239, 155, 61, 255), 404, 58, "center");
	update_text_source(overlay->time_text, overlay->show_time ? time_string : "", 19, true, overlay->opacity,
			   rgba(244, 241, 233, 255), 170, 36, "left");
	update_text_source(overlay->date_text, overlay->show_date ? datß¾4¶‰žËkºwµç}‰Í}¡½Ñ­•å}Ð€¨°‰½½°ÁÉ•ÍÍ•¤)ì(%¥˜€¡ÁÉ•ÍÍ•¤($%Ñ¥µ•É}Ñ½±”¡ÍÑ…Ñ¥}…ÍÐñ=Ù•É±…ä€¨ø¡‘…Ñ„¤¤ì)ô()Ù½¥¡½Ñ­•å}É•Í•Ð¡Ù½¥€©‘…Ñ„°½‰Í}¡½Ñ­•å}¥°½‰Í}¡½Ñ­•å}Ð€¨°‰½½°ÁÉ•ÍÍ•¤)ì(%¥˜€¡ÁÉ•ÍÍ•¤($%Ñ¥µ•É}É•Í•Ð¡ÍÑ…Ñ¥}…ÍÐñ=Ù•É±…ä€¨ø¡‘…Ñ„¤¤ì)ô()Ù½¥€©½Ù•É±…å}É•…Ñ”¡½‰Í}‘…Ñ…}Ð€©Í•ÑÑ¥¹Ì°½‰Í}Í½ÕÉ•}Ð€©Í½ÕÉ”¤)ì(%…ÕÑ¼€©½Ù•É±…ä€ô¹•Ü=Ù•É±…äì(%½Ù•É±…ä´ùÍ½ÕÉ”€ôÍ½ÕÉ”ì(%½Ù•É±…ä´ù±¥™•Ñ¥µ”´ù½Ù•É±…ä€ô½Ù•É±…äì(%½Ù•É±…ä´ùÍ•ÍÍ¥½¹}Á…¹•±}±•™Ð€ôµ…­•}ÁÉ¥Ù…Ñ•}¥µ…•}Í½ÕÉ” ‰ÕÉ¥½ÕÌ	¥Á•‘…°Í•ÍÍ¥½¸Á…¹•°±•™Ð…Àˆ°($$$$$$$€€€€€€‰…ÍÍ•ÑÌ½Í•ÍÍ¥½¸µÁ…¹•°µ±•™Ð¹Á¹œˆ¤ì(%½Ù•É±…ä´ùÍ•ÍÍ¥½¹}Á…¹•°€ôµ…­•}ÁÉ¥Ù…Ñ•}¥µ…•}Í½ÕÉ” ‰ÕÉ¥½ÕÌ	¥Á•‘…°Í•ÍÍ¥½¸Á…¹•°™¥±°ˆ°($$$$$$$€‰…ÍÍ•ÑÌ½Í•ÍÍ¥½¸µÁ…¹•°µµ¥‘‘±”¹Á¹œˆ¤ì(%½Ù•É±…ä´ùÍ•ÍÍ¥½¹}Á…¹•±}É¥¡Ð€ôµ…­•}ÁÉ¥Ù…Ñ•}¥µ…•}Í½ÕÉ” ‰ÕÉ¥½ÕÌ	¥Á•‘…°Í•ÍÍ¥½¸Á…¹•°É¥¡Ð…Àˆ°($$$$$$$€€€€€€€‰…ÍÍ•ÑÌ½Í•ÍÍ¥½¸µÁ…¹•°µÉ¥¡Ð¹Á¹œˆ¤ì(%½Ù•É±…ä´ùÑ•±•µ•ÑÉå}Á…¹•°€ôµ…­•}ÁÉ¥Ù…Ñ•}¥µ…•}Í½ÕÉ” ‰ÕÉ¥½ÕÌ	¥Á•‘…°É½Õ¹‘•Ñ•±•µ•ÑÉäÁ…¹•°ˆ°($$$$$$$€€€‰…ÍÍ•ÑÌ½Ñ•±•µ•ÑÉäµÁ…¹•°µÉ½Õ¹‘•¹Á¹œˆ¤ì(%½Ù•É±…ä´ùÍ•ÍÍ¥½¹}Á…¹•±}±•™Ñ}½Á…¥Ñå}™¥±Ñ•È€ô($%…ÑÑ…¡}½Á…¥Ñå}™¥±Ñ•È¡½Ù•É±…ä´ùÍ•ÍÍ¥½¹}Á…¹•±}±•™Ð°€‰ÕÉ¥½ÕÌ	¥Á•‘…°Í•ÍÍ¥½¸Á…¹•°±•™Ð½Á…¥Ñäˆ¤ì(%½Ù•É±…ä´ùÍ•ÍÍ¥½¹}Á…¹•±}½Á…¥Ñå}™¥±Ñ•È€ô($%…ÑÑ…¡}½Á…¥Ñå}™¥±Ñ•È¡½Ù•É±…ä´ùÍ•ÍÍ¥½¹}Á…¹•°°€‰ÕÉ¥½ÕÌ	¥Á•‘…°Í•ÍÍ¥½¸Á…¹•°™¥±°½Á…¥Ñäˆ¤ì(%½Ù•É±…ä´ùÍ•ÍÍ¥½¹}Á…¹•±}É¥¡Ñ}½Á…¥Ñå}™¥±Ñ•È€ô($%…ÑÑ…¡}½Á…¥Ñå}™¥±Ñ•È¡½Ù•É±…ä´ùÍ•ÍÍ¥½¹}Á…¹•±}É¥¡Ð°€‰ÕÉ¥½ÕÌ	¥Á•‘…°Í•ÍÍ¥½¸Á…¹•°É¥¡Ð½Á…¥Ñäˆ¤ì(%½Ù•É±…ä´ùÑ•±•µ•ÑÉå}Á…¹•±}½Á…¥Ñå}™¥±Ñ•È€ô($%…ÑÑ…¡}½Á…¥Ñå}™¥±Ñ•È¡½Ù•É±…ä´ùÑ•±•µ•ÑÉå}Á…¹•°°€‰ÕÉ¥½ÕÌ	¥Á•‘…°Ñ•±•µ•ÑÉäÁ…¹•°½Á…¥Ñäˆ¤ì(%½Ù•É±…ä´ù…•¹Ð€ôµ…­•}ÁÉ¥Ù…Ñ•}Í½ÕÉ” ‰½±½É}Í½ÕÉ•}ØÌˆ°€‰ÕÉ¥½ÕÌ	¥Á•‘…°…•¹Ðˆ¤ì(%½Ù•É±…ä´ù±…‰•±}Ñ•áÐ€ôµ…­•}ÁÉ¥Ù…Ñ•}Í½ÕÉ” ‰Ñ•áÑ}‘¥Á±ÕÍ}ØÌˆ°€‰ÕÉ¥½ÕÌ	¥Á•‘…°±…‰•°ˆ¤ì(%½Ù•É±…ä´ùÑ¥Ñ±•}Ñ•áÐ€ôµ…­•}ÁÉ¥Ù…Ñ•}Í½ÕÉ” ‰Ñ•áÑ}‘¥Á±ÕÍ}ØÌˆ°€‰ÕÉ¥½ÕÌ	¥Á•‘…°Ñ¥Ñ±”ˆ¤ì(%½Ù•É±…ä´ù±½}Ñ•áÐ€ôµ…­•}ÁÉ¥Ù…Ñ•}Í½ÕÉ” ‰Ñ•áÑ}‘¥Á±ÕÍ}ØÌˆ°€‰ÕÉ¥½ÕÌ	¥Á•‘…°±½œˆ¤ì(%½Ù•É±…ä´ùÑ¥µ•}Ñ•áÐ€ôµ…­•}ÁÉ¥Ù…Ñ•}Í½ÕÉ” ‰Ñ•áÑ}‘¥Á±ÕÍ}ØÌˆ°€‰ÕÉ¥½ÕÌ	¥Á•‘…°Ñ¥µ”ˆ¤ì(%½Ù•É±…ä´ù‘…Ñ•}Ñ•áÐ€ôµ…­•}ÁÉ¥Ù…Ñ•}Í½ÕÉ” ‰Ñ•áÑ}‘¥Á±ÕÍ}ØÌˆ°€‰ÕÉ¥½ÕÌ	¥Á•‘…°‘…Ñ”ˆ¤ì(%½Ù•É±…ä´ùÑ¥µ•É}Ñ•áÐ€ôµ…­•}ÁÉ¥Ù…Ñ•}Í½ÕÉ” ‰Ñ•áÑ}‘¥Á±ÕÍ}ØÌˆ°€‰ÕÉ¥½ÕÌ	¥Á•‘…°Ñ¥µ•Èˆ¤ì((%¡…È€©±½½}Á…Ñ €ô½‰Í}µ½‘Õ±•}™¥±” ‰…ÍÍ•ÑÌ½ÕÉ¥½ÕÌµ‰¥Á•‘…°µÁÉ¥µ…Éäµ±åÁ µÍ…™”¹Á¹œˆ¤ì(%¥˜€¡±½½}Á…Ñ ¤ì($%½‰Í}‘…Ñ…}Ð€©±½½}Í•ÑÑ¥¹Ì€ô½‰Í}‘…Ñ…}É•…Ñ” ¤ì($%½‰Í}‘…Ñ…}Í•Ñ}ÍÑÉ¥¹œ¡±½½}Í•ÑÑ¥¹Ì°€‰™¥±”ˆ°±½½}Á…Ñ ¤ì($%½‰Í}‘…Ñ…}Í•Ñ}‰½½°¡±½½}Í•ÑÑ¥¹Ì°€‰Õ¹±½…ˆ°™…±Í”¤ì($%½Ù•É±…ä´ù±½¼€ô½‰Í}Í½ÕÉ•}É•…Ñ•}ÁÉ¥Ù…Ñ” ‰¥µ…•}Í½ÕÉ”ˆ°€‰ÕÉ¥½ÕÌ	¥Á•‘…°…ÁÁÉ½Ù•±½¼ˆ°±½½}Í•ÑÑ¥¹Ì¤ì($%½‰Í}‘…Ñ…}É•±•…Í”¡±½½}Í•ÑÑ¥¹Ì¤ì($%‰™É•”¡±½½}Á…Ñ ¤ì(%ô(%½Ù•É±…ä´ù±½½}½Á…¥Ñå}™¥±Ñ•È€ô…ÑÑ…¡}½Á…¥Ñå}™¥±Ñ•È¡½Ù•É±…ä´ù±½¼°€‰ÕÉ¥½ÕÌ	¥Á•‘…°±½¼½Á…¥Ñäˆ¤ì(%…‘‘}…Ñ¥Ù•}¡¥±¡½Ù•É±…ä°½Ù•É±…ä´ùÍ•ÍÍ¥½¹}Á…¹•±}±•™Ð¤ì(%…‘‘}…Ñ¥Ù•}¡¥±¡½Ù•É±…ä°½Ù•É±…ä´ùÍ•ÍÍ¥½¹}Á…¹•°¤ì(%…‘‘}…Ñ¥Ù•}¡¥±¡½Ù•É±…ä°½Ù•É±…ä´ùÍ•ÍÍ¥½¹}Á…¹•±}É¥¡Ð¤ì(%…‘‘}…Ñ¥Ù•}¡¥±¡½Ù•É±…ä°½Ù•É±…ä´ùÑ•±•µ•ÑÉå}Á…¹•°¤ì(%…‘‘}…Ñ¥Ù•}¡¥±¡½Ù•É±…ä°½Ù•É±…ä´ù…•¹Ð¤ì(%…‘‘}…Ñ¥Ù•}¡¥±¡½Ù•É±…ä°½Ù•É±…ä´ù±½¼¤ì(%…‘‘}…Ñ¥Ù•}¡¥±¡½Ù•É±…ä°½Ù•É±…ä´ù±…‰•±}Ñ•áÐ¤ì(%…‘‘}…Ñ¥Ù•}¡¥±¡½Ù•É±…ä°½Ù•É±…ä´ùÑ¥Ñ±•}Ñ•áÐ¤ì(%…‘‘}…Ñ¥Ù•}¡¥±¡½Ù•É±…ä°½Ù•É±…ä´ù±½}Ñ•áÐ¤ì(%…‘‘}…Ñ¥Ù•}¡¥±¡½Ù•É±…ä°½Ù•É±…ä´ùÑ¥µ•}Ñ•áÐ¤ì(%…‘‘}…Ñ¥Ù•}¡¥±¡½Ù•É±…ä°½Ù•É±…ä´ù‘…Ñ•}Ñ•áÐ¤ì(%…‘‘}…Ñ¥Ù•}¡¥±¡½Ù•É±…ä°½Ù•É±…ä´ùÑ¥µ•É}Ñ•áÐ¤ì((%½Ù•É±…ä´ùÍÑ…ÉÑ}Á…ÕÍ•}¡½Ñ­•ä€ô½‰Í}¡½Ñ­•å}É•¥ÍÑ•É}Í½ÕÉ”¡Í½ÕÉ”°€‰ÕÉ¥½ÕÍ}‰¥Á•‘…°¹Ñ¥µ•È¹ÍÑ…ÉÑ}Á…ÕÍ”ˆ°($$$$$$$$€½‰Í}µ½‘Õ±•}Ñ•áÐ ‰!½Ñ­•åMÑ…ÉÑA…ÕÍ”ˆ¤°¡½Ñ­•å}ÍÑ…ÉÑ}Á…ÕÍ”°½Ù•É±…ä¤ì(%½Ù•É±…ä´ùÉ•Í•Ñ}¡½Ñ­•ä€ô½‰Í}¡½Ñ­•å}É•¥ÍÑ•É}Í½ÕÉ”¡Í½ÕÉ”°€‰ÕÉ¥½ÕÍ}‰¥Á•‘…°¹Ñ¥µ•È¹É•Í•Ðˆ°($$$$$$%½‰Í}µ½‘Õ±•}Ñ•áÐ ‰!½Ñ­•åI•Í•Ðˆ¤°¡½Ñ­•å}É•Í•Ð°½Ù•É±…ä¤ì(%ì($%ÍÑèé±½­}Õ…ÉñÍÑèéµÕÑ•àø±½¬¡}¥¹ÍÑ…¹•Í}µÕÑ•à¤ì($%}¥¹ÍÑ…¹•Ì¹ÁÕÍ¡}‰…¬¡½Ù•É±…ä¤ì(%ô(%½Ù•É±…å}ÕÁ‘…Ñ”¡½Ù•É±…ä°Í•ÑÑ¥¹Ì¤ì(%É•ÑÕÉ¸½Ù•É±…äì)ô()Ù½¥É•±•…Í•}Í½ÕÉ”¡½‰Í}Í½ÕÉ•}Ð€¨™Í½ÕÉ”¤)ì(%¥˜€¡Í½ÕÉ”¤ì($%½‰Í}Í½ÕÉ•}É•±•…Í”¡Í½ÕÉ”¤ì($%Í½ÕÉ”€ô¹Õ±±ÁÑÈì(%ô)ô()Ù½¥½Ù•É±…å}‘•ÍÑÉ½ä¡Ù½¥€©‘…Ñ„¤)ì(%…ÕÑ¼€©½Ù•É±…ä€ôÍÑ…Ñ¥}…ÍÐñ=Ù•É±…ä€¨ø¡‘…Ñ„¤ì(%½Ù•É±…ä´ù‘•ÍÑÉ½å¥¹œ¹ÍÑ½É”¡ÑÉÕ”°ÍÑèéµ•µ½Éå}½É‘•É}É•±•…Í”¤ì(%ì($%ÍÑèé±½­}Õ…ÉñÍÑèéµÕÑ•àø±¥™•Ñ¥µ•}±½¬¡½Ù•É±…ä´ù±¥™•Ñ¥µ”´ùµÕÑ•à¤ì($%½Ù•É±…ä´ù±¥™•Ñ¥µ”´ù½Ù•É±…ä€ô¹Õ±±ÁÑÈì(%ô(%ì($%ÍÑèé±½­}Õ…ÉñÍÑèéµÕÑ•àø±½¬¡}¥¹ÍÑ…¹•Í}µÕÑ•à¤ì($%}¥¹ÍÑ…¹•Ì¹•É…Í”¡ÍÑèéÉ•µ½Ù”¡}¥¹ÍÑ…¹•Ì¹‰•¥¸ ¤°}¥¹ÍÑ…¹•Ì¹•¹ ¤°½Ù•É±…ä¤°}¥¹ÍÑ…¹•Ì¹•¹ ¤¤ì(%ô(%‘¥Í½¹¹•Ñ}Ù•ÉÑ¥…±}½ÕÑÁÕÐ¡½Ù•É±…ä¤ì(%É•µ½Ù•}…Ñ¥Ù•}¡¥±¡½Ù•É±…ä°½Ù•É±…ä´ùÍ•ÍÍ¥½¹}Á…¹•±}±•™Ð¤ì(%É•µ½Ù•}…Ñ¥Ù•}¡¥±¡½Ù•É±…ä°½Ù•É±…ä´ùÍ•ÍÍ¥½¹}Á…¹•°¤ì(%É•µ½Ù•}…Ñ¥Ù•}¡¥±¡½Ù•É±…ä°½Ù•É±…ä´ùÍ•ÍÍ¥½¹}Á…¹•±}É¥¡Ð¤ì(%É•µ½Ù•}…Ñ¥Ù•}¡¥±¡½Ù•É±…ä°½Ù•É±…ä´ùÑ•±•µ•ÑÉå}Á…¹•°¤ì(%É•µ½Ù•}…Ñ¥Ù•}¡¥±¡½Ù•É±…ä°½Ù•É±…ä´ù…•¹Ð¤ì(%É•µ½Ù•}…Ñ¥Ù•}¡¥±¡½Ù•É±…ä°½Ù•É±…ä´ù±½¼¤ì(%É•µ½Ù•}…Ñ¥Ù•}¡¥±¡½Ù•É±…ä°½Ù•É±…ä´ù±…‰•±}Ñ•áÐ¤ì(%É•µ½Ù•}…Ñ¥Ù•}¡¥±¡½Ù•É±…ä°½Ù•É±…ä´ùÑ¥Ñ±•}Ñ•áÐ¤ì(%É•µ½Ù•}…Ñ¥Ù•}¡¥±¡½Ù•É±…ä°½Ù•É±…ä´ù±½}Ñ•áÐ¤ì(%É•µ½Ù•}…Ñ¥Ù•}¡¥±¡½Ù•É±…ä°½Ù•É±…ä´ùÑ¥µ•}Ñ•áÐ¤ì(%É•µ½Ù•}…Ñ¥Ù•}¡¥±¡½Ù•É±…ä°½Ù•É±…ä´ù‘…Ñ•}Ñ•áÐ¤ì(%É•µ½Ù•}…Ñ¥Ù•}¡¥±¡½Ù•É±…ä°½Ù•É±…ä´ùÑ¥µ•É}Ñ•áÐ¤ì(%¥˜€¡½Ù•É±…ä´ù±½¼€˜˜½Ù•É±…ä´ù±½½}½Á…¥Ñå}™¥±Ñ•È¤($%½‰Í}Í½ÕÉ•}™¥±Ñ•É}É•µ½Ù”¡½Ù•É±…ä´ù±½¼°½Ù•É±…ä´ù±½½}½Á…¥Ñå}™¥±Ñ•È¤ì(%¥˜€¡½Ù•É±…ä´ùÍ•ÍÍ¥½¹}Á…¹•±}±•™Ð€˜˜½Ù•É±…ä´ùÍ•ÍÍ¥½¹}Á…¹•±}±•™Ñ}½Á…¥Ñå}™¥±Ñ•È¤($%½‰Í}Í½ÕÉ•}™¥±Ñ•É}É•µ½Ù”¡½Ù•É±…ä´ùÍ•ÍÍ¥½¹}Á…¹•±}±•™Ð°½Ù•É±…ä´ùÍ•ÍÍ¥½¹}Á…¹•±}±•™Ñ}½Á…¥Ñå}™¥±Ñ•È¤ì(%¥˜€¡½Ù•É±…ä´ùÍ•ÍÍ¥½¹}Á…¹•°€˜˜½Ù•É±…ä´ùÍ•ÍÍ¥½¹}Á…¹•±}½Á…¥Ñå}™¥±Ñ•È¤($%½‰Í}Í½ÕÉ•}™¥±Ñ•É}É•µ½Ù”¡½Ù•É±…ä´ùÍ•ÍÍ¥½¹}Á…¹•°°½Ù•É±…ä´ùÍ•ÍÍ¥½¹}Á…¹•±}½Á…¥Ñå}™¥±Ñ•È¤ì(%¥˜€¡½Ù•É±…ä´ùÍ•ÍÍ¥½¹}Á…¹•±}É¥¡Ð€˜˜½Ù•É±…ä´ùÍ•ÍÍ¥½¹}Á…¹•±}É¥¡Ñ}½Á…¥Ñå}™¥±Ñ•È¤($%½‰Í}Í½ÕÉ•}™¥±Ñ•É}É•µ½Ù”¡½Ù•É±…ä´ùÍ•ÍÍ¥½¹}Á…¹•±}É¥¡Ð°½Ù•É±…ä´ùÍ•ÍÍ¥½¹}Á…¹•±}É¥¡Ñ}½Á…¥Ñå}™¥±Ñ•È¤ì(%¥˜€¡½Ù•É±…ä´ùÑ•±•µ•ÑÉå}Á…¹•°€˜˜½Ù•É±…ä´ùÑ•±•µ•ÑÉå}Á…¹•±}½Á…¥Ñå}™¥±Ñ•È¤($%½‰Í}Í½ÕÉ•}™¥±Ñ•É}É•µ½Ù”¡½Ù•É±…ä´ùÑ•±•µ•ÑÉå}Á…¹•°°½Ù•É±…ä´ùÑ•±•µ•ÑÉå}Á…¹•±}½Á…¥Ñå}™¥±Ñ•È¤ì(%É•±•…Í•}Í½ÕÉ”¡½Ù•É±…ä´ù±½½}½Á…¥Ñå}™¥±Ñ•È¤ì(%É•±•…Í•}Í½ÕÉ”¡½Ù•É±…ä´ùÍ•ÍÍ¥½¹}Á…¹•±}±•™Ñ}½Á…¥Ñå}™¥±Ñ•È¤ì(%É•±•…Í•}Í½ÕÉ”¡½Ù•É±…ä´ùÍ•ÍÍ¥½¹}Á…¹•±}½Á…¥Ñå}™¥±Ñ•È¤ì(%É•±•…Í•}Í½ÕÉ”¡½Ù•É±…ä´ùÍ•ÍÍ¥½¹}Á…¹•±}É¥¡Ñ}½Á…¥Ñå}™¥±Ñ•È¤ì(%É•±•…Í•}Í½ÕÉ”¡½Ù•É±…ä´ùÑ•±•µ•ÑÉå}Á…¹•±}½Á…¥Ñå}™¥±Ñ•È¤ì(%É•±•…Í•}Í½ÕÉ”¡½Ù•É±…ä´ù±½¼¤ì(%É•±•…Í•}Í½ÕÉ”¡½Ù•É±…ä´ùÍ•ÍÍ¥½¹}Á…¹•±}±•™Ð¤ì(%É•±•…Í•}Í½ÕÉ”¡½Ù•É±…ä´ùÍ•ÍÍ¥½¹}Á…¹•°¤ì(%É•±•…Í•}Í½ÕÉ”¡½Ù•É±…ä´ùÍ•ÍÍ¥½¹}Á…¹•±}É¥¡Ð¤ì(%É•±•…Í•}Í½ÕÉ”¡½Ù•É±…ä´ùÑ•±•µ•ÑÉå}Á…¹•°¤ì(%É•±•…Í•}Í½ÕÉ”¡½Ù•É±…ä´ù…•¹Ð¤ì(%É•±•…Í•}Í½ÕÉ”¡½Ù•É±…ä´ù±…‰•±}Ñ•áÐ¤ì(%É•±•…Í•}Í½ÕÉ”¡½Ù•É±…ä´ùÑ¥Ñ±•}Ñ•áÐ¤ì(%É•±•…Í•}Í½ÕÉ”¡½Ù•É±…ä´ù±½}Ñ•áÐ¤ì(%É•±•…Í•}Í½ÕÉ”¡½Ù•É±…ä´ùÑ¥µ•}Ñ•áÐ¤ì(%É•±•…Í•}Í½ÕÉ”¡½Ù•É±…ä´ù‘…Ñ•}Ñ•áÐ¤ì(%É•±•…Í•}Í½ÕÉ”¡½Ù•É±…ä´ùÑ¥µ•É}Ñ•áÐ¤ì(%‘•±•Ñ”½Ù•É±…äì)ô()Õ¥¹ÐÌÉ}Ð½Ù•É±…å}Ý¥‘Ñ ¡Ù½¥€©‘…Ñ„¤)ì(%…ÕÑ¼€©½Ù•É±…ä€ôÍÑ…Ñ¥}…ÍÐñ=Ù•É±…ä€¨ø¡‘…Ñ„¤ì(%ÍÑèé±½­}Õ…ÉñÍÑèéµÕÑ•àø±½¬¡½Ù•É±…ä´ùµÕÑ•à¤ì(%É•ÑÕÉ¸½Ù•É±…ä´ùÝ¥‘Ñ ì)ô()Õ¥¹ÐÌÉ}Ð½Ù•É±…å}¡•¥¡Ð¡Ù½¥€©‘…Ñ„¤)ì(%…ÕÑ¼€©½Ù•É±…ä€ôÍÑ…Ñ¥}…ÍÐñ=Ù•É±…ä€¨ø¡‘…Ñ„¤ì(%ÍÑèé±½­}Õ…ÉñÍÑèéµÕÑ•àø±½¬¡½Ù•É±…ä´ùµÕÑ•à¤ì(%É•ÑÕÉ¸½Ù•É±…ä´ù¡•¥¡Ðì)ô()Ù½¥½Ù•É±…å}•¹Õµ}…Ñ¥Ù•}Í½ÕÉ•Ì¡Ù½¥€©‘…Ñ„°½‰Í}Í½ÕÉ•}•¹Õµ}ÁÉ½}Ð•¹Õµ}…±±‰…¬°Ù½¥€©Á…É…´¤)ì(%…ÕÑ¼€©½Ù•É±…ä€ôÍÑ…Ñ¥}…ÍÐñ=Ù•É±…ä€¨ø¡‘…Ñ„¤ì(%½‰Í}Í½ÕÉ•}Ð€©¡¥±‘É•¹mt€ôí½Ù•É±…ä´ùÍ•ÍÍ¥½¹}Á…¹•±}±•™Ð°€½Ù•É±…ä´ùÍ•ÍÍ¥½¹}Á…¹•°°($$$$€€€½Ù•É±…ä´ùÍ•ÍÍ¥½¹}Á…¹•±}É¥¡Ð°½Ù•É±…ä´ùÑ•±•µ•ÑÉå}Á…¹•°°($$$$€€€½Ù•É±…ä´ù…•¹Ð°€€€€€€€€€€€€€½Ù•É±…ä´ù±½¼°($$$$€€€½Ù•É±…ä´ù±…‰•±}Ñ•áÐ°€€€€€€€€€½Ù•É±…ä´ùÑ¥Ñ±•}Ñ•áÐ°($$$$€€€½Ù•É±…ä´ù±½}Ñ•áÐ°€€€€€€€€€€€½Ù•É±…ä´ùÑ¥µ•}Ñ•áÐ°($$$$€€€½Ù•É±…ä´ù‘…Ñ•}Ñ•áÐ°€€€€€€€€€€½Ù•É±…ä´ùÑ¥µ•É}Ñ•áÑôì(%™½È€¡½‰Í}Í½ÕÉ•}Ð€©¡¥±€è¡¥±‘É•¸¤ì($%¥˜€¡¡¥±¤($$%•¹Õµ}…±±‰…¬¡½Ù•É±…ä´ùÍ½ÕÉ”°¡¥±°Á…É…´¤ì(%ô)ô()Ù½¥½Ù•É±…å}Í…Ù”¡Ù½¥€©‘…Ñ„°½‰Í}‘…Ñ…}Ð€©Í•ÑÑ¥¹Ì¤)ì(%…ÕÑ¼€©½Ù•É±…ä€ôÍÑ…Ñ¥}…ÍÐñ=Ù•É±…ä€¨ø¡‘…Ñ„¤ì(%ÍÑèé±½­}Õ…ÉñÍÑèéµÕÑ•àø±½¬¡½Ù•É±…ä´ùµÕÑ•à¤ì(%½‰Í}‘…Ñ…}Í•Ñ}¥¹Ð¡Í•ÑÑ¥¹Ì°€‰Ñ¥µ•É}•±…ÁÍ•‘}¹Ìˆ°ÍÑ…Ñ¥}…ÍÐñ±½¹œ±½¹œø¡•±…ÁÍ•‘}¹Í}±½­•¡½Ù•É±…ä°½Í}•ÑÑ¥µ•}¹Ì ¤¤¤¤ì(%½‰Í}‘…Ñ…}Í•Ñ}‰½½°¡Í•ÑÑ¥¹Ì°€‰Ñ¥µ•É}Ý…Í}ÉÕ¹¹¥¹œˆ°½Ù•É±…ä´ùÑ¥µ•É}ÉÕ¹¹¥¹œ¤ì)ô()Ù½¥½Ù•É±…å}±½…¡Ù½¥€©‘…Ñ„°½‰Í}‘…Ñ…}Ð€©Í•ÑÑ¥¹Ì¤)ì(%…ÕÑ¼€©½Ù•É±…ä€ôÍÑ…Ñ¥}…ÍÐñ=Ù•É±…ä€¨ø¡‘…Ñ„¤ì(%ÍÑèé±½­}Õ…ÉñÍÑèéµÕÑ•àø±½¬¡½Ù•É±…ä´ùµÕÑ•à¤ì(%¥˜€ …½‰Í}‘…Ñ…}¡…Í}ÕÍ•É}Ù…±Õ”¡Í•ÑÑ¥¹Ì°€‰Ñ¥µ•É}•±…ÁÍ•‘}¹Ìˆ¤¤($%É•ÑÕÉ¸ì(%½Ù•É±…ä´ù•±…ÁÍ•‘}‰•™½É•}ÍÑ…ÉÑ}¹Ì€ô($%ÍÑ…Ñ¥}…ÍÐñÕ¥¹ÐØÑ}Ðø¡ÍÑèéµ…àñ±½¹œ±½¹œø À°½‰Í}‘…Ñ…}•Ñ}¥¹Ð¡Í•ÑÑ¥¹Ì°€‰Ñ¥µ•É}•±…ÁÍ•‘}¹Ìˆ¤¤¤ì(%½Ù•É±…ä´ùÑ¥µ•É}ÍÑ…ÉÑ•‘}¹Ì€ô½Í}•ÑÑ¥µ•}¹Ì ¤ì(%¥˜€¡½Ù•É±…ä´ù‰¥¹‘¥¹œ€ôôQ¥µ•É	¥¹‘¥¹œèé5…¹Õ…°¤($%½Ù•É±…ä´ùÑ¥µ•É}ÉÕ¹¹¥¹œ€ô½‰Í}‘…Ñ…}•Ñ}‰½½°¡Í•ÑÑ¥¹Ì°€‰Ñ¥µ•É}Ý…Í}ÉÕ¹¹¥¹œˆ¤ì)ô()Ù½¥½Ù•É±…å}Ñ¥¬¡Ù½¥€©‘…Ñ„°™±½…Ð¤)ì(%…ÕÑ¼€©½Ù•É±…ä€ôÍÑ…Ñ¥}…ÍÐñ=Ù•É±…ä€¨ø¡‘…Ñ„¤ì(%½¹ÍÐÕ¥¹ÐØÑ}Ð¹½Ü€ô½Í}•ÑÑ¥µ•}¹Ì ¤ì(%ì($%ÍÑèé±½­}Õ…ÉñÍÑèéµÕÑ•àø±½¬¡½Ù•É±…ä´ùµÕÑ•à¤ì($%¥˜€¡¹½Ü€´½Ù•É±…ä´ù±…ÍÑ}±½­}ÕÁ‘…Ñ•}¹Ì€øô9M}AI}M=9€¼€Ð¤ì($$%ÕÁ‘…Ñ•}±½­}¡¥±‘É•¸¡½Ù•É±…ä°¹½Ü¤ì($$%½Ù•É±…ä´ù±…ÍÑ}±½­}ÕÁ‘…Ñ•}¹Ì€ô¹½Üì($%ô(%ô(%‰½½°É•ÑÉå}…¥ÑÕ´€ô™…±Í”ì(%ì($%ÍÑèé±½­}Õ…ÉñÍÑèéµÕÑ•àøÍÑ…Ñ•}±½¬¡½Ù•É±…ä´ùµÕÑ•à¤ì($%ÍÑèé±½­}Õ…ÉñÍÑèéµÕÑ•àø½ÕÑÁÕÑ}±½¬¡½Ù•É±…ä´ù½ÕÑÁÕÑ}µÕÑ•à¤ì($%É•ÑÉå}…¥ÑÕ´€ô½Ù•É±…ä´ù‰¥¹‘¥¹œ€ôôQ¥µ•É	¥¹‘¥¹œèé¥ÑÕµY•ÉÑ¥…°€˜˜€…½Ù•É±…ä´ùÙ•ÉÑ¥…±}½ÕÑÁÕÐ€˜˜($$$€€€€€¹½Ü€´½Ù•É±…ä´ù±…ÍÑ}…¥ÑÕµ}É•ÑÉå}¹Ì€øô€Ì€¨9M}AI}M=9ì($%¥˜€¡É•ÑÉå}…¥ÑÕ´¤($$%½Ù•É±…ä´ù±…ÍÑ}…¥ÑÕµ}É•ÑÉå}¹Ì€ô¹½Üì(%ô(%¥˜€¡É•ÑÉå}…¥ÑÕ´¤($%É•ÅÕ•ÍÑ}Ù•ÉÑ¥…±}½ÕÑÁÕÑ}½¹¹•Ñ¥½¸¡½Ù•É±…ä¤ì)ô()Ù½¥½Ù•É±…å}É•¹‘•È¡Ù½¥€©‘…Ñ„°Í}•™™•Ñ}Ð€¨¤)ì(%…ÕÑ¼€©½Ù•É±…ä€ôÍÑ…Ñ¥}…ÍÐñ=Ù•É±…ä€¨ø¡‘…Ñ„¤ì(%Õ¥¹ÐÌÉ}ÐÝ¥‘Ñ ì(%Õ¥¹ÐÌÉ}Ð¡•¥¡Ðì(%1…å½ÕÐ±…å½ÕÐì(%¥¹ÐÍ…±•}Á•É•¹Ðì(%¥¹Ð•‘•}µ…É¥¸ì(%‰½½°Í¡½Ý}±½¼ì(%‰½½°Í¡½Ý}‘…Ñ”ì(%‰½½°Í¡½Ý}Ñ¥µ”ì(%‰½½°Í¡½Ý}Ñ¥µ•Èì(%ì($%ÍÑèé±½­}Õ…ÉñÍÑèéµÕÑ•àø±½¬¡½Ù•É±…ä´ùµÕÑ•à¤ì($%Ý¥‘Ñ €ô½Ù•É±…ä´ùÝ¥‘Ñ ì($%¡•¥¡Ð€ô½Ù•É±…ä´ù¡•¥¡Ðì($%±…å½ÕÐ€ô½Ù•É±…ä´ù±…å½ÕÐì($%Í…±•}Á•É•¹Ð€ô½Ù•É±…ä´ùÍ…±•}Á•É•¹Ðì($%•‘•}µ…É¥¸€ô½Ù•É±…ä´ù•‘•}µ…É¥¸ì($%Í¡½Ý}±½¼€ô½Ù•É±…ä´ùÍ¡½Ý}±½¼ì($%Í¡½Ý}‘…Ñ”€ô½Ù•É±…ä´ùÍ¡½Ý}‘…Ñ”ì($%Í¡½Ý}Ñ¥µ”€ô½Ù•É±…ä´ùÍ¡½Ý}Ñ¥µ”ì($%Í¡½Ý}Ñ¥µ•È€ô½Ù•É±…ä´ùÍ¡½Ý}Ñ¥µ•Èì(%ô(%½¹ÍÐ™±½…ÐÕ¹¥Ð€ô€¡±…å½ÕÐ€ôô1…å½ÕÐèéY•ÉÑ¥…°¤($$$$€€€üÍÑèéµ¥¸¡ÍÑ…Ñ¥}…ÍÐñ™±½…Ðø¡Ý¥‘Ñ ¤€¼€ÄÐÐÀ¸Á˜°ÍÑ…Ñ¥}…ÍÐñ™±½…Ðø¡¡•¥¡Ð¤€¼€ÈÔØÀ¸Á˜¤($$$$€€€èÍÑèéµ¥¸¡ÍÑ…Ñ¥}…ÍÐñ™±½…Ðø¡Ý¥‘Ñ ¤€¼€ÈÔØÀ¸Á˜°ÍÑ…Ñ¥}…ÍÐñ™±½…Ðø¡¡•¥¡Ð¤€¼€ÄÐÐÀ¸Á˜¤ì(%½¹ÍÐ™±½…Ðµ…É¥¸€ôÍÑ…Ñ¥}…ÍÐñ™±½…Ðø¡•‘•}µ…É¥¸¤€¨Õ¹¥Ðì(%½¹ÍÐ™±½…ÐÉ•ÅÕ•ÍÑ•‘}Í…±”€ôÕ¹¥Ð€¨ÍÑ…Ñ¥}…ÍÐñ™±½…Ðø¡Í…±•}Á•É•¹Ð¤€¼€ÄÀÀ¸Á˜ì(%½¹ÍÐ™±½…Ð…Ù…¥±…‰±•}Ý¥‘Ñ €ôÍÑèéµ…à¡ÍÑ…Ñ¥}…ÍÐñ™±½…Ðø¡Ý¥‘Ñ ¤€´€È¸Á˜€¨µ…É¥¸°€Ä¸Á˜¤ì(%½¹ÍÐ™±½…Ð…Ù…¥±…‰±•}¡•¥¡Ð€ôÍÑèéµ…à¡ÍÑ…Ñ¥}…ÍÐñ™±½…Ðø¡¡•¥¡Ð¤€´€È¸Á˜€¨µ…É¥¸°€Ä¸Á˜¤ì(%½¹ÍÐ…ÕÑ¼Í½ÕÉ•}Ý¥‘Ñ €ômt¡½‰Í}Í½ÕÉ•}Ð€©Í½ÕÉ”¤ìÉ•ÑÕÉ¸Í½ÕÉ”€ü½‰Í}Í½ÕÉ•}•Ñ}Ý¥‘Ñ ¡Í½ÕÉ”¤€è€ÁTìôì(%½¹ÍÐ™±½…ÐÍ•ÍÍ¥½¹}Ñ•áÑ}Ý¥‘Ñ €ôÍÑ…Ñ¥}…ÍÐñ™±½…Ðø ($%ÍÑèéµ…à¡íÍ½ÕÉ•}Ý¥‘Ñ ¡½Ù•É±…ä´ù±…‰•±}Ñ•áÐ¤°Í½ÕÉ•}Ý¥‘Ñ ¡½Ù•É±…ä´ùÑ¥Ñ±•}Ñ•áÐ¤°($$$€Í½ÕÉ•}Ý¥‘Ñ ¡½Ù•É±…ä´ù±½}Ñ•áÐ¥ô¤¤ì(%½¹ÍÐ™±½…ÐÍ•ÍÍ¥½¹}½¹Ñ•¹Ñ}à€ôÍ¡½Ý}±½¼€ü€ÄØÀ¸Á˜€è€ÌÀ¸Á˜ì(%½¹ÍÐ™±½…ÐÍ•ÍÍ¥½¹}¹…Ñ¥Ù•}Ý¥‘Ñ €ô($%ÍÑèéµ…à¡Í•ÍÍ¥½¹}½¹Ñ•¹Ñ}à€¬Í•ÍÍ¥½¹}Ñ•áÑ}Ý¥‘Ñ €¬€ÌÀ¸Á˜°Í¡½Ý}±½¼€ü€ÌØÀ¸Á˜€è€ÈØÀ¸Á˜¤ì(%½¹ÍÐ™±½…Ð™¥Ñ}Í…±”€ôÍÑèéµ¥¸¡í…Ù…¥±…‰±•}Ý¥‘Ñ €¼Í•ÍÍ¥½¹}¹…Ñ¥Ù•}Ý¥‘Ñ °…Ù…¥±…‰±•}Ý¥‘Ñ €¼€ÐØÀ¸Á˜°($$$$$€…Ù…¥±…‰±•}¡•¥¡Ð€¼€ÄÐà¸Á˜°…Ù…¥±…‰±•}¡•¥¡Ð€¼€ÄÈÐ¸Á™ô¤ì(%½¹ÍÐ™±½…ÐÍ…±”€ôÍÑèéµ¥¸¡É•ÅÕ•ÍÑ•‘}Í…±”°™¥Ñ}Í…±”¤ì(%½¹ÍÐ™±½…ÐÍ•ÍÍ¥½¹} €ô€ÄÐà¸Á˜€¨Í…±”ì(%½¹ÍÐ™±½…ÐÑ•±•µ•ÑÉå}Ü€ô€ÐØÀ¸Á˜€¨Í…±”ì(%½¹ÍÐ™±½…ÐÍ•ÍÍ¥½¹}à€ôµ…É¥¸ì(%½¹ÍÐ™±½…ÐÍ•ÍÍ¥½¹}ä€ôÍÑ…Ñ¥}…ÍÐñ™±½…Ðø¡¡•¥¡Ð¤€´µ…É¥¸€´Í•ÍÍ¥½¹} ì(%½¹ÍÐ™±½…ÐÑ•±•µ•ÑÉå}à€ôÍÑ…Ñ¥}…ÍÐñ™±½…Ðø¡Ý¥‘Ñ ¤€´µ…É¥¸€´Ñ•±•µ•ÑÉå}Üì(%½¹ÍÐ™±½…ÐÑ•±•µ•ÑÉå}ä€ôµ…É¥¸ì((%½¹ÍÑ•áÁÈ™±½…ÐÁ…¹•±}…Á}Ý¥‘Ñ €ô€Èà¸Á˜ì(%É•¹‘•É}Í½ÕÉ”¡½Ù•É±…ä´ùÍ•ÍÍ¥½¹}Á…¹•±}±•™Ð°Í•ÍÍ¥½¹}à°Í•ÍÍ¥½¹}ä°Í…±”°Í…±”¤ì(%É•¹‘•É}Í½ÕÉ”¡½Ù•É±…ä´ùÍ•ÍÍ¥½¹}Á…¹•°°Í•ÍÍ¥½¹}à€¬Á…¹•±}…Á}Ý¥‘Ñ €¨Í…±”°Í•ÍÍ¥½¹}ä°($$€€€€€ÍÑèéµ…à¡Í•ÍÍ¥½¹}¹…Ñ¥Ù•}Ý¥‘Ñ €´€È¸Á˜€¨Á…¹•±}…Á}Ý¥‘Ñ °€Ä¸Á˜¤€¨Í…±”°Í…±”¤ì(%É•¹‘•É}Í½ÕÉ”¡½Ù•É±…ä´ùÍ•ÍÍ¥½¹}Á…¹•±}É¥¡Ð°Í•ÍÍ¥½¹}à€¬€¡Í•ÍÍ¥½¹}¹…Ñ¥Ù•}Ý¥‘Ñ €´Á…¹•±}…Á}Ý¥‘Ñ ¤€¨Í…±”°($$€€€€€Í•ÍÍ¥½¹}ä°Í…±”°Í…±”¤ì(%É•¹‘•É}Í½ÕÉ”¡½Ù•É±…ä´ù…•¹Ð°Í•ÍÍ¥½¹}à°Í•ÍÍ¥½¹}ä€¬€Èà¸Á˜€¨Í…±”°Í…±”°Í…±”¤ì(%¥˜€¡Í¡½Ý}±½¼€˜˜½Ù•É±…ä´ù±½¼¤ì($%½¹ÍÐ™±½…Ð±½½}¹…Ñ¥Ù”€ôÍÑ…Ñ¥}…ÍÐñ™±½…Ðø¡ÍÑèéµ…à¡½‰Í}Í½ÕÉ•}•Ñ}Ý¥‘Ñ ¡½Ù•É±…ä´ù±½¼¤°€ÅT¤¤ì($%½¹ÍÐ™±½…Ð±½½}Í¥é”€ô€ÄÈà¸Á˜€¨Í…±”ì($%É•¹‘•É}Í½ÕÉ”¡½Ù•É±…ä´ù±½¼°Í•ÍÍ¥½¹}à€¬€ÄØ¸Á˜€¨Í…±”°Í•ÍÍ¥½¹}ä€¬€ÄÀ¸Á˜€¨Í…±”°($$$€€€€€±½½}Í¥é”€¼±½½}¹…Ñ¥Ù”°±½½}Í¥é”€¼±½½}¹…Ñ¥Ù”¤ì(%ô(%½¹ÍÐ™±½…ÐÑ•áÑ}à€ôÍ•ÍÍ¥½¹}à€¬€¡Í¡½Ý}±½¼€ü€ÄØÀ¸Á˜€è€ÌÀ¸Á˜¤€¨Í…±”ì(%É•¹‘•É}Í½ÕÉ”¡½Ù•É±…ä´ù±…‰•±}Ñ•áÐ°Ñ•áÑ}à°Í•ÍÍ¥½¹}ä€¬€ÄÀ¸Á˜€¨Í…±”°Í…±”°Í…±”¤ì(%É•¹‘•É}Í½ÕÉ”¡½Ù•É±…ä´ùÑ¥Ñ±•}Ñ•áÐ°Ñ•áÑ}à°Í•ÍÍ¥½¹}ä€¬€Ìà¸Á˜€¨Í…±”°Í…±”°Í…±”¤ì(%É•¹‘•É}Í½ÕÉ”¡½Ù•É±…ä´ù±½}Ñ•áÐ°Ñ•áÑ}à°Í•ÍÍ¥½¹}ä€¬€äà¸Á˜€¨Í…±”°Í…±”°Í…±”¤ì((%¥˜€¡Í¡½Ý}‘…Ñ”ñðÍ¡½Ý}Ñ¥µ”ñðÍ¡½Ý}Ñ¥µ•È¤ì($%É•¹‘•É}Í½ÕÉ”¡½Ù•É±…ä´ùÑ•±•µ•ÑÉå}Á…¹•°°Ñ•±•µ•ÑÉå}à°Ñ•±•µ•ÑÉå}ä°Í…±”°Í…±”¤ì($%É•¹‘•É}Í½ÕÉ”¡½Ù•É±…ä´ùÑ¥µ•É}Ñ•áÐ°Ñ•±•µ•ÑÉå}à€¬€Èà¸Á˜€¨Í…±”°Ñ•±•µ•ÑÉå}ä€¬€à¸Á˜€¨Í…±”°Í…±”°Í…±”¤ì($%É•¹‘•É}Í½ÕÉ”¡½Ù•É±…ä´ùÑ¥µ•}Ñ•áÐ°Ñ•±•µ•ÑÉå}à€¬€Ðà¸Á˜€¨Í…±”°Ñ•±•µ•ÑÉå}ä€¬€ÜÔ¸Á˜€¨Í…±”°Í…±”°Í…±”¤ì($%É•¹‘•É}Í½ÕÉ”¡½Ù•É±…ä´ù‘…Ñ•}Ñ•áÐ°Ñ•±•µ•ÑÉå}à€¬€ÈÈÈ¸Á˜€¨Í…±”°Ñ•±•µ•ÑÉå}ä€¬€ÜÔ¸Á˜€¨Í…±”°Í…±”°Í…±”¤ì(%ô)ô()½¹ÍÐ¡…È€©½Ù•É±…å}¹…µ”¡Ù½¥€¨¤ìÉ•ÑÕÉ¸½‰Í}µ½‘Õ±•}Ñ•áÐ ‰M½ÕÉ•9…µ”ˆ¤ìô()Ù½¥™É½¹Ñ•¹‘}•Ù•¹Ð¡•¹Õ´½‰Í}™É½¹Ñ•¹‘}•Ù•¹Ð•Ù•¹Ð°Ù½¥€¨¤)ì(%¥˜€¡•Ù•¹Ð€„ô=	M}I=9Q9}Y9Q}MQI5%9}MQIQ€˜˜•Ù•¹Ð€„ô=	M}I=9Q9}Y9Q}MQI5%9}MQ=AA¤($%É•ÑÕÉ¸ì(%ÍÑèé±½­}Õ…ÉñÍÑèéµÕÑ•àø±½¬¡}¥¹ÍÑ…¹•Í}µÕÑ•à¤ì(%™½È€¡=Ù•É±…ä€©½Ù•É±…ä€è}¥¹ÍÑ…¹•Ì¤ì($%Q¥µ•É	¥¹‘¥¹œ‰¥¹‘¥¹œì($%ì($$%ÍÑèé±½­}Õ…ÉñÍÑèéµÕÑ•àø¥¹ÍÑ…¹•}±½¬¡½Ù•É±…ä´ùµÕÑ•à¤ì($$%‰¥¹‘¥¹œ€ô½Ù•É±…ä´ù‰¥¹‘¥¹œì($%ô($%¥˜€¡‰¥¹‘¥¹œ€„ôQ¥µ•É	¥¹‘¥¹œèé5…¥¹=‰Ì¤($$%½¹Ñ¥¹Õ”ì($%¥˜€¡•Ù•¹Ð€ôô=	M}I=9Q9}Y9Q}MQI5%9}MQIQ¤($$%Ñ¥µ•É}ÍÑ…ÉÐ¡½Ù•É±…ä¤ì($%•±Í”($$%Ñ¥µ•É}Á…ÕÍ”¡½Ù•É±…ä¤ì(%ô)ô()½‰Í}Í½ÕÉ•}¥¹™¼½Ù•É±…å}¥¹™¼€ôíôì()ô€¼¼¹…µ•ÍÁ…”()‰½½°½‰Í}µ½‘Õ±•}±½…¡Ù½¥¤)ì(%½Ù•É±…å}¥¹™¼¹¥€ôM=UI}%ì(%½Ù•É±…å}¥¹™¼¹ÑåÁ”€ô=	M}M=UI}QeA}%9AUPì(%½Ù•É±…å}¥¹™¼¹½ÕÑÁÕÑ}™±…Ì€ô($%=	M}M=UI}Y%<ð=	M}M=UI}UMQ=5}I\ð=	M}M=UI}=5A=M%Qð=	M}M=UI}MIì(%½Ù•É±…å}¥¹™¼¹•Ñ}¹…µ”€ô½Ù•É±…å}¹…µ”ì(%½Ù•É±…å}¥¹™¼¹É•…Ñ”€ô½Ù•É±…å}É•…Ñ”ì(%½Ù•É±…å}¥¹™¼¹‘•ÍÑÉ½ä€ô½Ù•É±…å}‘•ÍÑÉ½äì(%½Ù•É±…å}¥¹™¼¹•Ñ}Ý¥‘Ñ €ô½Ù•É±…å}Ý¥‘Ñ ì(%½Ù•É±…å}¥¹™¼¹•Ñ}¡•¥¡Ð€ô½Ù•É±…å}¡•¥¡Ðì(%½Ù•É±…å}¥¹™¼¹•Ñ}‘•™…Õ±ÑÌ€ô½Ù•É±…å}‘•™…Õ±ÑÌì(%½Ù•É±…å}¥¹™¼¹•Ñ}ÁÉ½Á•ÉÑ¥•Ì€ô½Ù•É±…å}ÁÉ½Á•ÉÑ¥•Ìì(%½Ù•É±…å}¥¹™¼¹ÕÁ‘…Ñ”€ô½Ù•É±…å}ÕÁ‘…Ñ”ì(%½Ù•É±…å}¥¹™¼¹Ù¥‘•½}Ñ¥¬€ô½Ù•É±…å}Ñ¥¬ì(%½Ù•É±…å}¥¹™¼¹Ù¥‘•½}É•¹‘•È€ô½Ù•É±…å}É•¹‘•Èì(%½Ù•É±…å}¥¹™¼¹•¹Õµ}…Ñ¥Ù•}Í½ÕÉ•Ì€ô½Ù•É±…å}•¹Õµ}…Ñ¥Ù•}Í½ÕÉ•Ìì(%½Ù•É±…å}¥¹™¼¹Í…Ù”€ô½Ù•É±…å}Í…Ù”ì(%½Ù•É±…å}¥¹™¼¹±½…€ô½Ù•É±…å}±½…ì(%½Ù•É±…å}¥¹™¼¹¥½¹}ÑåÁ”€ô=	M}%=9}QeA}QaPì(%½‰Í}É•¥ÍÑ•É}Í½ÕÉ” ™½Ù•É±…å}¥¹™¼¤ì(%½‰Í}™É½¹Ñ•¹‘}…‘‘}•Ù•¹Ñ}…±±‰…¬¡™É½¹Ñ•¹‘}•Ù•¹Ð°¹Õ±±ÁÑÈ¤ì(%‰±½œ¡1=}%9<°€‰mÕÉ¥½ÕÌ	¥Á•‘…±t9…Ñ¥Ù”Í•ÍÍ¥½¸½Ù•É±…ä±½…‘•ˆ¤ì(%É•ÑÕÉ¸ÑÉÕ”ì)ô()Ù½¥½‰Í}µ½‘Õ±•}Õ¹±½…¡Ù½¥¤)ì(%½‰Í}™É½¹Ñ•¹‘}É•µ½Ù•}•Ù•¹Ñ}…±±‰…¬¡™É½¹Ñ•¹‘}•Ù•¹Ð°¹Õ±±ÁÑÈ¤ì(%‰±½œ¡1=}%9<°€‰mÕÉ¥½ÕÌ	¥Á•‘…±t9…Ñ¥Ù”Í•ÍÍ¥½¸½Ù•É±…äÕ¹±½…‘•ˆ¤ì)ô()½¹ÍÐ¡…È€©½‰Í}µ½‘Õ±•}‘•ÍÉ¥ÁÑ¥½¸¡Ù½¥¤)ì(%É•ÑÕÉ¸€‰9…Ñ¥Ù”ÕÉ¥½ÕÌ	¥Á•‘…°Í•ÍÍ¥½¸½Ù•É±…äÝ¥Ñ ¥¹‘•Á•¹‘•¹Ð=	L…¹¥ÑÕ´Y•ÉÑ¥…°Ñ¥µ•È‰¥¹‘¥¹Ì¸ˆì)ô