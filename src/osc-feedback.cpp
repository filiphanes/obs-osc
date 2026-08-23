#include "osc-plugin.h"

#include "osc-addresses.h"
#include "osc-server.h"
#include "osc-subscribe.h"
#include "osc-util.h"
#include "osc-util.h"

#include <obs-frontend-api.h>
#include <obs-module.h>

#include <chrono>
#include <mutex>
#include <unordered_map>
#include <vector>

/* ------------------------------------------------------------------ */
/* Outgoing feedback                                                   */
/* ------------------------------------------------------------------ */

void osc_feedback_send(const char *address, std::initializer_list<osc_argument> args)
{
	if (!g_config.feedback_enabled)
		return;

	std::vector<uint8_t> buf;
	osc_build_message(&buf, address, args);

	/* Per-topic subscribers (UDP patterns) and OSCQuery LISTEN peers
	 * get the event only when it matches what they asked for. */
	osc_publish(address, buf.data(), buf.size());

	/* Legacy targets: a fixed feedback host always receives every
	 * event; the implicit reply-to-last-controller broadcast stays
	 * only while nobody subscribes selectively, which would defeat
	 * per-topic filtering. */
	g_server.send_fixed(buf.data(), buf.size());
	if (!osc_has_subscribers())
		g_server.send_last(buf.data(), buf.size());
}

/* Consumes a reference: releases source after sending its name. */
static void send_source_name(const char *address, obs_source_t *source)
{
	if (!source)
		return;

	osc_feedback_send(address, {osc_str(obs_source_get_name(source))});
	obs_source_release(source);
}

/* ------------------------------------------------------------------ */
/* Source signal hooks                                                 */
/* ------------------------------------------------------------------ */

static void mute_signal_cb(void *param, calldata_t *cd)
{
	auto *source = (obs_source_t *)param;
	bool muted = false;

	UNUSED_PARAMETER(cd);
	calldata_get_bool(cd, "muted", &muted);

	const std::string address = osc_addr::mute_for(obs_source_get_name(source));
	osc_feedback_send(address.c_str(), {osc_int(muted ? 1 : 0)});
}

/* Rate limiter shared by chatty signals: true when 'key' was already
 * sent within the last 'ms' milliseconds (records this send). */
static bool throttled(const std::string &key, long ms)
{
	static std::mutex mutex;
	static std::unordered_map<std::string, std::chrono::steady_clock::time_point> last_sent;

	std::lock_guard<std::mutex> lock(mutex);
	const auto now = std::chrono::steady_clock::now();
	const auto it = last_sent.find(key);

	if (it != last_sent.end() && now - it->second < std::chrono::milliseconds(ms))
		return true;

	last_sent[key] = now;
	return false;
}

static void volume_signal_cb(void *param, calldata_t *cd)
{
	auto *source = (obs_source_t *)param;
	double volume = 0.0;

	if (!calldata_get_float(cd, "volume", &volume))
		return;

	const char *name = obs_source_get_name(source);
	if (!name || throttled(name, 33))
		return;

	const std::string address = osc_addr::volume_for(name);
	osc_feedback_send(address.c_str(), {osc_flt(osc_volume_position((float)volume))});
}

static void media_signal_cb(void *param, calldata_t *cd)
{
	auto *source = (obs_source_t *)param;

	UNUSED_PARAMETER(cd);

	const int state = (int)obs_source_media_get_state(source);
	const std::string address = osc_addr::media_state_for(obs_source_get_name(source));
	osc_feedback_send(address.c_str(), {osc_int(state)});
}

/* ------------------------------------------------------------------ */
/* Tally hooks: activate/deactivate/show/hide carry no calldata        */
/* payload, so each callback encodes the state itself.                 */
/* ------------------------------------------------------------------ */

static void send_bool_feedback(const char *address, int value)
{
	osc_feedback_send(address, {osc_int(value)});
}

static void activate_signal_cb(void *param, calldata_t *cd)
{
	auto *source = (obs_source_t *)param;
	UNUSED_PARAMETER(cd);

	const std::string address = osc_addr::prefixed(osc_addr::active_prefix, obs_source_get_name(source));
	send_bool_feedback(address.c_str(), 1);
}

