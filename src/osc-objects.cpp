#include "osc-objects.h"

#include "osc-addresses.h"
#include "osc-plugin.h"
#include "osc-server.h"

#include <obs.h>
#include <util/base.h>

#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <string>

namespace osc_objects {

namespace {

/* Builds and sends one poll reply to the datagram's sender. */
void reply(const osc_net::osc_endpoint &to, const std::string &address, std::initializer_list<osc_argument> args)
{
	g_server.reply(to, address.c_str(), args);
}

/* ------------------------------------------------------------------ */
/* Scene items                                                         */
/* ------------------------------------------------------------------ */

struct flag_ctx {
	const osc_net::osc_endpoint *to; /* null = write */
	item_prop prop;
	const bool *value; /* write path: null toggles each match */
};

bool flag_visit(const char *scene_name, obs_sceneitem_t *item, void *param)
{
	auto *ctx = (flag_ctx *)param;

	obs_source_t *source = obs_sceneitem_get_source(item);
	const char *item_name = source ? obs_source_get_name(source) : "";
	const bool state = ctx->prop == item_prop::visible ? obs_sceneitem_visible(item)
							   : obs_sceneitem_locked(item);

	if (!ctx->to) {
		const bool target = ctx->value ? *ctx->value : !state;
		if (ctx->prop == item_prop::visible)
			obs_sceneitem_set_visible(item, target);
		else
			obs_sceneitem_set_locked(item, target);
		return true;
	}

	const std::string address = ctx->prop == item_prop::visible
					    ? osc_addr::visible_for(scene_name, item_name)
					    : osc_addr::locked_for(scene_name, item_name);
	reply(*ctx->to, address, {osc_int(state ? 1 : 0)});
	return true;
}

struct order_ctx {
	const osc_net::osc_endpoint *to; /* null = write */
	int position; /* write payload */
};

bool order_visit(const char *scene_name, obs_sceneitem_t *item, void *param)
{
	auto *ctx = (order_ctx *)param;

	obs_source_t *source = obs_sceneitem_get_source(item);
	if (!source)
		return true;

	if (!ctx->to) {
		obs_sceneitem_set_order_position(item, ctx->position);
		return true;
	}

	const std::string address = osc_addr::order_for(scene_name, obs_source_get_name(source));
	reply(*ctx->to, address, {osc_int(obs_sceneitem_get_order_position(item))});
	return true;
}

const osc_transform_field *find_transform_field(const char *name)
{
	for (size_t i = 0; i < osc_transform_field_count; i++) {
		if (strcmp(osc_transform_fields[i].name, name) == 0)
			return &osc_transform_fields[i];
	}
	return nullptr;
}

struct transform_ctx {
	const osc_net::osc_endpoint *to; /* null = write */
	const osc_transform_field *field; /* null = every field when polling */
	float value; /* write payload */
};

bool transform_visit(const char *scene_name, obs_sceneitem_t *item, void *param)
{
	auto *ctx = (transform_ctx *)param;

	obs_source_t *source = obs_sceneitem_get_source(item);
	if (!source)
		return true;
	const char *item_name = obs_source_get_name(source);

	if (!ctx->to) {
		ctx->field->set(item, ctx->value);
		return true;
	}

	if (ctx->field) {
		reply(*ctx->to, osc_addr::transform_for(scene_name, item_name, ctx->field->name),
		      {osc_flt(ctx->field->get(item))});
		return true;
	}

	for (size_t i = 0; i < osc_transform_field_count; i++) {
		const osc_transform_field &f = osc_transform_fields[i];
		reply(*ctx->to, osc_addr::transform_for(scene_name, item_name, f.name), {osc_flt(f.get(item))});
	}
	return true;
}

/* ------------------------------------------------------------------ */
/* Filters                                                             */
/* ------------------------------------------------------------------ */

/* Returns an add-ref'd filter of one source picked by selector;
 * release after use. */
obs_source_t *resolve_filter(obs_source_t *parent, const char *selector)
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

			if (!osc_selector_matches(c->selector, obs_source_get_name(filter), c->next_index++))
				return;

			if (obs_source_get_ref(filter))
				c->found = filter;
		},
		&ctx);

	return ctx.found;
}

/* True when the property type maps to an OSC argument. */
bool param_is_scalar(obs_property_t *prop)
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

