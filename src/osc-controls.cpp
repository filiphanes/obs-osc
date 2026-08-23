#include "osc-plugin.h"

#include "osc-addresses.h"
#include "osc-net.h" /* osc_endpoint definition for the dispatch closure */
#include "osc-objects.h"
#include "osc-query.h"
#include "osc-server.h"
#include "osc-subscribe.h"
#include "osc-ui-task.h"
#include "osc-util.h"

#include <obs-frontend-api.h>
#include <obs-module.h>

#include <cmath>
#include <cstdlib>
#include <cstring>

/* ------------------------------------------------------------------ */
/* Sources                                                             */
/* ------------------------------------------------------------------ */

/* Returns an add-ref'd input source by name, or NULL. Release after use. */
static obs_source_t *get_input_by_name(const char *name)
{
	return osc_source_of_type(name, OBS_SOURCE_TYPE_INPUT);
}

/* Returns an add-ref'd scene source by name, or NULL. Release after use. */
static obs_source_t *get_scene_by_name(const char *name)
{
	return osc_source_of_type(name, OBS_SOURCE_TYPE_SCENE);
}

/* ------------------------------------------------------------------ */
/* Scenes, preview and transitions                                     */
/* ------------------------------------------------------------------ */

struct scene_index_ctx {
	int want = 0;
	int current = 0;
	std::string found_name;
};

static bool scene_index_enum(void *param, obs_source_t *scene)
{
	scene_index_ctx *ctx = (scene_index_ctx *)param;

	ctx->current++;
	if (ctx->current == ctx->want) {
		ctx->found_name = obs_source_get_name(scene);
		return false;
	}
	return true;
}

static void switch_program_scene(const char *name)
{
	obs_source_t *scene = get_scene_by_name(name);
	if (!scene) {
		blog(LOG_WARNING, "[obs-osc] program scene '%s' not found", name);
		return;
	}

	obs_frontend_set_current_scene(scene);
	obs_source_release(scene);
}

static void switch_program_scene_index(int index)
{
	if (index <= 0)
		return;

	scene_index_ctx ctx;
	ctx.want = index;
	obs_enum_scenes(scene_index_enum, &ctx);

	if (!ctx.found_name.empty())
		switch_program_scene(ctx.found_name.c_str());
	else
		blog(LOG_WARNING, "[obs-osc] no scene at index %d", index);
}

static void switch_preview_scene(const char *name)
{
	obs_source_t *scene = get_scene_by_name(name);
	if (!scene) {
		blog(LOG_WARNING, "[obs-osc] preview scene '%s' not found", name);
		return;
	}

	obs_frontend_set_current_preview_scene(scene);
	obs_source_release(scene);
}

struct transition_ctx {
	const char *want;
	obs_source_t *found;
};

static void transition_enum_cb(obs_source_t *transition, void *param)
{
	transition_ctx *ctx = (transition_ctx *)param;

	if (!ctx->found && strcmp(obs_source_get_name(transition), ctx->want) == 0)
		ctx->found = obs_source_get_ref(transition); /* survive the enumeration */
}

static void select_transition(const char *name)
{
	transition_ctx ctx = {name, nullptr};
	osc_enum_transitions(transition_enum_cb, &ctx);

	if (ctx.found) {
		obs_frontend_set_current_transition(ctx.found);
		obs_source_release(ctx.found);
	} else {
		blog(LOG_WARNING, "[obs-osc] transition '%s' not found", name);
	}
}

/* ------------------------------------------------------------------ */
/* Outputs: one engine for stream/record/replay/virtualcam             */
/* ------------------------------------------------------------------ */

/*
 * Shared implementation of "<output>/<sub>" commands. sub is one of
 * the route's registered sub-actions ("start"/"stop"/"toggle" plus
 * optional extras) or empty; anything unrecognized falls back to an
 * explicit bool argument.
 */
