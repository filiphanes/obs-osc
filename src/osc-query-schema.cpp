#include "osc-query-schema.h"

#include "osc-addresses.h"
#include "osc-plugin.h"
#include "osc-ui-task.h"
#include "osc-util.h"

#include <obs-frontend-api.h>
#include <obs-module.h>

#include <cstring>
#include <string>
#include <vector>

static const char contents_key[] = "CONTENTS";

/* ------------------------------------------------------------------ */
/* JSON node helpers                                                   */
/* ------------------------------------------------------------------ */

/* Adds a child when the parent's CONTENTS map is already at hand and
 * transfers ownership of child to it. */
static void map_set(obs_data_t *contents, const char *name, obs_data_t *child)
{
	obs_data_set_obj(contents, name, child);
	obs_data_release(child);
}

/* Adds a child to a node's CONTENTS map. */
static void add_child(obs_data_t *parent, const char *name, obs_data_t *child)
{
	obs_data_t *contents = obs_data_get_obj(parent, contents_key);
	map_set(contents, name, child);
	obs_data_release(contents);
}

static void set_description(obs_data_t *node, const char *description)
{
	obs_data_set_string(node, "DESCRIPTION", description);
}

/* Core leaf factory: ACCESS plus optional TYPE, VALUE, RANGE and
 * DESCRIPTION; every make_* below is a thin wrapper around this. */
static obs_data_t *make_leaf(int access, const char *type, double value, bool ranged, double min,
			     double max, const char *description)
{
	obs_data_t *node = obs_data_create();
	obs_data_set_int(node, "ACCESS", access);

	if (type) {
		obs_data_set_string(node, "TYPE", type);
		if (type[0] == 'i')
			obs_data_set_int(node, "VALUE", (long)value);
		else
			obs_data_set_double(node, "VALUE", value);
	}

	if (ranged) {
		obs_data_array_t *range = obs_data_array_create();
		obs_data_t *bounds = obs_data_create();
		if (type && type[0] == 'i') {
			obs_data_set_int(bounds, "MIN", (long)min);
			obs_data_set_int(bounds, "MAX", (long)max);
		} else {
			obs_data_set_double(bounds, "MIN", min);
			obs_data_set_double(bounds, "MAX", max);
		}
		obs_data_array_push_back(range, bounds);
		obs_data_release(bounds);
		obs_data_set_array(node, "RANGE", range);
		obs_data_array_release(range);
	}

	if (description)
		set_description(node, description);
	return node;
}

/* Write-only command with no arguments: renders as a button. */
static obs_data_t *make_bang(const char *description = nullptr)
{
	return make_leaf(1, nullptr, 0.0, false, 0.0, 0.0, description);
}

static obs_data_t *make_rw_int(long value, long min, long max, const char *description = nullptr)
{
	return make_leaf(3, "i", (double)value, true, (double)min, (double)max, description);
}

static obs_data_t *make_rw_float(double value, const char *description = nullptr)
{
	return make_leaf(3, "f", value, true, 0.0, 1.0, description);
}

static obs_data_t *make_ro_int(long value, const char *description = nullptr)
{
	return make_leaf(2, "i", (double)value, false, 0.0, 0.0, description);
}

static obs_data_t *make_container(const char *description = nullptr)
{
	obs_data_t *node = obs_data_create();
	obs_data_set_int(node, "ACCESS", 0);
	obs_data_t *contents = obs_data_create();
	obs_data_set_obj(node, contents_key, contents);
	obs_data_release(contents);
	if (description)
		set_description(node, description);
	return node;
}

static void add_media_children(obs_data_t *parent)
{
	for (size_t i = 0; i < osc_addr::media_verb_count; i++)
		add_child(parent, osc_addr::media_verbs[i].name, make_bang());

	add_child(parent, osc_addr::media_state_node,
		  make_ro_int((long)OBS_MEDIA_STATE_NONE,
			      "Media state (0 none, 1 playing, 4 paused, ...)"));
}

/* ------------------------------------------------------------------ */
/* Per-section builders                                                */
/* ------------------------------------------------------------------ */

