#include "osc-query-schema.h"

#include "osc-addresses.h"
#include "osc-plugin.h"
#include "osc-ui-task.h"
#include "osc-util.h"

#include <obs-frontend-api.h>
#include <obs-module.h>

#include <cstring>
#include <string>

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

/* ------------------------------------------------------------------ */
/* Tree construction (must run on the UI thread)                       */
/* ------------------------------------------------------------------ */

static void add_media_children(obs_data_t *parent)
{
	for (size_t i = 0; i < osc_addr::media_verb_count; i++)
		add_child(parent, osc_addr::media_verbs[i].name, make_bang());

	add_child(parent, osc_addr::media_state_node,
		  make_ro_int((long)OBS_MEDIA_STATE_NONE,
			      "Media state (0 none, 1 playing, 4 paused, ...)"));
}

struct source_ctx {
	obs_data_t *mute;
	obs_data_t *volume;
	obs_data_t *media;
};

static bool source_enum_visit(obs_source_t *source, void *param)
{
	auto *ctx = (struct source_ctx *)param;

	const char *name = obs_source_get_name(source);

	add_child(ctx->mute, name, make_rw_int(obs_source_muted(source) ? 1 : 0, 0, 1));
	add_child(ctx->volume, name,
		  make_rw_float(osc_volume_position((float)obs_source_get_volume(source))));

	if (obs_source_get_output_flags(source) & OBS_SOURCE_CONTROLLABLE_MEDIA) {
		obs_data_t *media = make_container("Media playback controls");
		add_media_children(media);
		add_child(ctx->media, name, media);
	}
	return true;
}

static bool scene_enum_cb(void *param, obs_source_t *scene)
{
	add_child((obs_data_t *)param, obs_source_get_name(scene), make_bang());
	return true;
}

static void transition_enum_cb(obs_source_t *transition, void *param)
{
	add_child((obs_data_t *)param, obs_source_get_name(transition), make_bang());
}

static void add_name_list(obs_data_t *parent, char **names)
{
	if (!names)
		return;

	for (size_t i = 0; names[i]; i++)
		add_child(parent, names[i], make_bang());
}

/* ---- Scene items, filters, tally and hotkeys ----------------------- */

struct scene_item_roots {
	obs_data_t *visible;
	obs_data_t *locked;
	obs_data_t *order;
};

/* Per-scene walk state; scene_name is bound by the scene callback. */
struct scene_walk_ctx {
	scene_item_roots parents;
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

	add_child(ctx->parents.visible, name, make_rw_int(obs_sceneitem_visible(item) ? 1 : 0, 0, 1));
	add_child(ctx->parents.locked, name, make_rw_int(obs_sceneitem_locked(item) ? 1 : 0, 0, 1));
	add_child(ctx->parents.order, name,
		  make_rw_int(obs_sceneitem_get_order_position(item), 0, 4095, "0 = bottom"));
	return true;
}

static bool schema_scene_cb(void *param, obs_source_t *scene)
{
	auto *roots = (struct scene_item_roots *)param;

	const char *scene_name = obs_source_get_name(scene);

	obs_data_t *visible = make_container();
	obs_data_t *locked = make_container();
	obs_data_t *order = make_container();

	struct scene_walk_ctx ctx = {{visible, locked, order}, scene_name};
	obs_scene_enum_items(obs_scene_from_source(scene), schema_item_cb, &ctx);

	add_child(roots->visible, scene_name, visible);
	add_child(roots->locked, scene_name, locked);
	add_child(roots->order, scene_name, order);
	return true;
}

static void schema_filter_cb(obs_source_t *parent, obs_source_t *filter, void *param)
{
	UNUSED_PARAMETER(parent);

	add_child((obs_data_t *)param, obs_source_get_name(filter),
		  make_rw_int(obs_source_enabled(filter) ? 1 : 0, 0, 1));
}