static void run_output_command(osc_addr::output_id id, const osc_message &msg, const char *sub)
{
	bool (*active)() = nullptr;
	void (*start)() = nullptr;
	void (*stop)() = nullptr;
	bool (*paused)() = nullptr;
	void (*set_paused)(bool) = nullptr;
	void (*save)() = nullptr;

	switch (id) {
	case osc_addr::output_id::stream:
		active = obs_frontend_streaming_active;
		start = obs_frontend_streaming_start;
		stop = obs_frontend_streaming_stop;
		break;
	case osc_addr::output_id::record:
		active = obs_frontend_recording_active;
		start = obs_frontend_recording_start;
		stop = obs_frontend_recording_stop;
		paused = obs_frontend_recording_paused;
		set_paused = [](bool on) { obs_frontend_recording_pause(on); };
		break;
	case osc_addr::output_id::replay:
		active = obs_frontend_replay_buffer_active;
		start = obs_frontend_replay_buffer_start;
		stop = obs_frontend_replay_buffer_stop;
		save = []() { obs_frontend_replay_buffer_save(); };
		break;
	case osc_addr::output_id::virtualcam:
		active = obs_frontend_virtualcam_active;
		start = obs_frontend_start_virtualcam;
		stop = obs_frontend_stop_virtualcam;
		break;
	}

	if (save && strcmp(sub, osc_addr::sub_save) == 0) {
		if (active())
			save();
		return;
	}

	if (set_paused && strcmp(sub, osc_addr::sub_pause) == 0) {
		set_paused(msg.arg_bool(0, !paused()));
		return;
	}

	const bool is_active = active();

	bool start_it;
	if (strcmp(sub, osc_addr::sub_start) == 0)
		start_it = true;
	else if (strcmp(sub, osc_addr::sub_stop) == 0)
		start_it = false;
	else if (strcmp(sub, osc_addr::sub_toggle) == 0)
		start_it = !is_active;
	else
		start_it = msg.arg_bool(0, !is_active);

	if (start_it && !is_active)
		start();
	else if (!start_it && is_active)
		stop();
}

/* ------------------------------------------------------------------ */
/* Sources: mute, volume, media playback                               */
/* ------------------------------------------------------------------ */

/* Mutes all inputs matched by glob pattern; value == nullptr toggles
 * each match individually. */
static void mute_inputs(const char *pattern, const bool *value)
{
	struct mute_set_ctx {
		const bool *value;
	} ctx = {value};

	osc_visit_sources(
		pattern, osc_source_kind::input,
		[](obs_source_t *source, void *p) {
			auto *c = (struct mute_set_ctx *)p;

			obs_source_set_muted(source, c->value ? *c->value : !obs_source_muted(source));
			return true;
		},
		&ctx);
}

static void set_input_volume(const char *name, float multiplier)
{
	obs_source_t *source = get_input_by_name(name);
	if (!source) {
		blog(LOG_WARNING, "[obs-osc] source '%s' not found", name);
		return;
	}

	obs_source_set_volume(source, multiplier);
	obs_source_release(source);
}

static void media_command(const char *name, const char *verb)
{
	obs_source_t *source = get_input_by_name(name);
	if (!source) {
		blog(LOG_WARNING, "[obs-osc] source '%s' not found", name);
		return;
	}

	for (size_t i = 0; i < osc_addr::media_verb_count; i++) {
		if (strcmp(verb, osc_addr::media_verbs[i].name) != 0)
			continue;

		switch (osc_addr::media_verbs[i].action) {
		case osc_addr::media_action::play:
			obs_source_media_play_pause(source, false);
			break;
		case osc_addr::media_action::pause:
			obs_source_media_play_pause(source, true);
			break;
		case osc_addr::media_action::toggle:
			obs_source_media_play_pause(source, obs_source_media_get_state(source) == OBS_MEDIA_STATE_PLAYING);
			break;
		case osc_addr::media_action::stop:
			obs_source_media_stop(source);
			break;
		case osc_addr::media_action::restart:
			obs_source_media_restart(source);
			break;
		case osc_addr::media_action::next:
			obs_source_media_next(source);
			break;
		case osc_addr::media_action::prev:
			obs_source_media_previous(source);
			break;
		}
		break;
	}

	obs_source_release(source);
}

/* Splits "<first>/<second>" at the last '/' of the segment after a
 * prefix; second keeps any further slashes, so names containing '/'
 * cannot be addressed (see README limitations). */
static bool split_two_segments(const char *rest, std::string *first, std::string *second)
{
	const char *slash = strchr(rest, '/');
	if (!slash || !*slash || !slash[1])
		return false;

	first->assign(rest, slash - rest);
	second->assign(slash + 1);
	return !first->empty() && !second->empty();
}

/* ------------------------------------------------------------------ */
/* Polls: zero-argument messages read state                            */
/* ------------------------------------------------------------------ */