/* Each builder produces exactly one top-level section, so a request
 * for "/volume/Mic" costs one enumeration instead of the whole tree;
 * polling clients no longer rebuild everything on every GET. All
 * builders must run on the UI thread and hand ownership of their node
 * to the caller. Sections whose values derive from shared walks take
 * null roots for the parts a request did not ask for. */

static bool scene_bang_cb(void *param, obs_source_t *scene)
{
	add_child((obs_data_t *)param, obs_source_get_name(scene), make_bang());
	return true;
}

static obs_data_t *build_scenes_section(const char *description)
{
	obs_data_t *node = make_container(description);
	obs_enum_scenes(scene_bang_cb, node);
	return node;
}

static obs_data_t *build_transition_section(void)
{
	obs_data_t *node = make_container("Select transition; 'go' runs it");
	add_child(node, osc_addr::transition_go, make_bang("Trigger transition"));
	osc_enum_transitions(
		[](obs_source_t *transition, void *param) {
			add_child((obs_data_t *)param, obs_source_get_name(transition),
				  make_bang());
		},
		node);
	return node;
}

static obs_data_t *build_output_section(const osc_addr::output_info &out)
{
	obs_data_t *node = make_container(out.description);

	for (size_t k = 0; k < out.sub_count; k++) {
		if (strcmp(out.subs[k], osc_addr::sub_pause) == 0)
			add_child(node, out.subs[k],
				  make_rw_int(obs_frontend_recording_paused() ? 1 : 0, 0, 1,
					      "Pause state"));
		else
			add_child(node, out.subs[k], make_bang());
	}
	return node;
}

/* Mute / volume / media share one input walk; unrequested roots stay
 * null so their branches cost nothing. */
struct source_ctx {
	obs_data_t *mute; /* all three may be null */
	obs_data_t *volume;
	obs_data_t *media;
};

static bool source_enum_visit(obs_source_t *source, void *param)
{
	auto *ctx = (struct source_ctx *)param;

	const char *name = obs_source_get_name(source);

	if (ctx->mute)
		add_child(ctx->mute, name, make_rw_int(obs_source_muted(source) ? 1 : 0, 0, 1));
	if (ctx->volume)
		add_child(ctx->volume, name,
			  make_rw_float(osc_volume_position((float)obs_source_get_volume(source))));
	if (ctx->media && (obs_source_get_output_flags(source) & OBS_SOURCE_CONTROLLABLE_MEDIA)) {
		obs_data_t *media = make_container("Media playback controls");
		add_media_children(media);
		add_child(ctx->media, name, media);
	}
	return true;
}

static obs_data_t *build_source_section(const std::string &which)
{
	const char *description;
	source_ctx ctx = {nullptr, nullptr, nullptr};

	if (which == osc_addr::mute) {
		description = "Mute inputs (* and ? globs work over OSC)";
		ctx.mute = make_container();
	} else if (which == osc_addr::volume) {
		description = "Volume faders, 0..1";
		ctx.volume = make_container();
	} else {
		description = "Media source transport";
		ctx.media = make_container();
	}

	osc_visit_sources("*", osc_source_kind::input, source_enum_visit, &ctx);

	obs_data_t *node = ctx.mute ? ctx.mute : (ctx.volume ? ctx.volume : ctx.media);
	set_description(node, description);
	return node;
}

/* ---- Scene items: visibility / lock / stacking order ---------------- */

struct item_roots {
	obs_data_t *visible; /* any member may be null */
	obs_data_t *locked;
	obs_data_t *order;
};

/* Per-scene walk state; scene_name is bound by the scene callback. */
struct scene_walk_ctx {
	item_roots parents;
	const char *scene_name;
};

static bool schema_item_cb(obs_scene_t *scene, obs_sceneitem_t *item, void *param)
{
	UNUSED_PARAMETER(scene);
	auto *ctx = (struct scene_walk_ctx *)param;

	obs_source_t *source = obs_sceneitem_get_source(item);
	if (!source)
		return true;

	const char *name = obs_source_get_name(source);

	if (ctx->parents.visible)
		add_child(ctx->parents.visible, name,
			  make_rw_int(obs_sceneitem_visible(item) ? 1 : 0, 0, 1));
	if (ctx->parents.locked)
		add_child(ctx->parents.locked, name,
			  make_rw_int(obs_sceneitem_locked(item) ? 1 : 0, 0, 1));
	if (ctx->parents.order)
		add_child(ctx->parents.order, name,
			  make_rw_int(obs_sceneitem_get_order_position(item), 0, 4095,
				      "0 = bottom"));
	return true;
}