/* Reports one scalar parameter's current value. */
void read_property(const std::string &base, obs_property_t *prop, obs_data_t *settings,
		   const osc_net::osc_endpoint &to)
{
	const char *name = obs_property_name(prop);

	switch (obs_property_get_type(prop)) {
	case OBS_PROPERTY_BOOL:
		reply(to, base + "/" + name, {osc_int(obs_data_get_bool(settings, name) ? 1 : 0)});
		break;
	case OBS_PROPERTY_INT:
		reply(to, base + "/" + name, {osc_int((int32_t)obs_data_get_int(settings, name))});
		break;
	case OBS_PROPERTY_FLOAT:
		reply(to, base + "/" + name, {osc_flt((float)obs_data_get_double(settings, name))});
		break;
	case OBS_PROPERTY_TEXT:
	case OBS_PROPERTY_PATH:
		reply(to, base + "/" + name, {osc_str(obs_data_get_string(settings, name))});
		break;
	default:
		break;
	}
}

/* Copies the current settings, changes one value and pushes them back
 * through obs_source_update() so source UIs stay in sync. */
void write_property(obs_source_t *filter, obs_property_t *prop, const osc_message &msg)
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

struct filter_enable_ctx {
	const char *selector;
	const bool *value; /* null toggles each match */
	const osc_net::osc_endpoint *to; /* null = write */
};

void run_filter_enable_pass(const char *source_pattern, const char *selector, const bool *value,
			    const osc_net::osc_endpoint *to)
{
	filter_enable_ctx ctx = {selector, value, to};

	osc_visit_sources(
		source_pattern, osc_source_kind::any,
		[](obs_source_t *source, void *p) {
			auto *c = (filter_enable_ctx *)p;

			struct chain_ctx {
				const char *source_name;
				const char *selector;
				const bool *value;
				const osc_net::osc_endpoint *to;
				size_t next_index;
			} chain = {obs_source_get_name(source), c->selector, c->value, c->to, 0};

			obs_source_enum_filters(
				source,
				[](obs_source_t *parent, obs_source_t *filter, void *fp) {
					UNUSED_PARAMETER(parent);
					auto *fc = (chain_ctx *)fp;

					if (!osc_selector_matches(fc->selector, obs_source_get_name(filter),
								  fc->next_index++))
						return;

					const std::string address = osc_addr::filter_for(
						fc->source_name, obs_source_get_name(filter));

					if (!fc->to) {
						const bool target =
							fc->value ? *fc->value : !obs_source_enabled(filter);
						obs_source_set_enabled(filter, target);
						return;
					}

					reply(*fc->to, address,
					      {osc_int(obs_source_enabled(filter) ? 1 : 0)});
				},
				&chain);
			return true;
		},
		&ctx);
}

enum class param_mode { list_all, read_one, write_one };

struct param_ctx {
	param_mode mode;
	const char *selector;
	const char *param;
	const osc_message *msg; /* write_one payload */
	const osc_net::osc_endpoint *to;
};

void run_param_pass(const char *source_pattern, const char *selector, const char *param, param_mode mode,
		    const osc_message *msg, const osc_net::osc_endpoint *to)
{
	param_ctx ctx = {mode, selector, param, msg, to};

	osc_visit_sources(
		source_pattern, osc_source_kind::any,
		[](obs_source_t *source, void *p) {
			auto *c = (param_ctx *)p;

			obs_source_t *filter = resolve_filter(source, c->selector);
			if (!filter)
				return true;

			const std::string base = osc_addr::filter_for(obs_source_get_name(source),
								      obs_source_get_name(filter));

			/* One property iteration serves every mode; the
			 * property must be used while the iterator owns
			 * it, since destroying the set frees it. */
			bool handled = false;
			obs_properties_t *props = obs_source_properties(filter);
			for (obs_property_t *prop = obs_properties_first(props); prop != nullptr;
			     obs_property_next(&prop)) {
				if (c->mode != param_mode::list_all &&
				    strcmp(obs_property_name(prop), c->param) != 0)
					continue;
				if (!param_is_scalar(prop)) {
					if (c->mode == param_mode::write_one ||
					    c->mode == param_mode::read_one)
						blog(LOG_WARNING,
						     "[obs-osc] filter parameter '%s' has no OSC representation",
						     c->param);
					continue;
				}

				if (c->mode == param_mode::write_one) {
					write_property(filter, prop, *c->msg);
				} else {
					obs_data_t *settings = obs_source_get_settings(filter);
					read_property(base, prop, settings, *c->to);
					obs_data_release(settings);
				}
				handled = true;

				if (c->mode != param_mode::list_all)
					break;
			}
			obs_properties_destroy(props);

			if (!handled && c->mode != param_mode::list_all)
				blog(LOG_WARNING, "[obs-osc] filter '%s' has no OSC parameter '%s'",
				     obs_source_get_name(filter), c->param);

			obs_source_release(filter);
			return true;
		},
		&ctx);
}