/* A message without arguments is a read request in the style of other
 * OSC servers (e.g. the Behringer Wing): the current value(s) are sent
 * back to the requesting client, encoded exactly like the equivalent
 * feedback message. Replies go to the sender's own address and port,
 * regardless of the configured feedback target or feedback_enabled.
 * All of this runs on the UI thread. */

static int output_active(osc_addr::output_id id)
{
	switch (id) {
	case osc_addr::output_id::stream:
		return obs_frontend_streaming_active();
	case osc_addr::output_id::record:
		return obs_frontend_recording_active();
	case osc_addr::output_id::replay:
		return obs_frontend_replay_buffer_active();
	case osc_addr::output_id::virtualcam:
		return obs_frontend_virtualcam_active();
	}
	return 0;
}

/* Answers once per input matching the glob, addressed by exact name. */
static void poll_mute_inputs(const char *pattern, const osc_net::osc_endpoint &to)
{
	struct reply_ctx {
		const osc_net::osc_endpoint *to;
	} ctx = {&to};

	osc_visit_sources(
		pattern, osc_source_kind::input,
		[](obs_source_t *source, void *p) {
			auto *c = (struct reply_ctx *)p;

			g_server.reply(*c->to, osc_addr::mute_for(obs_source_get_name(source)).c_str(),
				       {osc_int(obs_source_muted(source) ? 1 : 0)});
			return true;
		},
		&ctx);
}

static void poll_media_state(const char *name, const osc_net::osc_endpoint &to)
{
	obs_source_t *source = get_input_by_name(name);
	if (!source) {
		blog(LOG_WARNING, "[obs-osc] poll: source '%s' not found", name);
		return;
	}

	const std::string address = osc_addr::media_state_for(name);
	g_server.reply(to, address.c_str(), {osc_int((int)obs_source_media_get_state(source))});
	obs_source_release(source);
}

/* Answers a read request for a known route. Returns false for unknown
 * or write-only addresses so they keep reaching the dispatcher.
 * Replies are coalesced into one #bundle datagram: polls of bare
 * routes answer dozens of addresses, and clients get an atomic
 * snapshot instead of a stream of datagrams. */