static bool schema_scene_cb(void *param, obs_source_t *scene)
{
	auto *roots = (struct item_roots *)param;

	const char *scene_name = obs_source_get_name(scene);

	obs_data_t *visible = roots->visible ? make_container() : nullptr;
	obs_data_t *locked = roots->locked ? make_container() : nullptr;
	obs_data_t *order = roots->order ? make_container() : nullptr;

	struct scene_walk_ctx ctx = {{visible, locked, order}, scene_name};
	obs_scene_enum_items(obs_scene_from_source(scene), schema_item_cb, &ctx);

	if (visible)
		add_child(roots->visible, scene_name, visible);
	if (locked)
		add_child(roots->locked, scene_name, locked);
	if (order)
		add_child(roots->order, scene_name, order);
	return true;
}

/* One scene walk fills every requested root; null out-params skip a
 * branch, so single-section requests pay for one root only while the
 * root document still shares a single enumeration across all three. */
static void build_item_sections(obs_data_t **visible_out, obs_data_t **locked_out,
				obs_data_t **order_out)
{
	struct item_roots roots = {visible_out ? make_container("Scene item visibility") : nullptr,
				   locked_out ? make_container("Scene item lock") : nullptr,
				   order_out ? make_container("Scene item stacking order") : nullptr};

	obs_enum_scenes(schema_scene_cb, &roots);

	if (visible_out)
		*visible_out = roots.visible;
	if (locked_out)
		*locked_out = roots.locked;
	if (order_out)
		*order_out = roots.order;
}

static obs_data_t *build_filter_section(void)
{
	obs_data_t *node = make_container("Filter enable toggles");
	osc_visit_sources(
		"*", osc_source_kind::any,
		[](obs_source_t *source, void *param) {
			if (obs_source_filter_count(source) == 0)
				return true;

			obs_data_t *filters = make_container();
			obs_source_enum_filters(
				source,
				[](obs_source_t *parent, obs_source_t *filter, void *fp) {
					UNUSED_PARAMETER(parent);
					add_child((obs_data_t *)fp, obs_source_get_name(filter),
						  make_rw_int(obs_source_enabled(filter) ? 1 : 0, 0,
							      1));
				},
				filters);
			add_child((obs_data_t *)param, obs_source_get_name(source), filters);
			return true;
		},
		node);
	return node;
}

/* Active / showing tally share shape; one flag picks the branch. */
struct tally_ctx {
	obs_data_t *root;
	bool showing;
};

static bool tally_enum_visit(obs_source_t *source, void *param)
{
	auto *ctx = (struct tally_ctx *)param;

	const int on = ctx->showing ? obs_source_showing(source) : obs_source_active(source);
	add_child(ctx->root, obs_source_get_name(source),
		  make_ro_int(on ? 1 : 0,
			      ctx->showing ? "On screen" : "In program chain"));
	return true;
}

static obs_data_t *build_tally_section(bool showing)
{
	tally_ctx ctx = {make_container(showing ? "Inputs shown on screen at all"
						 : "Inputs rendered in the program chain"),
			 showing};
	osc_visit_sources("*", osc_source_kind::input, tally_enum_visit, &ctx);
	return ctx.root;
}

static obs_data_t *build_hotkey_section(void)
{
	obs_data_t *node = make_container("Trigger hotkey by OBS name");
	obs_enum_hotkeys(
		[](void *data, obs_hotkey_id id, obs_hotkey_t *key) {
			UNUSED_PARAMETER(id);
			add_child((obs_data_t *)data, obs_hotkey_get_name(key), make_bang());
			return true;
		},
		node);
	return node;
}

static void add_name_children(obs_data_t *parent, char **names)
{
	if (!names)
		return;

	for (size_t i = 0; names[i]; i++)
		add_child(parent, names[i], make_bang());
}

