#include "osc-plugin.h"

#include "osc-addresses.h"
#include "osc-net.h"
#include "osc-server.h"
#include "osc-subscribe.h"
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
	struct enum_ctx {
		const char *pattern;
		const bool *value;
	} ctx = {pattern, value};

	obs_enum_sources(
		[](void *p, obs_source_t *source) {
			const struct enum_ctx *c = (const struct enum_ctx *)p;

			if (osc_is_input(source) && osc_glob_match(c->pattern, obs_source_get_name(source))) {
				const bool target = c->value ? *c->value : !obs_source_muted(source);
				obs_source_set_muted(source, target);
			}
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

/* Splits "<name>/<verb>" at the last '/' of the segment after the prefix. */
static bool split_name_verb(const char *rest, std::string *name, std::string *verb)
{
	const char *slash = strchr(rest, '/');
	if (!slash || !*slash || !slash[1])
		return false;

	name->assign(rest, slash - rest);
	verb->assign(slash + 1);
	return !name->empty() && !verb->empty();
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

static void poll_reply(const osc_net::osc_endpoint &to, const char *address,
		       std::initializer_list<osc_argument> args)
{
	std::vector<uint8_t> buf;
	osc_build_message(&buf, address, args);
	g_server.send_to(buf.data(), buf.size(), to);
}

/* Replies with a source's name; empty name when there is no source
 * (e.g. preview scene while studio mode is off). Consumes the ref. */
static void poll_reply_source_name(const osc_net::osc_endpoint &to, const char *address,
				   obs_source_t *source)
{
	poll_reply(to, address, {osc_str(source ? obs_source_get_name(source) : "")});
	if (source)
		obs_source_release(source);
}

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

/* ------------------------------------------------------------------ */
/* Scene items and filters: the object model routes                    */
/*                                                                    */
/* Two-level addresses "<prefix><parent>/<child>": scenes hold items,  */
/* sources hold filters. Both segments accept '*'/'?' globs; a bare    */
/* route or single-segment address polls every match (see README).     */
/* ------------------------------------------------------------------ */

/* Selects which boolean property the scene-item routes operate on. */
enum class item_prop { visible, locked };

/* True when the segment addresses an element by array index instead
 * of a name: "2" picks the third filter or scene item, while "Mic"
 * stays a name/glob. Sources named purely digits cannot be addressed
 * as names on these routes. */
static bool is_index_segment(const char *segment)
{
	if (!*segment)
		return false;

	for (const char *c = segment; *c; c++) {
		if (*c < '0' || *c > '9')
			return false;
	}
	return true;
}

/* Filter selectors match either the chain position or the name. */
static bool filter_selector_matches(const char *selector, const char *filter_name, size_t index)
{
	return is_index_segment(selector) ? (size_t)atoi(selector) == index : osc_glob_match(selector, filter_name);
}

static const osc_transform_field *find_transform_field(const char *name)
{
	for (size_t i = 0; i < osc_transform_field_count; i++) {
		if (strcmp(osc_transform_fields[i].name, name) == 0)
			return &osc_transform_fields[i];
	}
	return nullptr;
}

/* Applies a visibility/lock change to every item of every scene whose
 * name matches the scene pattern. value == nullptr toggles each match
 * individually. */
static void apply_scene_item_bool(const char *scene_pattern, const char *item_pattern, const bool *value,
				  item_prop prop)
{
	struct scene_ctx {
		const char *scene_pattern;
		const char *item_pattern;
		const bool *value;
		item_prop prop;
	} ctx = {scene_pattern, item_pattern, value, prop};

	obs_enum_scenes(
		[](void *p, obs_source_t *scene_source) {
			auto *c = (struct scene_ctx *)p;

			if (!osc_glob_match(c->scene_pattern, obs_source_get_name(scene_source)))
				return true;

			struct item_ctx {
				const char *item_pattern;
				const bool *value;
				item_prop prop;
			} items = {c->item_pattern, c->value, c->prop};

			obs_scene_enum_items(
				obs_scene_from_source(scene_source),
				[](obs_scene_t *scene, obs_sceneitem_t *item, void *ip) {
					UNUSED_PARAMETER(scene);
					auto *ic = (struct item_ctx *)ip;

					obs_source_t *source = obs_sceneitem_get_source(item);
					if (!source || !osc_glob_match(ic->item_pattern, obs_source_get_name(source)))
						return true;

					bool target;
					if (ic->value)
						target = *ic->value;
					else if (ic->prop == item_prop::visible)
						target = !obs_sceneitem_visible(item);
					else
						target = !obs_sceneitem_locked(item);

					if (ic->prop == item_prop::visible)
						obs_sceneitem_set_visible(item, target);
					else
						obs_sceneitem_set_locked(item, target);
					return true;
				},
				&items);
			return true;
		},
		&ctx);
}

/* Moves every matching item to the same position index (0 = bottom of
 * the scene). With multiple matches each move happens in turn, so the
 * final stacking of those items is undefined; prefer exact names. */
static void apply_scene_item_order(const char *scene_pattern, const char *item_pattern, int position)
{
	struct scene_ctx {
		const char *scene_pattern;
		const char *item_pattern;
		int position;
	} ctx = {scene_pattern, item_pattern, position};

	obs_enum_scenes(
		[](void *p, obs_source_t *scene_source) {
			auto *c = (struct scene_ctx *)p;

			if (!osc_glob_match(c->scene_pattern, obs_source_get_name(scene_source)))
				return true;

			obs_scene_enum_items(
				obs_scene_from_source(scene_source),
				[](obs_scene_t *scene, obs_sceneitem_t *item, void *ip) {
					UNUSED_PARAMETER(scene);
					auto *oc = (struct scene_ctx *)ip;

					obs_source_t *source = obs_sceneitem_get_source(item);
					if (source && osc_glob_match(oc->item_pattern, obs_source_get_name(source)))
						obs_sceneitem_set_order_position(item, oc->position);
					return true;
				},
				c);
			return true;
		},
		&ctx);
}

/* Walks scene items once, answering a poll reply per match. Used for
 * /visible and /locked reads. */
static void poll_scene_item_bool(const char *scene_pattern, const char *item_pattern,
				 const osc_net::osc_endpoint &to, item_prop prop)
{
	struct scene_ctx {
		const char *scene_pattern;
		const char *item_pattern;
		const osc_net::osc_endpoint *to;
		item_prop prop;
		const char *scene_name; /* bound while walking */
	} ctx = {scene_pattern, item_pattern, &to, prop, ""};

	obs_enum_scenes(
		[](void *p, obs_source_t *scene_source) {
			auto *c = (struct scene_ctx *)p;

			const char *name = obs_source_get_name(scene_source);
			if (!osc_glob_match(c->scene_pattern, name))
				return true;
			c->scene_name = name;

			obs_scene_enum_items(
				obs_scene_from_source(scene_source),
				[](obs_scene_t *scene, obs_sceneitem_t *item, void *ip) {
					UNUSED_PARAMETER(scene);
					auto *pc = (struct scene_ctx *)ip;

					obs_source_t *source = obs_sceneitem_get_source(item);
					if (!source || !osc_glob_match(pc->item_pattern, obs_source_get_name(source)))
						return true;

					const bool on = pc->prop == item_prop::visible ? obs_sceneitem_visible(item)
										       : obs_sceneitem_locked(item);
					const std::string address =
						pc->prop == item_prop::visible
							? osc_addr::visible_for(pc->scene_name, obs_source_get_name(source))
							: osc_addr::locked_for(pc->scene_name, obs_source_get_name(source));
					poll_reply(*pc->to, address.c_str(), {osc_int(on ? 1 : 0)});
					return true;
				},
				c);
			return true;
		},
		&ctx);
}

/* Answers a poll reply per matching item with its order position. */
static void poll_scene_item_order(const char *scene_pattern, const char *item_pattern,
				  const osc_net::osc_endpoint &to)
{
	struct scene_ctx {
		const char *scene_pattern;
		const char *item_pattern;
		const osc_net::osc_endpoint *to;
		const char *scene_name; /* bound while walking */
	} ctx = {scene_pattern, item_pattern, &to, ""};

	obs_enum_scenes(
		[](void *p, obs_source_t *scene_source) {
			auto *c = (struct scene_ctx *)p;

			const char *name = obs_source_get_name(scene_source);
			if (!osc_glob_match(c->scene_pattern, name))
				return true;
			c->scene_name = name;

			obs_scene_enum_items(
				obs_scene_from_source(scene_source),
				[](obs_scene_t *scene, obs_sceneitem_t *item, void *ip) {
					UNUSED_PARAMETER(scene);
					auto *pc = (struct scene_ctx *)ip;

					obs_source_t *source = obs_sceneitem_get_source(item);
					if (!source || !osc_glob_match(pc->item_pattern, obs_source_get_name(source)))
						return true;

					const std::string address =
						osc_addr::order_for(pc->scene_name, obs_source_get_name(source));
					poll_reply(*pc->to, address.c_str(), {osc_int(obs_sceneitem_get_order_position(item))});
					return true;
				},
				c);
			return true;
		},
		&ctx);
}

/* One pass over sources and their filters serving both filter verbs:
 * when 'value' is given it is applied, otherwise each matching filter
 * reports its enabled state to '*to' (when non-null). Any source type
 * may carry filters, so scenes are included here. */
static void run_filter_pass(const char *source_pattern, const char *filter_pattern, const bool *value,
			    const osc_net::osc_endpoint *to)
{
	struct source_ctx {
		const char *source_pattern;
		const char *filter_pattern;
		const bool *value;
		const osc_net::osc_endpoint *to;
	} ctx = {source_pattern, filter_pattern, value, to};

	obs_enum_sources(
		[](void *p, obs_source_t *source) {
			auto *sc = (struct source_ctx *)p;

			const char *source_name = obs_source_get_name(source);
			if (!osc_glob_match(sc->source_pattern, source_name))
				return true;

			struct filter_ctx {
				const char *filter_pattern;
				const char *source_name;
				const bool *value;
				const osc_net::osc_endpoint *to;
				size_t next_index;
			} fc = {sc->filter_pattern, source_name, sc->value, sc->to, 0};

			obs_source_enum_filters(
				source,
				[](obs_source_t *parent, obs_source_t *filter, void *fp) {
					UNUSED_PARAMETER(parent);
					auto *c = (struct filter_ctx *)fp;

					const char *filter_name = obs_source_get_name(filter);
					if (!filter_selector_matches(c->filter_pattern, filter_name, c->next_index++))
						return;

					const std::string address = osc_addr::filter_for(c->source_name, filter_name);

					if (c->to) {
						poll_reply(*c->to, address.c_str(), {osc_int(obs_source_enabled(filter) ? 1 : 0)});
						return;
					}

					const bool target = c->value ? *c->value : !obs_source_enabled(filter);
					obs_source_set_enabled(filter, target);
				},
				&fc);
			return true;
		},
		&ctx);
}

/* Answers once per input matching the glob with its current tally
 * state: rendered in the program chain ('active') or shown on screen
 * at all ('showing'). */
static void poll_tally(const char *pattern, const osc_net::osc_endpoint &to, bool showing)
{
	struct tally_ctx {
		const char *pattern;
		const osc_net::osc_endpoint *to;
		bool showing;
	} ctx = {pattern, &to, showing};

	obs_enum_sources(
		[](void *p, obs_source_t *source) {
			auto *c = (struct tally_ctx *)p;

			if (!osc_is_input(source) || !osc_glob_match(c->pattern, obs_source_get_name(source)))
				return true;

			const bool on = c->showing ? obs_source_showing(source) : obs_source_active(source);
			const std::string address = osc_addr::prefixed(
				c->showing ? osc_addr::showing_prefix : osc_addr::active_prefix, obs_source_get_name(source));
			poll_reply(*c->to, address.c_str(), {osc_int(on ? 1 : 0)});
			return true;
		},
		&ctx);
}

/* ------------------------------------------------------------------ */
/* Filter settings parameters                                          */
/*                                                                    */
/* Addressed as "/filter/<source>/<filter-or-index>/<param>". Only      */
/* scalar property types map to OSC arguments (bool, int, float, text  */
/* and path); lists, colors and buttons are rejected with a warning.   */
/* The pseudo-parameter "params" reports every scalar parameter of a   */
/* filter at once. Writes copy the current settings, change one value  */
/* and push them back with obs_source_update().                        */
/* ------------------------------------------------------------------ */

/* Returns an add-ref'd filter of one source picked by name/glob or
 * digits index into the chain order. Release after use. */
static obs_source_t *resolve_filter(obs_source_t *parent, const char *selector)
{
	struct resolve_ctx {
		const char *selector;
		size_t next_index;
		obs_source_t *found;
	} ctx = {selector, 0, nullptr};

	obs_source_enum_filters(
		parent,
		[](obs_source_t *parent, obs_source_t *filter, void *p) {
			UNUSED_PARAMETER(parent);
			auto *c = (struct resolve_ctx *)p;

			if (!filter_selector_matches(c->selector, obs_source_get_name(filter), c->next_index++))
				return;

			if (obs_source_get_ref(filter))
				c->found = filter;
		},
		&ctx);

	return ctx.found;
}

/* Reports one scalar parameter's current value to '*to'. */
static void report_filter_param(const std::string &address, obs_property_t *prop, obs_data_t *settings,
				const osc_net::osc_endpoint &to)
{
	const char *name = obs_property_name(prop);

	switch (obs_property_get_type(prop)) {
	case OBS_PROPERTY_BOOL:
		poll_reply(to, address.c_str(), {osc_int(obs_data_get_bool(settings, name) ? 1 : 0)});
		break;
	case OBS_PROPERTY_INT:
		poll_reply(to, address.c_str(), {osc_int((int32_t)obs_data_get_int(settings, name))});
		break;
	case OBS_PROPERTY_FLOAT:
		poll_reply(to, address.c_str(), {osc_flt((float)obs_data_get_double(settings, name))});
		break;
	case OBS_PROPERTY_TEXT:
	case OBS_PROPERTY_PATH:
		poll_reply(to, address.c_str(), {osc_str(obs_data_get_string(settings, name))});
		break;
	default:
		break; /* caller already warned for writes; silence on listing */
	}
}

/* True when the property type maps to an OSC argument. */
static bool param_is_scalar(obs_property_t *prop)
{
	switch (obs_property_get_type(prop)) {
	case OBS_PROPERTY_BOOL:
	case OBS_PROPERTY_INT:
	case OBS_PROPERTY_FLOAT:
	case OBS_PROPERTY_TEXT:
	case OBS_PROPERTY_PATH:
		return true;
	default:
		return false;
	}
}

/* Applies one parameter write by copying the settings, changing the
 * single value and pushing them back through obs_source_update(). */
static void apply_filter_param(obs_source_t *filter, obs_property_t *prop, const osc_message &msg)
{
	const char *name = obs_property_name(prop);
	obs_data_t *settings = obs_source_get_settings(filter);

	switch (obs_property_get_type(prop)) {
	case OBS_PROPERTY_BOOL:
		obs_data_set_bool(settings, name, msg.arg_bool(0, false));
		break;
	case OBS_PROPERTY_INT:
		obs_data_set_int(settings, name, msg.arg_int(0, 0));
		break;
	case OBS_PROPERTY_FLOAT:
		obs_data_set_double(settings, name, msg.arg_float(0, 0.0f));
		break;
	case OBS_PROPERTY_TEXT:
	case OBS_PROPERTY_PATH:
		obs_data_set_string(settings, name, msg.arg_string(0, "").c_str());
		break;
	default:
		break;
	}

	obs_source_update(filter, settings);
	obs_data_release(settings);
}

/* Runs one parameter operation against the matching filters of every
 * source matching the source pattern. 'msg' writes when non-null,
 * otherwise the current value is reported to '*to'. */
static void run_filter_param_pass(const char *source_pattern, const char *selector, const char *param,
				  const osc_message *msg, const osc_net::osc_endpoint *to)
{
	struct source_ctx {
		const char *source_pattern;
		const char *selector;
		const char *param;
		const osc_message *msg;
		const osc_net::osc_endpoint *to;
	} ctx = {source_pattern, selector, param, msg, to};

	obs_enum_sources(
		[](void *p, obs_source_t *source) {
			auto *c = (struct source_ctx *)p;

			if (!osc_glob_match(c->source_pattern, obs_source_get_name(source)))
				return true;

			obs_source_t *filter = resolve_filter(source, c->selector);
			if (!filter)
				return true;

			const char *filter_name = obs_source_get_name(filter);
			const std::string base = osc_addr::filter_for(obs_source_get_name(source), filter_name);

			if (strcmp(c->param, osc_addr::filter_params_node) == 0) {
				obs_properties_t *props = obs_source_properties(filter);
				for (obs_property_t *prop = obs_properties_first(props); prop != nullptr;
				     obs_property_next(&prop)) {
					if (!param_is_scalar(prop))
						continue;
					obs_data_t *settings = obs_source_get_settings(filter);
					report_filter_param(base + "/" + obs_property_name(prop), prop, settings, *c->to);
					obs_data_release(settings);
				}
				obs_properties_destroy(props);
			} else if (c->msg) {
				bool handled = false;
				obs_properties_t *props = obs_source_properties(filter);
				for (obs_property_t *prop = obs_properties_first(props); prop != nullptr && !handled;
				     obs_property_next(&prop)) {
					if (strcmp(obs_property_name(prop), c->param) != 0)
						continue;
					handled = true;
					if (!param_is_scalar(prop)) {
						blog(LOG_WARNING,
						     "[obs-osc] filter parameter '%s' has no OSC representation", c->param);
						break;
					}
					apply_filter_param(filter, prop, *c->msg);
				}
				obs_properties_destroy(props);
				if (!handled)
					blog(LOG_WARNING, "[obs-osc] filter '%s' has no parameter '%s'", filter_name,
					     c->param);
			} else {
				bool found = false;
				obs_properties_t *props = obs_source_properties(filter);
				for (obs_property_t *prop = obs_properties_first(props); prop != nullptr && !found;
				     obs_property_next(&prop)) {
					if (strcmp(obs_property_name(prop), c->param) != 0)
						continue;
					found = true;
					obs_data_t *settings = obs_source_get_settings(filter);
					report_filter_param(base + "/" + obs_property_name(prop), prop, settings, *c->to);
					obs_data_release(settings);
				}
				obs_properties_destroy(props);
				if (!found)
					blog(LOG_WARNING, "[obs-osc] filter '%s' has no parameter '%s'", filter_name,
					     c->param);
			}

			obs_source_release(filter);
			return true;
		},
		&ctx);
}

/* ------------------------------------------------------------------ */
/* Scene item transform fields                                         */
/*                                                                    */
/* Addressed as "/transform/<scene>/<item-or-index>/<field>", fields    */
/* defined in the shared osc_transform_fields table. Reads answer per  */
/* field; writes move one field of every matched item.                 */
/* ------------------------------------------------------------------ */

struct transform_ctx {
	const char *scene_pattern;
	const char *item_selector; /* null = every item */
	const osc_transform_field *field; /* null = every field (polls) */
	float value; /* write payload */
	const osc_net::osc_endpoint *to; /* poll target; null = write */
	const char *scene_name; /* bound while walking scenes */
};

static bool transform_item_cb(obs_scene_t *scene, obs_sceneitem_t *item, void *param)
{
	UNUSED_PARAMETER(scene);
	auto *ctx = (struct transform_ctx *)param;

	obs_source_t *source = obs_sceneitem_get_source(item);
	if (!source)
		return true;

	const char *item_name = obs_source_get_name(source);

	if (ctx->item_selector) {
		const bool match = is_index_segment(ctx->item_selector)
					   ? (size_t)obs_sceneitem_get_order_position(item) ==
						     (size_t)atoi(ctx->item_selector)
					   : osc_glob_match(ctx->item_selector, item_name);
		if (!match)
			return true;
	}

	if (!ctx->to) {
		ctx->field->set(item, ctx->value);
		return true;
	}

	if (ctx->field) {
		const std::string address = osc_addr::transform_for(ctx->scene_name, item_name, ctx->field->name);
		poll_reply(*ctx->to, address.c_str(), {osc_flt(ctx->field->get(item))});
		return true;
	}

	for (size_t i = 0; i < osc_transform_field_count; i++) {
		const osc_transform_field &f = osc_transform_fields[i];
		const std::string address = osc_addr::transform_for(ctx->scene_name, item_name, f.name);
		poll_reply(*ctx->to, address.c_str(), {osc_flt(f.get(item))});
	}
	return true;
}

/* Walks scene items applying one transform read or write. */
static void run_transform_pass(const char *scene_pattern, const char *item_selector,
			       const osc_transform_field *field, float value, const osc_net::osc_endpoint *to)
{
	transform_ctx ctx = {scene_pattern, item_selector, field, value, to, ""};

	obs_enum_scenes(
		[](void *p, obs_source_t *scene_source) {
			auto *c = (struct transform_ctx *)p;

			const char *name = obs_source_get_name(scene_source);
			if (!osc_glob_match(c->scene_pattern, name))
				return true;
			c->scene_name = name;

			obs_scene_enum_items(obs_scene_from_source(scene_source), transform_item_cb, c);
			return true;
		},
		&ctx);
}

/* Press-and-release of the first hotkey registered under this exact
 * OBS hotkey name (e.g. "OBSBasic.StartRecording", or the name shown
 * in Settings > Hotkeys). */
static void trigger_hotkey_by_name(const char *name)
{
	struct hotkey_ctx {
		const char *name;
		obs_hotkey_id id;
		bool found;
	} ctx = {name, 0, false};

	obs_enum_hotkeys(
		[](void *p, obs_hotkey_id id, obs_hotkey_t *key) {
			auto *hc = (struct hotkey_ctx *)p;

			if (strcmp(hc->name, obs_hotkey_get_name(key)) != 0)
				return true;

			hc->id = id;
			hc->found = true;
			return false;
		},
		&ctx);

	if (!ctx.found) {
		blog(LOG_WARNING, "[obs-osc] hotkey '%s' not found", name);
		return;
	}

	obs_hotkey_trigger_routed_callback(ctx.id, true);
	obs_hotkey_trigger_routed_callback(ctx.id, false);
}

struct mute_poll_ctx {
	const char *pattern;
	const osc_net::osc_endpoint *to;
};

/* Answers once per input matching the glob, addressed by exact name. */
static void poll_mute_inputs(const char *pattern, const osc_net::osc_endpoint &to)
{
	mute_poll_ctx ctx = {pattern, &to};

	obs_enum_sources(
		[](void *p, obs_source_t *source) {
			auto *c = (mute_poll_ctx *)p;

			if (!osc_is_input(source) || !osc_glob_match(c->pattern, obs_source_get_name(source)))
				return true;

			const std::string address = osc_addr::mute_for(obs_source_get_name(source));
			poll_reply(*c->to, address.c_str(), {osc_int(obs_source_muted(source) ? 1 : 0)});
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
	poll_reply(to, address.c_str(), {osc_int((int)obs_source_media_get_state(source))});
	obs_source_release(source);
}

/* Answers a read request for a known route. Returns false for unknown
 * or write-only addresses so they keep reaching the dispatcher. */
static bool poll_dispatch(const std::string &a, const osc_net::osc_endpoint &to)
{
	if (a == osc_addr::studio) {
		poll_reply(to, osc_addr::studio, {osc_int(obs_frontend_preview_program_mode_active())});
		return true;
	}
	if (a == osc_addr::program) {
		poll_reply_source_name(to, osc_addr::program, obs_frontend_get_current_scene());
		return true;
	}
	if (a == osc_addr::preview) {
		poll_reply_source_name(to, osc_addr::preview, obs_frontend_get_current_preview_scene());
		return true;
	}
	if (a == osc_addr::transition) {
		poll_reply_source_name(to, osc_addr::transition, obs_frontend_get_current_transition());
		return true;
	}
	if (a == osc_addr::profile) {
		char *name = obs_frontend_get_current_profile();
		poll_reply(to, osc_addr::profile, {osc_str(name ? name : "")});
		bfree(name);
		return true;
	}
	if (a == osc_addr::collection) {
		char *name = obs_frontend_get_current_scene_collection();
		poll_reply(to, osc_addr::collection, {osc_str(name ? name : "")});
		bfree(name);
		return true;
	}
	if (a == osc_addr::record_paused) {
		poll_reply(to, osc_addr::record_paused, {osc_int(obs_frontend_recording_paused())});
		return true;
	}

	for (size_t i = 0; i < osc_addr::output_count; i++) {
		const osc_addr::output_info &out = osc_addr::outputs[i];
		if (a == out.base) {
			poll_reply(to, out.base, {osc_int(output_active(out.id))});
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
		poll_reply(to, address.c_str(), {osc_flt(osc_volume_position((float)obs_source_get_volume(source)))});
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
		poll_reply(to, address.c_str(), {osc_flt(db)});
		obs_source_release(source);
		return true;
	}
	if (const char *rest = osc_addr::tail(a, osc_addr::media_prefix)) {
		std::string name;
		std::string verb;
		if (!split_name_verb(rest, &name, &verb)) {
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
		poll_scene_item_bool("*", "*", to, item_prop::visible);
		return true;
	}
	if (a == osc_addr::locked) {
		poll_scene_item_bool("*", "*", to, item_prop::locked);
		return true;
	}
	if (a == osc_addr::order) {
		poll_scene_item_order("*", "*", to);
		return true;
	}
	if (a == osc_addr::filter) {
		run_filter_pass("*", "*", nullptr, &to);
		return true;
	}
	if (a == osc_addr::active) {
		poll_tally("*", to, false);
		return true;
	}
	if (a == osc_addr::showing) {
		poll_tally("*", to, true);
		return true;
	}

	if (const char *rest = osc_addr::tail(a, osc_addr::visible_prefix)) {
		std::string scene;
		std::string item;
		if (!split_name_verb(rest, &scene, &item))
			poll_scene_item_bool(rest, "*", to, item_prop::visible);
		else
			poll_scene_item_bool(scene.c_str(), item.c_str(), to, item_prop::visible);
		return true;
	}
	if (const char *rest = osc_addr::tail(a, osc_addr::locked_prefix)) {
		std::string scene;
		std::string item;
		if (!split_name_verb(rest, &scene, &item))
			poll_scene_item_bool(rest, "*", to, item_prop::locked);
		else
			poll_scene_item_bool(scene.c_str(), item.c_str(), to, item_prop::locked);
		return true;
	}
	if (const char *rest = osc_addr::tail(a, osc_addr::order_prefix)) {
		std::string scene;
		std::string item;
		if (!split_name_verb(rest, &scene, &item))
			poll_scene_item_order(rest, "*", to);
		else
			poll_scene_item_order(scene.c_str(), item.c_str(), to);
		return true;
	}
	if (const char *rest = osc_addr::tail(a, osc_addr::filter_prefix)) {
		std::string source;
		std::string rest2;
		if (!split_name_verb(rest, &source, &rest2)) {
			run_filter_pass(rest, "*", nullptr, &to);
			return true;
		}

		std::string selector;
		std::string param;
		if (!split_name_verb(rest2.c_str(), &selector, &param)) {
			run_filter_pass(source.c_str(), rest2.c_str(), nullptr, &to);
			return true;
		}

		/* "params" lists every scalar parameter; anything else reads one. */
		run_filter_param_pass(source.c_str(), selector.c_str(), param.c_str(), nullptr, &to);
		return true;
	}
	if (a == osc_addr::transform) {
		run_transform_pass("*", "*", nullptr, 0.0f, &to);
		return true;
	}
	if (const char *rest = osc_addr::tail(a, osc_addr::transform_prefix)) {
		std::string scene;
		std::string remainder;
		std::string item;
		std::string field_name;
		if (!split_name_verb(rest, &scene, &remainder)) {
			run_transform_pass(rest, "*", nullptr, 0.0f, &to);
			return true;
		}

		const osc_transform_field *field = nullptr;
		if (!split_name_verb(remainder.c_str(), &item, &field_name)) {
			run_transform_pass(scene.c_str(), remainder.c_str(), nullptr, 0.0f, &to);
			return true;
		}

		field = find_transform_field(field_name.c_str());
		if (!field) {
			blog(LOG_WARNING, "[obs-osc] poll: unknown transform field '%s'", field_name.c_str());
			return true;
		}
		run_transform_pass(scene.c_str(), item.c_str(), field, 0.0f, &to);
		return true;
	}
	if (const char *pattern = osc_addr::tail(a, osc_addr::active_prefix)) {
		poll_tally(pattern, to, false);
		return true;
	}
	if (const char *pattern = osc_addr::tail(a, osc_addr::showing_prefix)) {
		poll_tally(pattern, to, true);
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
		if (split_name_verb(rest, &name, &verb))
			media_command(name.c_str(), verb.c_str());
		return;
	}

	if (const char *rest = osc_addr::tail(a, osc_addr::visible_prefix)) {
		std::string scene;
		std::string item;
		if (split_name_verb(rest, &scene, &item)) {
			if (arg_is_toggle(msg)) {
				apply_scene_item_bool(scene.c_str(), item.c_str(), nullptr, item_prop::visible);
			} else {
				const bool value = msg.arg_bool(0, false);
				apply_scene_item_bool(scene.c_str(), item.c_str(), &value, item_prop::visible);
			}
		}
		return;
	}

	if (const char *rest = osc_addr::tail(a, osc_addr::locked_prefix)) {
		std::string scene;
		std::string item;
		if (split_name_verb(rest, &scene, &item)) {
			if (arg_is_toggle(msg)) {
				apply_scene_item_bool(scene.c_str(), item.c_str(), nullptr, item_prop::locked);
			} else {
				const bool value = msg.arg_bool(0, false);
				apply_scene_item_bool(scene.c_str(), item.c_str(), &value, item_prop::locked);
			}
		}
		return;
	}

	if (const char *rest = osc_addr::tail(a, osc_addr::order_prefix)) {
		std::string scene;
		std::string item;
		if (split_name_verb(rest, &scene, &item))
			apply_scene_item_order(scene.c_str(), item.c_str(), msg.arg_int(0, -1));
		return;
	}

	if (const char *rest = osc_addr::tail(a, osc_addr::filter_prefix)) {
		std::string source;
		std::string rest2;
		if (!split_name_verb(rest, &source, &rest2))
			return;

		/* Three segments select one parameter of the filter; two
		 * segments toggle/set its enabled state. */
		std::string selector;
		std::string param;
		if (split_name_verb(rest2.c_str(), &selector, &param)) {
			run_filter_param_pass(source.c_str(), selector.c_str(), param.c_str(), &msg, nullptr);
			return;
		}

		if (arg_is_toggle(msg)) {
			run_filter_pass(source.c_str(), rest2.c_str(), nullptr, nullptr);
		} else {
			const bool value = msg.arg_bool(0, false);
			run_filter_pass(source.c_str(), rest2.c_str(), &value, nullptr);
		}
		return;
	}

	if (const char *rest = osc_addr::tail(a, osc_addr::transform_prefix)) {
		std::string scene;
		std::string remainder;
		std::string item;
		std::string field_name;
		if (!split_name_verb(rest, &scene, &remainder))
			return;
		if (!split_name_verb(remainder.c_str(), &item, &field_name))
			return;

		const osc_transform_field *field = find_transform_field(field_name.c_str());
		if (!field) {
			blog(LOG_WARNING, "[obs-osc] unknown transform field '%s'", field_name.c_str());
			return;
		}

		run_transform_pass(scene.c_str(), item.c_str(), field, msg.arg_float(0, 0.0f), nullptr);
		return;
	}

	if (const char *name = osc_addr::tail(a, osc_addr::hotkey_prefix)) {
		trigger_hotkey_by_name(name);
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