static void deactivate_signal_cb(void *param, calldata_t *cd)
{
	auto *source = (obs_source_t *)param;
	UNUSED_PARAMETER(cd);

	const std::string address = osc_addr::prefixed(osc_addr::active_prefix, obs_source_get_name(source));
	send_bool_feedback(address.c_str(), 0);
}

static void show_signal_cb(void *param, calldata_t *cd)
{
	auto *source = (obs_source_t *)param;
	UNUSED_PARAMETER(cd);

	const std::string address = osc_addr::prefixed(osc_addr::showing_prefix, obs_source_get_name(source));
	send_bool_feedback(address.c_str(), 1);
}

static void hide_signal_cb(void *param, calldata_t *cd)
{
	auto *source = (obs_source_t *)param;
	UNUSED_PARAMETER(cd);

	const std::string address = osc_addr::prefixed(osc_addr::showing_prefix, obs_source_get_name(source));
	send_bool_feedback(address.c_str(), 0);
}

/* ------------------------------------------------------------------ */
/* Scene item hooks: visibility, lock and order                        */
/* ------------------------------------------------------------------ */

static void item_visible_signal_cb(void *param, calldata_t *cd)
{
	auto *scene_source = (obs_source_t *)param;
	obs_sceneitem_t *item = nullptr;
	bool visible = false;

	if (!calldata_get_ptr(cd, "item", &item))
		return;
	calldata_get_bool(cd, "visible", &visible);

	obs_source_t *item_source = item ? obs_sceneitem_get_source(item) : nullptr;
	if (!item_source)
		return;

	const std::string address =
		osc_addr::visible_for(obs_source_get_name(scene_source), obs_source_get_name(item_source));
	send_bool_feedback(address.c_str(), visible ? 1 : 0);
}

static void item_locked_signal_cb(void *param, calldata_t *cd)
{
	auto *scene_source = (obs_source_t *)param;
	obs_sceneitem_t *item = nullptr;
	bool locked = false;

	if (!calldata_get_ptr(cd, "item", &item))
		return;
	calldata_get_bool(cd, "locked", &locked);

	obs_source_t *item_source = item ? obs_sceneitem_get_source(item) : nullptr;
	if (!item_source)
		return;

	const std::string address =
		osc_addr::locked_for(obs_source_get_name(scene_source), obs_source_get_name(item_source));
	send_bool_feedback(address.c_str(), locked ? 1 : 0);
}

/* Reports every scalar transform field of one item; fires after our
 * own writes too, echoing the applied state to the controller. */
static void send_item_transform_feedback(const char *scene_name, const char *item_name,
					 obs_sceneitem_t *item)
{
	for (size_t i = 0; i < osc_transform_field_count; i++) {
		const osc_transform_field &f = osc_transform_fields[i];

		const std::string address = osc_addr::transform_for(scene_name, item_name, f.name);
		osc_feedback_send(address.c_str(), {osc_flt(f.get(item))});
	}
}

static void item_transform_signal_cb(void *param, calldata_t *cd)
{
	auto *scene_source = (obs_source_t *)param;
	obs_sceneitem_t *item = nullptr;

	if (!calldata_get_ptr(cd, "item", &item))
		return;

	obs_source_t *item_source = item ? obs_sceneitem_get_source(item) : nullptr;
	if (!item_source || !obs_source_get_name(item_source))
		return;

	/* UI drags emit a burst of these; throttle per scene and item. */
	const char *scene_name = obs_source_get_name(scene_source);
	const std::string key = std::string(scene_name ? scene_name : "") + "/" + obs_source_get_name(item_source);
	if (throttled(key, 100))
		return;

	send_item_transform_feedback(scene_name, obs_source_get_name(item_source), item);
}

/* "reorder" carries no item reference, so every item's position is
 * reported. UI drags emit this repeatedly; the per-scene throttle
 * keeps the burst bounded while controllers still converge quickly. */