static obs_data_t *build_profile_section(void)
{
	obs_data_t *node = make_container("Switch profile");
	char **profiles = obs_frontend_get_profiles();
	add_name_children(node, profiles);
	/* The frontend returns one contiguous allocation: one bfree. */
	bfree(profiles);
	return node;
}

static obs_data_t *build_collection_section(void)
{
	obs_data_t *node = make_container("Switch scene collection");
	char **collections = obs_frontend_get_scene_collections();
	add_name_children(node, collections);
	bfree(collections);
	return node;
}

/* Builds one top-level node addressed by its first path segment; null
 * means the segment is unknown. The caller owns the returned node. */
static obs_data_t *build_section(const std::string &segment)
{
	if (segment == "program")
		return build_scenes_section("Switch program scene");
	if (segment == "preview")
		return build_scenes_section("Set preview scene");
	if (segment == osc_addr::leaf(osc_addr::transition))
		return build_transition_section();

	for (size_t i = 0; i < osc_addr::output_count; i++) {
		if (segment == osc_addr::leaf(osc_addr::outputs[i].base))
			return build_output_section(osc_addr::outputs[i]);
	}

	if (segment == osc_addr::leaf(osc_addr::studio))
		return make_rw_int(obs_frontend_preview_program_mode_active() ? 1 : 0, 0, 1,
				   "Studio mode");
	if (segment == osc_addr::leaf(osc_addr::screenshot))
		return make_bang("Take screenshot");

	if (segment == osc_addr::mute || segment == osc_addr::volume ||
	    segment == osc_addr::media)
		return build_source_section(segment);

	if (segment == osc_addr::leaf(osc_addr::visible_prefix)) {
		obs_data_t *visible = nullptr;
		build_item_sections(&visible, nullptr, nullptr);
		return visible;
	}
	if (segment == osc_addr::leaf(osc_addr::locked_prefix)) {
		obs_data_t *locked = nullptr;
		build_item_sections(nullptr, &locked, nullptr);
		return locked;
	}
	if (segment == osc_addr::leaf(osc_addr::order_prefix)) {
		obs_data_t *order = nullptr;
		build_item_sections(nullptr, nullptr, &order);
		return order;
	}

	if (segment == osc_addr::leaf(osc_addr::filter_prefix))
		return build_filter_section();

	if (segment == osc_addr::leaf(osc_addr::active_prefix))
		return build_tally_section(false);
	if (segment == osc_addr::leaf(osc_addr::showing_prefix))
		return build_tally_section(true);

	if (segment == osc_addr::hotkey_prefix)
		return build_hotkey_section();

	if (segment == osc_addr::leaf(osc_addr::profile))
		return build_profile_section();
	if (segment == osc_addr::leaf(osc_addr::collection))
		return build_collection_section();

	if (segment == osc_addr::leaf(osc_addr::subscribe))
		return make_bang("Subscribe this client to a topic: ,s <pattern> [,i port]; "
				 "wildcards * and ? allowed");
	if (segment == osc_addr::leaf(osc_addr::unsubscribe))
		return make_bang("Unsubscribe: ,s <pattern>, or no argument for everything");

	return nullptr;
}

/* ------------------------------------------------------------------ */
/* Tree assembly                                                       */
/* ------------------------------------------------------------------ */

/* Builds the complete root document ("/"). Every section comes from
 * the same builders a targeted request uses; the scene-item trio
 * shares one enumeration. */