static bool poll_dispatch(const std::string &a, const osc_net::osc_endpoint &to)
{
	osc_reply_batch batch(to);
	if (a == osc_addr::studio) {
		g_server.reply(to, osc_addr::studio, {osc_int(obs_frontend_preview_program_mode_active())});
		return true;
	}
	if (a == osc_addr::program) {
		obs_source_t *scene = obs_frontend_get_current_scene();
		g_server.reply(to, osc_addr::program,
			       {osc_str(scene ? obs_source_get_name(scene) : "")});
		if (scene)
			obs_source_release(scene);
		return true;
	}
	if (a == osc_addr::preview) {
		obs_source_t *scene = obs_frontend_get_current_preview_scene();
		g_server.reply(to, osc_addr::preview,
			       {osc_str(scene ? obs_source_get_name(scene) : "")});
		if (scene)
			obs_source_release(scene);
		return true;
	}
	if (a == osc_addr::transition) {
		obs_source_t *transition = obs_frontend_get_current_transition();
		g_server.reply(to, osc_addr::transition,
			       {osc_str(transition ? obs_source_get_name(transition) : "")});
		if (transition)
			obs_source_release(transition);
		return true;
	}
	if (a == osc_addr::profile) {
		char *name = obs_frontend_get_current_profile();
		g_server.reply(to, osc_addr::profile, {osc_str(name ? name : "")});
		bfree(name);
		return true;
	}
	if (a == osc_addr::collection) {
		char *name = obs_frontend_get_current_scene_collection();
		g_server.reply(to, osc_addr::collection, {osc_str(name ? name : "")});
		bfree(name);
		return true;
	}
	if (a == osc_addr::record_paused) {
		g_server.reply(to, osc_addr::record_paused, {osc_int(obs_frontend_recording_paused())});
		return true;
	}

	for (size_t i = 0; i < osc_addr::output_count; i++) {
		const osc_addr::output_info &out = osc_addr::outputs[i];
		if (a == out.base) {
			g_server.reply(to, out.base, {osc_int(output_active(out.id))});
			return true;
		}
	}

	if (const char *pattern = osc_addr::tail(a, osc_addr::mute_prefix)) {
		poll_mute_inputs(pattern, to);
		return true;
	}
	if (const char *name = osc_addr::tail(a, osc_addr::volume_prefix)) {
		obs_source_t *source = get_input_by_name(name);
		if (!source) {
			blog(LOG_WARNING, "[obs-osc] poll: source '%s' not found", name);
			return true;
		}

		const std::string address = osc_addr::volume_for(name);
		g_server.reply(to, address.c_str(),
			       {osc_flt(osc_volume_position((float)obs_source_get_volume(source)))});
		obs_source_release(source);
		return true;
	}
	if (const char *name = osc_addr::tail(a, osc_addr::db_prefix)) {
		obs_source_t *source = get_input_by_name(name);
		if (!source) {
			blog(LOG_WARNING, "[obs-osc] poll: source '%s' not found", name);
			return true;
		}

		/* Silence has no dB value; report the UI floor instead. */
		const float mult = (float)obs_source_get_volume(source);
		const float db = mult > 0.0f ? 20.0f * log10f(mult) : -100.0f;
		const std::string address = osc_addr::prefixed(osc_addr::db_prefix, name);
		g_server.reply(to, address.c_str(), {osc_flt(db)});
		obs_source_release(source);
		return true;
	}
	if (const char *rest = osc_addr::tail(a, osc_addr::media_prefix)) {
		std::string name;
		std::string verb;
		if (!split_two_segments(rest, &name, &verb)) {
			/* "/media/<name>" without a verb polls the state. */
			poll_media_state(rest, to);
			return true;
		}
		if (verb == osc_addr::media_state_node) {
			poll_media_state(name.c_str(), to);
			return true;
		}
		/* Transport verbs act regardless of arguments. */
		return false;
	}

	/* Bare object-model routes read everything they cover. */
	if (a == osc_addr::visible) {
		osc_objects::poll_item_flag(to, "*", "*", osc_objects::item_prop::visible);
		return true;
	}
	if (a == osc_addr::locked) {
		osc_objects::poll_item_flag(to, "*", "*", osc_objects::item_prop::locked);
		return true;
	}
	if (a == osc_addr::order) {
		osc_objects::poll_item_order(to, "*", "*");
		return true;
	}
	if (a == osc_addr::filter) {
		osc_objects::poll_filter_enabled(to, "*", "*");
		return true;
	}
	if (a == osc_addr::active) {
		osc_objects::poll_input_state(to, "*", false);
		return true;
	}
	if (a == osc_addr::showing) {
		osc_objects::poll_input_state(to, "*", true);
		return true;
	}
	if (a == osc_addr::transform) {
		osc_objects::poll_transform(to, "*", "*", nullptr);
		return true;
	}

	if (const char *rest = osc_addr::tail(a, osc_addr::visible_prefix)) {
		std::string scene;
		std::string item;
		if (!split_two_segments(rest, &scene, &item))
			osc_objects::poll_item_flag(to, rest, "*", osc_objects::item_prop::visible);
		else
			osc_objects::poll_item_flag(to, scene.c_str(), item.c_str(), osc_objects::item_prop::visible);
		return true;
	}
	if (const char *rest = osc_addr::tail(a, osc_addr::locked_prefix)) {
		std::string scene;
		std::string item;
		if (!split_two_segments(rest, &scene, &item))
			osc_objects::poll_item_flag(to, rest, "*", osc_objects::item_prop::locked);
		else
			osc_objects::poll_item_flag(to, scene.c_str(), item.c_str(), osc_objects::item_prop::locked);
		return true;
	}
	if (const char *rest = osc_addr::tail(a, osc_addr::order_prefix)) {
		std::string scene;
		std::string item;
		if (!split_two_segments(rest, &scene, &item))
			osc_objects::poll_item_order(to, rest, "*");
		else
			osc_objects::poll_item_order(to, scene.c_str(), item.c_str());
		return true;
	}
	if (const char *rest = osc_addr::tail(a, osc_addr::filter_prefix)) {
		std::string source;
		std::string rest2;
		if (!split_two_segments(rest, &source, &rest2)) {
			osc_objects::poll_filter_enabled(to, rest, "*");
			return true;
		}

		std::string selector;
		std::string param;
		if (!split_two_segments(rest2.c_str(), &selector, &param)) {
			osc_objects::poll_filter_enabled(to, source.c_str(), rest2.c_str());
			return true;
		}

		/* "params" lists every scalar parameter; anything else
		 * reads one. */
		if (param == osc_addr::filter_params_node)
			osc_objects::poll_filter_params(to, source.c_str(), selector.c_str());
		else
			osc_objects::poll_filter_param(to, source.c_str(), selector.c_str(), param.c_str());
		return true;
	}
	if (const char *rest = osc_addr::tail(a, osc_addr::transform_prefix)) {
		std::string scene;
		std::string remainder;
		std::string item;
		std::string field_name;
		if (!split_two_segments(rest, &scene, &remainder)) {
			osc_objects::poll_transform(to, rest, "*", nullptr);
			return true;
		}
		if (!split_two_segments(remainder.c_str(), &item, &field_name)) {
			osc_objects::poll_transform(to, scene.c_str(), remainder.c_str(), nullptr);
			return true;
		}

		osc_objects::poll_transform(to, scene.c_str(), item.c_str(), field_name.c_str());
		return true;
	}
	if (const char *pattern = osc_addr::tail(a, osc_addr::active_prefix)) {
		osc_objects::poll_input_state(to, pattern, false);
		return true;
	}
	if (const char *pattern = osc_addr::tail(a, osc_addr::showing_prefix)) {
		osc_objects::poll_input_state(to, pattern, true);
		return true;
	}

	return false;
}