static bool schema_filter_source_visit(obs_source_t *source, void *param)
{
	if (obs_source_filter_count(source) == 0)
		return true;

	obs_data_t *node = make_container();
	obs_source_enum_filters(source, schema_filter_cb, node);
	add_child((obs_data_t *)param, obs_source_get_name(source), node);
	return true;
}

struct tally_roots {
	obs_data_t *active;
	obs_data_t *showing;
};

static bool schema_tally_visit(obs_source_t *source, void *param)
{
	auto *roots = (struct tally_roots *)param;

	const char *name = obs_source_get_name(source);
	add_child(roots->active, name, make_ro_int(obs_source_active(source) ? 1 : 0, "In program chain"));
	add_child(roots->showing, name, make_ro_int(obs_source_showing(source) ? 1 : 0, "On screen"));
	return true;
}

static bool schema_hotkey_cb(void *data, obs_hotkey_id id, obs_hotkey_t *key)
{
	UNUSED_PARAMETER(id);

	add_child((obs_data_t *)data, obs_hotkey_get_name(key), make_bang());
	return true;
}

static void add_output_containers(obs_data_t *obs_contents)
{
	for (size_t i = 0; i < osc_addr::output_count; i++) {
		const osc_addr::output_info &out = osc_addr::outputs[i];

		obs_data_t *node = make_container(out.description);
		for (size_t k = 0; k < out.sub_count; k++) {
			if (strcmp(out.subs[k], osc_addr::sub_pause) == 0)
				add_child(node, out.subs[k],
					  make_rw_int(obs_frontend_recording_paused() ? 1 : 0, 0, 1,
						      "Pause state"));
			else
				add_child(node, out.subs[k], make_bang());
		}
		map_set(obs_contents, osc_addr::leaf(out.base), node);
	}
}