static obs_data_t *build_root(void)
{
	obs_data_t *root = obs_data_create();
	obs_data_t *obs_contents = obs_data_create();
	obs_data_set_obj(root, contents_key, obs_contents);

	map_set(obs_contents, "program", build_section("program"));
	map_set(obs_contents, "preview", build_section("preview"));
	map_set(obs_contents, osc_addr::leaf(osc_addr::transition),
		build_section(osc_addr::leaf(osc_addr::transition)));

	for (size_t i = 0; i < osc_addr::output_count; i++) {
		const osc_addr::output_info &out = osc_addr::outputs[i];
		map_set(obs_contents, osc_addr::leaf(out.base), build_output_section(out));
	}

	map_set(obs_contents, osc_addr::leaf(osc_addr::studio),
		build_section(osc_addr::leaf(osc_addr::studio)));
	map_set(obs_contents, osc_addr::leaf(osc_addr::screenshot),
		build_section(osc_addr::leaf(osc_addr::screenshot)));

	map_set(obs_contents, osc_addr::mute, build_section(osc_addr::mute));
	map_set(obs_contents, osc_addr::volume, build_section(osc_addr::volume));
	map_set(obs_contents, osc_addr::media, build_section(osc_addr::media));

	obs_data_t *visible = nullptr;
	obs_data_t *locked = nullptr;
	obs_data_t *order = nullptr;
	build_item_sections(&visible, &locked, &order);
	map_set(obs_contents, osc_addr::leaf(osc_addr::visible_prefix), visible);
	map_set(obs_contents, osc_addr::leaf(osc_addr::locked_prefix), locked);
	map_set(obs_contents, osc_addr::leaf(osc_addr::order_prefix), order);

	map_set(obs_contents, osc_addr::leaf(osc_addr::filter_prefix),
		build_section(osc_addr::leaf(osc_addr::filter_prefix)));

	map_set(obs_contents, osc_addr::leaf(osc_addr::active_prefix),
		build_section(osc_addr::leaf(osc_addr::active_prefix)));
	map_set(obs_contents, osc_addr::leaf(osc_addr::showing_prefix),
		build_section(osc_addr::leaf(osc_addr::showing_prefix)));

	map_set(obs_contents, osc_addr::hotkey_prefix, build_section(osc_addr::hotkey_prefix));

	map_set(obs_contents, osc_addr::leaf(osc_addr::profile),
		build_section(osc_addr::leaf(osc_addr::profile)));
	map_set(obs_contents, osc_addr::leaf(osc_addr::collection),
		build_section(osc_addr::leaf(osc_addr::collection)));

	map_set(obs_contents, osc_addr::leaf(osc_addr::subscribe),
		build_section(osc_addr::leaf(osc_addr::subscribe)));
	map_set(obs_contents, osc_addr::leaf(osc_addr::unsubscribe),
		build_section(osc_addr::leaf(osc_addr::unsubscribe)));

	obs_data_release(obs_contents);
	return root;
}

/* Splits a path into its non-empty segments. */
static std::vector<std::string> split_path(const std::string &path)
{
	std::vector<std::string> segments;

	size_t start = 0;
	while (start <= path.size()) {
		const size_t slash = path.find('/', start);
		std::string segment = slash == std::string::npos
					      ? path.substr(start)
					      : path.substr(start, slash - start);
		if (!segment.empty())
			segments.push_back(std::move(segment));
		if (slash == std::string::npos)
			break;
		start = slash + 1;
	}
	return segments;
}

/* Walks the requested path, building only the section it addresses:
 * the first segment selects one subtree, later segments descend its
 * CONTENTS without constructing anything. Empty result means not
 * found. Must run on the UI thread. */
static std::string walk_tree(const std::string &path)
{
	std::string body;

	const std::vector<std::string> segments = split_path(path);

	if (segments.empty()) {
		obs_data_t *root = build_root();
		const char *json = obs_data_get_json(root);
		body = json ? json : "";
		obs_data_release(root);
		return body;
	}

	obs_data_t *node = build_section(segments.front());
	for (size_t i = 1; node && i < segments.size(); i++) {
		obs_data_t *contents = obs_data_get_obj(node, contents_key);
		obs_data_t *child =
			contents ? obs_data_get_obj(contents, segments[i].c_str()) : nullptr;
		obs_data_release(contents);
		obs_data_release(node);
		node = child;
	}

	if (node) {
		/* Serializes into an internal cache owned by node; copy it. */
		const char *json = obs_data_get_json(node);
		body = json ? json : "";
		obs_data_release(node);
	}
	return body;
}

std::string osc_query_schema_lookup(const std::string &path)
{
	std::string body;

	blog(LOG_DEBUG, "[obs-osc] queuing query build for '%s'", path.c_str());

	osc_ui_run([&] { body = walk_tree(path); });

	blog(LOG_DEBUG, "[obs-osc] query build done for '%s'", path.c_str());
	return body;
}