static bool reorder_report_cb(obs_scene_t *scene, obs_sceneitem_t *item, void *param)
{
	UNUSED_PARAMETER(scene);
	auto *scene_name = (const char *)param;

	obs_source_t *item_source = obs_sceneitem_get_source(item);
	if (!item_source)
		return true;

	const std::string address = osc_addr::order_for(scene_name, obs_source_get_name(item_source));
	send_bool_feedback(address.c_str(), obs_sceneitem_get_order_position(item));
	return true;
}

static void scene_reorder_signal_cb(void *param, calldata_t *cd)
{
	auto *scene_source = (obs_source_t *)param;
	UNUSED_PARAMETER(cd);

	const char *name = obs_source_get_name(scene_source);
	if (!name || throttled(name, 100))
		return;

	obs_scene_enum_items(obs_scene_from_source(scene_source), reorder_report_cb, (void *)name);
}

/* ------------------------------------------------------------------ */
/* Filter enable tracking                                              */
/*                                                                    */
/* Filters are sources whose "enable" signal carries their state.      */
/* They come and go with the parent's filter chain, so each hooked     */
/* filter is kept add-ref'd and tracked until it is removed or its     */
/* parent goes away. The enable callback receives the parent as param  */
/* and reads the filter from the calldata, so no lookup happens there. */
/* ------------------------------------------------------------------ */

struct filter_hook_entry {
	obs_source_t *parent; /* borrowed */
	obs_source_t *filter; /* add-ref'd while hooked */
};

static std::vector<filter_hook_entry> s_filter_hooks;
static std::mutex s_filter_mutex;

static void filter_enable_signal_cb(void *param, calldata_t *cd)
{
	auto *parent = (obs_source_t *)param;
	obs_source_t *filter = nullptr;
	bool enabled = false;

	if (!calldata_get_ptr(cd, "source", &filter))
		return;
	calldata_get_bool(cd, "enabled", &enabled);

	const std::string address = osc_addr::filter_for(obs_source_get_name(parent), obs_source_get_name(filter));
	send_bool_feedback(address.c_str(), enabled ? 1 : 0);
}

static void hook_filter(obs_source_t *parent, obs_source_t *filter)
{
	if (!filter)
		return;

	std::lock_guard<std::mutex> lock(s_filter_mutex);

	for (const filter_hook_entry &entry : s_filter_hooks) {
		if (entry.parent == parent && entry.filter == filter)
			return;
	}

	if (!obs_source_get_ref(filter))
		return;

	signal_handler_connect(obs_source_get_signal_handler(filter), "enable", filter_enable_signal_cb, parent);
	s_filter_hooks.push_back({parent, filter});
}

static void drop_hook_at(size_t index)
{
	filter_hook_entry &entry = s_filter_hooks[index];

	signal_handler_disconnect(obs_source_get_signal_handler(entry.filter), "enable", filter_enable_signal_cb,
				  entry.parent);
	obs_source_release(entry.filter);
	s_filter_hooks.erase(s_filter_hooks.begin() + index);
}

static void unhook_filter(obs_source_t *parent, obs_source_t *filter)
{
	std::lock_guard<std::mutex> lock(s_filter_mutex);

	for (size_t i = 0; i < s_filter_hooks.size(); i++) {
		if (s_filter_hooks[i].parent == parent && s_filter_hooks[i].filter == filter) {
			drop_hook_at(i);
			return;
		}
	}
}

/* Drops every filter hook of one parent; runs when the parent is
 * destroyed and on plugin shutdown. */
static void unhook_filters_of_parent(obs_source_t *parent)
{
	std::lock_guard<std::mutex> lock(s_filter_mutex);

	for (size_t i = s_filter_hooks.size(); i > 0; i--) {
		if (s_filter_hooks[i - 1].parent == parent)
			drop_hook_at(i - 1);
	}
}

static void filter_add_signal_cb(void *param, calldata_t *cd)
{
	auto *parent = (obs_source_t *)param;
	obs_source_t *filter = nullptr;

	UNUSED_PARAMETER(param);
	if (calldata_get_ptr(cd, "filter", &filter))
		hook_filter(parent, filter);
}