static obs_data_t *build_tree(void)
{
	obs_data_t *root = obs_data_create();
	obs_data_t *obs_contents = obs_data_create();
	obs_data_set_obj(root, contents_key, obs_contents);

	/* Scenes */
	obs_data_t *program = make_container("Switch program scene");
	obs_enum_scenes(scene_enum_cb, program);
	map_set(obs_contents, osc_addr::leaf(osc_addr::program), program);

	obs_data_t *preview = make_container("Set preview scene");
	obs_enum_scenes(scene_enum_cb, preview);
	map_set(obs_contents, osc_addr::leaf(osc_addr::preview), preview);

	/* Transitions: "go" triggers, each child selects a transition */
	obs_data_t *transition = make_container("Select transition; 'go' runs it");
	add_child(transition, osc_addr::transition_go, make_bang("Trigger transition"));
	osc_enum_transitions(transition_enum_cb, transition);
	map_set(obs_contents, osc_addr::leaf(osc_addr::transition), transition);

	/* Outputs: stream/record/replay/virtualcam from the registry */
	add_output_containers(obs_contents);

	map_set(obs_contents, osc_addr::leaf(osc_addr::studio),
		make_rw_int(obs_frontend_preview_program_mode_active() ? 1 : 0, 0, 1,
			    "Studio mode"));
	map_set(obs_contents, osc_addr::leaf(osc_addr::screenshot),
		make_bang("Take screenshot"));

	/* Sources */
	obs_data_t *mute = make_container("Mute inputs (* and ? globs work over OSC)");
	obs_data_t *volume = make_container("Volume faders, 0..1");
	obs_data_t *media = make_container("Media source transport");
	struct source_ctx ctx = {mute, volume, media};
	osc_visit_sources("*", osc_source_kind::input, source_enum_visit, &ctx);
	map_set(obs_contents, osc_addr::mute, mute);
	map_set(obs_contents, osc_addr::volume, volume);
	map_set(obs_contents, osc_addr::media, media);

	/* Scene items of every scene: visibility, lock, stacking order */
	obs_data_t *visible = make_container("Scene item visibility");
	obs_data_t *locked = make_container("Scene item lock");
	obs_data_t *order = make_container("Scene item stacking order");
	struct scene_item_roots item_roots = {visible, locked, order};
	obs_enum_scenes(schema_scene_cb, &item_roots);
	map_set(obs_contents, osc_addr::leaf(osc_addr::visible), visible);
	map_set(obs_contents, osc_addr::leaf(osc_addr::locked), locked);
	map_set(obs_contents, osc_addr::leaf(osc_addr::order), order);

	/* Filters of every source that carries any */
	obs_data_t *filters = make_container("Filter enable toggles");
	osc_visit_sources("*", osc_source_kind::any, schema_filter_source_visit, filters);
	map_set(obs_contents, osc_addr::leaf(osc_addr::filter), filters);

	/* Tally state of inputs */
	obs_data_t *active = make_container("Inputs rendered in the program chain");
	obs_data_t *showing = make_container("Inputs shown on screen at all");
	struct tally_roots tally = {active, showing};
	osc_visit_sources("*", osc_source_kind::input, schema_tally_visit, &tally);
	map_set(obs_contents, osc_addr::leaf(osc_addr::active), active);
	map_set(obs_contents, osc_addr::leaf(osc_addr::showing), showing);

	/* Hotkeys registered anywhere in OBS, triggered by exact name */
	obs_data_t *hotkeys = make_container("Trigger hotkey by OBS name");
	obs_enum_hotkeys(schema_hotkey_cb, hotkeys);
	map_set(obs_contents, osc_addr::leaf(osc_addr::hotkey_prefix), hotkeys);

	/* Profiles and scene collections. The frontend returns a single
	 * contiguous allocation (pointer array + string data): one bfree. */
	obs_data_t *profile = make_container("Switch profile");
	char **profiles = obs_frontend_get_profiles();
	add_name_list(profile, profiles);
	bfree(profiles);
	map_set(obs_contents, osc_addr::leaf(osc_addr::profile), profile);

	obs_data_t *collection = make_container("Switch scene collection");
	char **collections = obs_frontend_get_scene_collections();
	add_name_list(collection, collections);
	bfree(collections);
	map_set(obs_contents, osc_addr::leaf(osc_addr::collection), collection);

	/* Per-topic feedback registration over UDP; OSCQuery clients use
	 * the LISTEN extension (websocket) instead. */
	map_set(obs_contents, osc_addr::leaf(osc_addr::subscribe),
		make_bang("Subscribe this client to a topic: ,s <pattern> [,i port]; "
			  "wildcards * and ? allowed"));
	map_set(obs_contents, osc_addr::leaf(osc_addr::unsubscribe),
		make_bang("Unsubscribe: ,s <pattern>, or no argument for everything"));

	/* The plugin namespace starts at the OSC root, so the tree's
	 * CONTENTS live directly on the root node. */
	obs_data_release(obs_contents);

	return root;
}

/* Walks the requested path through the tree; empty result means not found.
 * Must run on the UI thread. */
static std::string walk_tree(const std::string &path)
{
	std::string body;
	obs_data_t *root = build_tree();
	obs_data_t *node = root;

	/* Descend through each node's CONTENTS per path segment. "/"
	 * serves the root itself, which now carries the whole tree. */
	bool matched = false;
	size_t start = 0;
	while (node && start <= path.size()) {
		size_t slash = path.find('/', start);
		std::string segment = slash == std::string::npos ? path.substr(start)
								 : path.substr(start, slash - start);
		if (!segment.empty()) {
			matched = true;
			obs_data_t *contents = obs_data_get_obj(node, contents_key);
			obs_data_t *child =
				contents ? obs_data_get_obj(contents, segment.c_str()) : nullptr;
			obs_data_release(contents);
			obs_data_release(node);
			node = child;
		}
		if (slash == std::string::npos)
			break;
		start = slash + 1;
	}
	if (matched && node == root)
		node = nullptr; /* segments existed but none matched */

	if (node) {
		/* Serializes into an internal cache owned by node; we copy it. */
		const char *json = obs_data_get_json(node);
		body = json ? json : "";
		obs_data_release(node);
	}

	obs_data_release(root);
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