/* True when the first argument is the explicit "-1" toggle sentinel
 * (int32 or float32). Routes whose no-argument form is now a poll use
 * it to keep one-button toggle workflows alive. */
static bool arg_is_toggle(const osc_message &msg)
{
	if (msg.args.empty())
		return false;

	const osc_argument &arg = msg.args[0];
	if (arg.type == 'i')
		return arg.i == -1;
	if (arg.type == 'f')
		return arg.f == -1.0f;
	return false;
}

/* ------------------------------------------------------------------ */
/* Dispatcher                                                          */
/* ------------------------------------------------------------------ */

void osc_dispatch_command(const osc_message &msg, const osc_net::osc_endpoint &reply_to)
{
	const std::string &a = msg.address;

	if (!osc_addr::has_prefix(a, osc_addr::root)) {
		blog(LOG_DEBUG, "[obs-osc] ignoring foreign address '%s'", a.c_str());
		return;
	}

	/* Subscription management first: it must also catch the no-
	 * argument /unsubscribe, which is not a poll route. */
	if (a == osc_addr::subscribe || a == osc_addr::unsubscribe) {
		osc_handle_subscription(msg, reply_to, a == osc_addr::unsubscribe);
		return;
	}

	/* No arguments means "read": answer with the current value(s).
	 * Every command branch below therefore sees at least one argument. */
	if (msg.args.empty() && poll_dispatch(a, reply_to))
		return;

	if (a == osc_addr::studio) {
		/* -1 toggles, anything else sets. The poll case was handled
		 * above for empty-argument messages. */
		bool value = msg.arg_bool(0, false);
		if (arg_is_toggle(msg))
			value = !obs_frontend_preview_program_mode_active();
		obs_frontend_set_preview_program_mode(value);
		return;
	}

	if (a == osc_addr::transition) {
		/* With an argument this is the legacy trigger form; without
		 * one it polled above. "/transition/go" is equivalent. */
		obs_frontend_preview_program_trigger_transition();
		return;
	}
	if (const char *name = osc_addr::tail(a, osc_addr::transition_prefix)) {
		if (strcmp(name, osc_addr::transition_go) == 0)
			obs_frontend_preview_program_trigger_transition();
		else
			select_transition(name);
		return;
	}

	if (const char *index = osc_addr::tail(a, osc_addr::program_index_prefix)) {
		switch_program_scene_index(atoi(index));
		return;
	}
	if (const char *name = osc_addr::tail(a, osc_addr::program_prefix)) {
		switch_program_scene(name);
		return;
	}
	if (const char *name = osc_addr::tail(a, osc_addr::preview_prefix)) {
		switch_preview_scene(name);
		return;
	}

	for (size_t i = 0; i < osc_addr::output_count; i++) {
		const osc_addr::output_info &out = osc_addr::outputs[i];
		if (const char *sub = osc_addr::sub_action(a, out.base)) {
			run_output_command(out.id, msg, sub);
			return;
		}
	}

	if (a == osc_addr::screenshot) {
		obs_frontend_take_screenshot();
		return;
	}

	if (const char *pattern = osc_addr::tail(a, osc_addr::mute_prefix)) {
		/* -1 toggles matching inputs, 0/1 sets them; polls never get
		 * here because empty-argument messages read above. */
		if (arg_is_toggle(msg)) {
			mute_inputs(pattern, nullptr);
		} else {
			const bool value = msg.arg_bool(0, false);
			mute_inputs(pattern, &value);
		}
		return;
	}

	if (const char *name = osc_addr::tail(a, osc_addr::volume_prefix)) {
		set_input_volume(name, osc_volume_multiplier(msg.arg_float(0, -1.0f)));
		return;
	}

	if (const char *name = osc_addr::tail(a, osc_addr::db_prefix)) {
		set_input_volume(name, powf(10.0f, msg.arg_float(0, 0.0f) / 20.0f));
		return;
	}

	if (const char *rest = osc_addr::tail(a, osc_addr::media_prefix)) {
		std::string name;
		std::string verb;
		if (split_two_segments(rest, &name, &verb))
			media_command(name.c_str(), verb.c_str());
		return;
	}

	if (const char *rest = osc_addr::tail(a, osc_addr::visible_prefix)) {
		std::string scene;
		std::string item;
		if (split_two_segments(rest, &scene, &item)) {
			if (arg_is_toggle(msg)) {
				osc_objects::set_item_flag(scene.c_str(), item.c_str(),
							   osc_objects::item_prop::visible, nullptr);
			} else {
				const bool value = msg.arg_bool(0, false);
				osc_objects::set_item_flag(scene.c_str(), item.c_str(),
							   osc_objects::item_prop::visible, &value);
			}
		}
		return;
	}

	if (const char *rest = osc_addr::tail(a, osc_addr::locked_prefix)) {
		std::string scene;
		std::string item;
		if (split_two_segments(rest, &scene, &item)) {
			if (arg_is_toggle(msg)) {
				osc_objects::set_item_flag(scene.c_str(), item.c_str(),
							   osc_objects::item_prop::locked, nullptr);
			} else {
				const bool value = msg.arg_bool(0, false);
				osc_objects::set_item_flag(scene.c_str(), item.c_str(),
							   osc_objects::item_prop::locked, &value);
			}
		}
		return;
	}

	if (const char *rest = osc_addr::tail(a, osc_addr::order_prefix)) {
		std::string scene;
		std::string item;
		if (split_two_segments(rest, &scene, &item))
			osc_objects::set_item_order(scene.c_str(), item.c_str(), msg.arg_int(0, -1));
		return;
	}

	if (const char *rest = osc_addr::tail(a, osc_addr::filter_prefix)) {
		std::string source;
		std::string rest2;
		if (!split_two_segments(rest, &source, &rest2))
			return;

		/* Three segments select one parameter of the filter; two
		 * segments toggle/set its enabled state. */
		std::string selector;
		std::string param;
		if (split_two_segments(rest2.c_str(), &selector, &param)) {
			osc_objects::write_filter_param(msg, source.c_str(), selector.c_str(), param.c_str());
			return;
		}

		if (arg_is_toggle(msg)) {
			osc_objects::set_filter_enabled(source.c_str(), rest2.c_str(), nullptr);
		} else {
			const bool value = msg.arg_bool(0, false);
			osc_objects::set_filter_enabled(source.c_str(), rest2.c_str(), &value);
		}
		return;
	}

	if (const char *rest = osc_addr::tail(a, osc_addr::transform_prefix)) {
		std::string scene;
		std::string remainder;
		std::string item;
		std::string field_name;
		if (!split_two_segments(rest, &scene, &remainder))
			return;
		if (!split_two_segments(remainder.c_str(), &item, &field_name))
			return;

		osc_objects::write_transform(scene.c_str(), item.c_str(), field_name.c_str(),
					     msg.arg_float(0, 0.0f));
		return;
	}

	if (const char *name = osc_addr::tail(a, osc_addr::hotkey_prefix)) {
		osc_objects::trigger_hotkey(name);
		return;
	}

	if (const char *name = osc_addr::tail(a, osc_addr::profile_prefix)) {
		obs_frontend_set_current_profile(name);
		return;
	}

	if (const char *name = osc_addr::tail(a, osc_addr::collection_prefix)) {
		obs_frontend_set_current_scene_collection(name);
		return;
	}

	blog(LOG_DEBUG, "[obs-osc] unhandled address '%s'", a.c_str());
}