static void filter_remove_signal_cb(void *param, calldata_t *cd)
{
	auto *parent = (obs_source_t *)param;
	obs_source_t *filter = nullptr;

	UNUSED_PARAMETER(param);
	if (calldata_get_ptr(cd, "filter", &filter))
		unhook_filter(parent, filter);
}

/* Hooks the filters an input or scene already carries; runs from the
 * initial enumeration and whenever such a source is created. */
static void hook_existing_filters(obs_source_t *source)
{
	obs_source_enum_filters(
		source,
		[](obs_source_t *parent, obs_source_t *filter, void *param) {
			UNUSED_PARAMETER(param);
			hook_filter(parent, filter);
		},
		nullptr);
}

static inline bool osc_is_scene(obs_source_t *source)
{
	return obs_source_get_type(source) == OBS_SOURCE_TYPE_SCENE;
}

/* One table per source kind drives both the connect and disconnect
 * passes so the lists can never drift apart. */
struct signal_hook {
	const char *signal;
	signal_callback_t callback;
};

static const signal_hook input_hooks[] = {
	{"mute", mute_signal_cb},            {"volume", volume_signal_cb},
	{"media_play", media_signal_cb},      {"media_pause", media_signal_cb},
	{"media_restart", media_signal_cb},   {"media_stopped", media_signal_cb},
	{"media_next", media_signal_cb},      {"media_previous", media_signal_cb},
	{"activate", activate_signal_cb},     {"deactivate", deactivate_signal_cb},
	{"show", show_signal_cb},             {"hide", hide_signal_cb},
	{"filter_add", filter_add_signal_cb}, {"filter_remove", filter_remove_signal_cb},
};

/* Scenes report item state changes; positions arrive via "reorder",
 * which carries no item, so all items of the scene are reported. */
static const signal_hook scene_hooks[] = {
	{"item_visible", item_visible_signal_cb},
	{"item_locked", item_locked_signal_cb},
	{"reorder", scene_reorder_signal_cb},
	{"item_transform", item_transform_signal_cb},
};

#define OSC_HOOK_COUNT(table) (sizeof(table) / sizeof((table)[0]))

static void connect_hook_table(obs_source_t *source, const signal_hook *hooks, size_t count, bool connect)
{
	signal_handler_t *sh = obs_source_get_signal_handler(source);

	for (size_t i = 0; i < count; i++) {
		if (connect)
			signal_handler_connect(sh, hooks[i].signal, hooks[i].callback, source);
		else
			signal_handler_disconnect(sh, hooks[i].signal, hooks[i].callback, source);
	}
}

static void change_hook_connections(obs_source_t *source, bool connect)
{
	if (osc_is_input(source)) {
		connect_hook_table(source, input_hooks, OSC_HOOK_COUNT(input_hooks), connect);
	} else if (osc_is_scene(source)) {
		connect_hook_table(source, scene_hooks, OSC_HOOK_COUNT(scene_hooks), connect);
	} else {
		return;
	}

	/* Filter enable-hooks follow the parent's hooks: symmetric by
	 * construction, so no caller can forget half of the pair. */
	if (connect)
		hook_existing_filters(source);
	else
		unhook_filters_of_parent(source);
}

static bool enum_connect_cb(void *param, obs_source_t *source)
{
	UNUSED_PARAMETER(param);

	change_hook_connections(source, true);
	return true;
}

static bool enum_disconnect_cb(void *param, obs_source_t *source)
{
	UNUSED_PARAMETER(param);

	change_hook_connections(source, false);
	return true;
}

static void source_create_cb(void *param, calldata_t *cd)
{
	obs_source_t *source = nullptr;

	UNUSED_PARAMETER(param);
	if (!calldata_get_ptr(cd, "source", &source))
		return;

	change_hook_connections(source, true);
}

static void source_destroy_cb(void *param, calldata_t *cd)
{
	obs_source_t *source = nullptr;

	UNUSED_PARAMETER(param);
	if (!calldata_get_ptr(cd, "source", &source))
		return;

	/* Also drops the filter enable-hooks of this source. */
	change_hook_connections(source, false);
}

/* ------------------------------------------------------------------ */
/* Frontend event hooks                                                */
/* ------------------------------------------------------------------ */