struct tally_ctx {
	const osc_net::osc_endpoint *to;
	bool showing;
};

} // namespace

/* ------------------------------------------------------------------ */
/* Public entry points                                                 */
/* ------------------------------------------------------------------ */

void set_item_flag(const char *scene_pattern, const char *item_selector, item_prop prop, const bool *value)
{
	flag_ctx ctx = {nullptr, prop, value};
	osc_visit_scene_items(scene_pattern, item_selector, flag_visit, &ctx);
}

void poll_item_flag(const osc_net::osc_endpoint &to, const char *scene_pattern, const char *item_selector,
		    item_prop prop)
{
	flag_ctx ctx = {&to, prop, nullptr};
	osc_visit_scene_items(scene_pattern, item_selector, flag_visit, &ctx);
}

void set_item_order(const char *scene_pattern, const char *item_selector, int position)
{
	order_ctx ctx = {nullptr, position};
	osc_visit_scene_items(scene_pattern, item_selector, order_visit, &ctx);
}

void poll_item_order(const osc_net::osc_endpoint &to, const char *scene_pattern, const char *item_selector)
{
	order_ctx ctx = {&to, 0};
	osc_visit_scene_items(scene_pattern, item_selector, order_visit, &ctx);
}

void write_transform(const char *scene_pattern, const char *item_selector, const char *field_name, float value)
{
	const osc_transform_field *field = find_transform_field(field_name);
	if (!field) {
		blog(LOG_WARNING, "[obs-osc] unknown transform field '%s'", field_name);
		return;
	}

	transform_ctx ctx = {nullptr, field, value};
	osc_visit_scene_items(scene_pattern, item_selector, transform_visit, &ctx);
}

void poll_transform(const osc_net::osc_endpoint &to, const char *scene_pattern, const char *item_selector,
		    const char *field_name)
{
	const osc_transform_field *field = nullptr;
	if (field_name) {
		field = find_transform_field(field_name);
		if (!field) {
			blog(LOG_WARNING, "[obs-osc] poll: unknown transform field '%s'", field_name);
			return;
		}
	}

	transform_ctx ctx = {&to, field, 0.0f};
	osc_visit_scene_items(scene_pattern, item_selector, transform_visit, &ctx);
}

void set_filter_enabled(const char *source_pattern, const char *selector, const bool *value)
{
	run_filter_enable_pass(source_pattern, selector, value, nullptr);
}

void poll_filter_enabled(const osc_net::osc_endpoint &to, const char *source_pattern, const char *selector)
{
	run_filter_enable_pass(source_pattern, selector, nullptr, &to);
}

void poll_filter_params(const osc_net::osc_endpoint &to, const char *source_pattern, const char *selector)
{
	run_param_pass(source_pattern, selector, "", param_mode::list_all, nullptr, &to);
}

void poll_filter_param(const osc_net::osc_endpoint &to, const char *source_pattern, const char *selector,
		       const char *param)
{
	run_param_pass(source_pattern, selector, param, param_mode::read_one, nullptr, &to);
}

void write_filter_param(const osc_message &msg, const char *source_pattern, const char *selector,
			const char *param)
{
	run_param_pass(source_pattern, selector, param, param_mode::write_one, &msg, nullptr);
}

void poll_input_state(const osc_net::osc_endpoint &to, const char *pattern, bool showing)
{
	tally_ctx ctx = {&to, showing};

	osc_visit_sources(
		pattern, osc_source_kind::input,
		[](obs_source_t *source, void *p) {
			auto *c = (tally_ctx *)p;

			const bool on = c->showing ? obs_source_showing(source) : obs_source_active(source);
			const std::string address = osc_addr::prefixed(
				c->showing ? osc_addr::showing_prefix : osc_addr::active_prefix,
				obs_source_get_name(source));
			reply(*c->to, address, {osc_int(on ? 1 : 0)});
			return true;
		},
		&ctx);
}

void trigger_hotkey(const char *name)
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

} // namespace osc_objects
