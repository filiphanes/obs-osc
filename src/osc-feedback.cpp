#include "osc-plugin.h"

#include "osc-addresses.h"
#include "osc-server.h"
#include "osc-subscribe.h"
#include "osc-util.h"

#include <obs-frontend-api.h>
#include <obs-module.h>

#include <chrono>
#include <mutex>
#include <unordered_map>

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

static bool volume_throttled(const char *name)
{
	static std::mutex mutex;
	static std::unordered_map<std::string, std::chrono::steady_clock::time_point> last_sent;

	std::lock_guard<std::mutex> lock(mutex);
	const auto now = std::chrono::steady_clock::now();
	const auto it = last_sent.find(name);

	if (it != last_sent.end() && now - it->second < std::chrono::milliseconds(33))
		return true;

	last_sent[name] = now;
	return false;
}

static void volume_signal_cb(void *param, calldata_t *cd)
{
	auto *source = (obs_source_t *)param;
	double volume = 0.0;

	if (!calldata_get_float(cd, "volume", &volume))
		return;

	const char *name = obs_source_get_name(source);
	if (!name || volume_throttled(name))
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

/* One table drives both the connect and disconnect passes so the two
 * lists can never drift apart. */
struct signal_hook {
	const char *signal;
	signal_callback_t callback;
};

static const signal_hook source_hooks[] = {
	{"mute", mute_signal_cb},         {"volume", volume_signal_cb},
	{"media_play", media_signal_cb},  {"media_pause", media_signal_cb},
	{"media_restart", media_signal_cb}, {"media_stopped", media_signal_cb},
	{"media_next", media_signal_cb},  {"media_previous", media_signal_cb},
};

static void change_hook_connections(obs_source_t *source, bool connect)
{
	signal_handler_t *sh = obs_source_get_signal_handler(source);

	for (const signal_hook &hook : source_hooks) {
		if (connect)
			signal_handler_connect(sh, hook.signal, hook.callback, source);
		else
			signal_handler_disconnect(sh, hook.signal, hook.callback, source);
	}
}

static bool enum_connect_cb(void *param, obs_source_t *source)
{
	UNUSED_PARAMETER(param);

	if (osc_is_input(source))
		change_hook_connections(source, true);
	return true;
}

static bool enum_disconnect_cb(void *param, obs_source_t *source)
{
	UNUSED_PARAMETER(param);

	if (osc_is_input(source))
		change_hook_connections(source, false);
	return true;
}

static void source_create_cb(void *param, calldata_t *cd)
{
	obs_source_t *source = nullptr;

	UNUSED_PARAMETER(param);
	if (!calldata_get_ptr(cd, "source", &source))
		return;

	if (osc_is_input(source))
		change_hook_connections(source, true);
}

static void source_destroy_cb(void *param, calldata_t *cd)
{
	obs_source_t *source = nullptr;

	UNUSED_PARAMETER(param);
	if (!calldata_get_ptr(cd, "source", &source))
		return;

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