static void frontend_event_cb(enum obs_frontend_event event, void *param)
{
	UNUSED_PARAMETER(param);

	switch (event) {
	case OBS_FRONTEND_EVENT_SCENE_CHANGED:
		send_source_name(osc_addr::program, obs_frontend_get_current_scene());
		break;
	case OBS_FRONTEND_EVENT_PREVIEW_SCENE_CHANGED:
		send_source_name(osc_addr::preview, obs_frontend_get_current_preview_scene());
		break;
	case OBS_FRONTEND_EVENT_STUDIO_MODE_ENABLED:
		osc_feedback_send(osc_addr::studio, {osc_int(1)});
		break;
	case OBS_FRONTEND_EVENT_STUDIO_MODE_DISABLED:
		osc_feedback_send(osc_addr::studio, {osc_int(0)});
		break;
	case OBS_FRONTEND_EVENT_TRANSITION_CHANGED:
		send_source_name(osc_addr::transition, obs_frontend_get_current_transition());
		break;
	case OBS_FRONTEND_EVENT_STREAMING_STARTED:
		osc_feedback_send(osc_addr::stream, {osc_int(1)});
		break;
	case OBS_FRONTEND_EVENT_STREAMING_STOPPED:
		osc_feedback_send(osc_addr::stream, {osc_int(0)});
		break;
	case OBS_FRONTEND_EVENT_RECORDING_STARTED:
		osc_feedback_send(osc_addr::record, {osc_int(1)});
		break;
	case OBS_FRONTEND_EVENT_RECORDING_STOPPED:
		osc_feedback_send(osc_addr::record, {osc_int(0)});
		break;
	case OBS_FRONTEND_EVENT_RECORDING_PAUSED:
		osc_feedback_send(osc_addr::record_paused, {osc_int(1)});
		break;
	case OBS_FRONTEND_EVENT_RECORDING_UNPAUSED:
		osc_feedback_send(osc_addr::record_paused, {osc_int(0)});
		break;
	case OBS_FRONTEND_EVENT_REPLAY_BUFFER_STARTED:
		osc_feedback_send(osc_addr::replay, {osc_int(1)});
		break;
	case OBS_FRONTEND_EVENT_REPLAY_BUFFER_STOPPED:
		osc_feedback_send(osc_addr::replay, {osc_int(0)});
		break;
	case OBS_FRONTEND_EVENT_REPLAY_BUFFER_SAVED:
		osc_feedback_send(osc_addr::replay_saved, {osc_int(1)});
		break;
	case OBS_FRONTEND_EVENT_VIRTUALCAM_STARTED:
		osc_feedback_send(osc_addr::virtualcam, {osc_int(1)});
		break;
	case OBS_FRONTEND_EVENT_VIRTUALCAM_STOPPED:
		osc_feedback_send(osc_addr::virtualcam, {osc_int(0)});
		break;
	case OBS_FRONTEND_EVENT_PROFILE_CHANGED: {
		char *name = obs_frontend_get_current_profile();
		osc_feedback_send(osc_addr::profile, {osc_str(name ? name : "")});
		bfree(name);
		break;
	}
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED: {
		char *name = obs_frontend_get_current_scene_collection();
		osc_feedback_send(osc_addr::collection, {osc_str(name ? name : "")});
		bfree(name);
		break;
	}
	default:
		break;
	}
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

void osc_feedback_init(void)
{
	obs_enum_sources(enum_connect_cb, nullptr);

	signal_handler_t *sh = obs_get_signal_handler();
	signal_handler_connect(sh, "source_create", source_create_cb, nullptr);
	signal_handler_connect(sh, "source_destroy", source_destroy_cb, nullptr);

	obs_frontend_add_event_callback(frontend_event_cb, nullptr);
}

void osc_feedback_shutdown(void)
{
	obs_frontend_remove_event_callback(frontend_event_cb, nullptr);

	signal_handler_t *sh = obs_get_signal_handler();
	signal_handler_disconnect(sh, "source_create", source_create_cb, nullptr);
	signal_handler_disconnect(sh, "source_destroy", source_destroy_cb, nullptr);

	obs_enum_sources(enum_disconnect_cb, nullptr);
}
