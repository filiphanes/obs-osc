#include "osc-util.h"

#include <obs-frontend-api.h>

#include <cmath>
#include <cstdlib>
#include <cstring>

obs_source_t *osc_source_of_type(const char *name, enum obs_source_type type)
{
	obs_source_t *source = obs_get_source_by_name(name);
	if (!source)
		return nullptr;

	if (obs_source_get_type(source) != type) {
		obs_source_release(source);
		return nullptr;
	}
	return source;
}

/* Shell-style glob with '*' and '?' for mute patterns. Iterative
 * two-pointer form: O(n*m) worst case instead of the recursive form's
 * exponential backtracking, so attacker-controlled /subscribe patterns
 * cannot burn signal-thread time. */
bool osc_glob_match(const char *pattern, const char *str)
{
	const char *restart_pattern = nullptr;
	const char *restart_str = nullptr;

	while (*str) {
		if (*pattern == '?' || *pattern == *str) {
			pattern++;
			str++;
		} else if (*pattern == '*') {
			restart_pattern = ++pattern;
			restart_str = str;
		} else if (restart_pattern) {
			pattern = restart_pattern;
			str = ++restart_str;
		} else {
			return false;
		}
	}

	while (*pattern == '*')
		pattern++;
	return !*pattern;
}

bool osc_has_wildcard(const char *pattern)
{
	return pattern && (strchr(pattern, '*') || strchr(pattern, '?'));
}

bool osc_index_segment(const char *segment)
{
	if (!*segment)
		return false;

	for (const char *c = segment; *c; c++) {
		if (*c < '0' || *c > '9')
			return false;
	}
	return true;
}

bool osc_selector_matches(const char *selector, const char *name, size_t index)
{
	return osc_index_segment(selector) ? (size_t)atoi(selector) == index : osc_glob_match(selector, name);
}

void osc_visit_sources(const char *pattern, osc_source_kind kind,
		       bool (*visit)(obs_source_t *source, void *param), void *param)
{
	struct visit_ctx {
		const char *pattern;
		osc_source_kind kind;
		bool (*visit)(obs_source_t *, void *);
		void *param;
	} ctx = {pattern, kind, visit, param};

	/* Exact names hit libobs' source hash table directly instead of
	 * walking every registered source; hardware controllers address
	 * sources by name almost exclusively. */
	if (!osc_has_wildcard(pattern)) {
		obs_source_t *source = obs_get_source_by_name(pattern);
		if (!source)
			return;

		bool type_ok;
		switch (kind) {
		case osc_source_kind::input:
			type_ok = osc_is_input(source);
			break;
		case osc_source_kind::scene:
			type_ok = obs_source_get_type(source) == OBS_SOURCE_TYPE_SCENE;
			break;
		default:
			type_ok = true;
			break;
		}

		if (type_ok)
			visit(source, param);
		obs_source_release(source);
		return;
	}

	obs_enum_sources(
		[](void *p, obs_source_t *source) {
			auto *c = (struct visit_ctx *)p;

			bool type_ok;
			switch (c->kind) {
			case osc_source_kind::input:
				type_ok = osc_is_input(source);
				break;
			case osc_source_kind::scene:
				type_ok = obs_source_get_type(source) == OBS_SOURCE_TYPE_SCENE;
				break;
			default:
				type_ok = true;
				break;
			}

			if (type_ok && osc_glob_match(c->pattern, obs_source_get_name(source)))
				return c->visit(source, c->param);
			return true;
		},
		&ctx);
}

/* Per-scene walk state shared by the exact-name and glob paths of
 * osc_visit_scene_items(). */
struct scene_ctx {
	const char *scene_pattern;
	const char *item_selector;
	bool (*visit)(const char *, obs_sceneitem_t *, void *);
	void *param;
	const char *scene_name; /* bound while walking */
};

/* Item walk shared by the exact-name and glob paths; scene_name is
 * bound by the caller. */
static bool visit_item_cb(obs_scene_t *scene, obs_sceneitem_t *item, void *param)
{
	UNUSED_PARAMETER(scene);
	auto *ic = (struct scene_ctx *)param;

	obs_source_t *source = obs_sceneitem_get_source(item);
	if (!source)
		return true;

	if (ic->item_selector &&
	    !osc_selector_matches(ic->item_selector, obs_source_get_name(source),
				  (size_t)obs_sceneitem_get_order_position(item)))
		return true;

	return ic->visit(ic->scene_name, item, ic->param);
}

static void enum_items_of_scene(obs_source_t *scene_source, struct scene_ctx *ctx)
{
	ctx->scene_name = obs_source_get_name(scene_source);
	obs_scene_enum_items(obs_scene_from_source(scene_source), visit_item_cb, ctx);
}

void osc_visit_scene_items(const char *scene_pattern, const char *item_selector,
			   bool (*visit)(const char *scene_name, obs_sceneitem_t *item, void *param),
			   void *param)
{
	scene_ctx ctx = {scene_pattern, item_selector, visit, param, nullptr};

	/* One scene's items are walked directly when the scene is named
	 * exactly; only globs pay for the global scene enumeration. */
	if (!osc_has_wildcard(scene_pattern)) {
		obs_source_t *scene_source = obs_get_source_by_name(scene_pattern);
		if (!scene_source)
			return;

		if (obs_source_get_type(scene_source) == OBS_SOURCE_TYPE_SCENE)
			enum_items_of_scene(scene_source, &ctx);
		obs_source_release(scene_source);
		return;
	}

	obs_enum_scenes(
		[](void *p, obs_source_t *scene_source) {
			auto *c = (struct scene_ctx *)p;

			if (!osc_glob_match(c->scene_pattern, obs_source_get_name(scene_source)))
				return true;
			enum_items_of_scene(scene_source, c);
			return true;
		},
		&ctx);
}

void osc_enum_transitions(void (*cb)(obs_source_t *transition, void *param), void *param)
{
	struct obs_frontend_source_list transitions = {};
	obs_frontend_get_transitions(&transitions);

	for (size_t i = 0; i < transitions.sources.num; i++)
		cb(transitions.sources.array[i], param);

	obs_frontend_source_list_free(&transitions);
}

float osc_volume_multiplier(float position)
{
	if (position < 0.0f)
		position = 0.0f;
	if (position > 1.0f)
		position = 1.0f;
	return position * position; /* perceptual curve, clamped to [0,16] */
}

float osc_volume_position(float multiplier)
{
	if (multiplier < 0.0f)
		multiplier = 0.0f;
	if (multiplier > 16.0f)
		multiplier = 16.0f;
	return sqrtf(multiplier);
}

/* ------------------------------------------------------------------ */
/* Scene-item transform fields                                         */
/* ------------------------------------------------------------------ */

static float field_pos_x(const obs_sceneitem_t *item)
{
	struct vec2 pos;

	obs_sceneitem_get_pos(item, &pos);
	return pos.x;
}

static void field_pos_x_set(obs_sceneitem_t *item, float value)
{
	struct vec2 pos;

	obs_sceneitem_get_pos(item, &pos);
	pos.x = value;
	obs_sceneitem_set_pos(item, &pos);
}

static float field_pos_y(const obs_sceneitem_t *item)
{
	struct vec2 pos;

	obs_sceneitem_get_pos(item, &pos);
	return pos.y;
}

static void field_pos_y_set(obs_sceneitem_t *item, float value)
{
	struct vec2 pos;

	obs_sceneitem_get_pos(item, &pos);
	pos.y = value;
	obs_sceneitem_set_pos(item, &pos);
}

static float field_scale_x(const obs_sceneitem_t *item)
{
	struct vec2 scale;

	obs_sceneitem_get_scale(item, &scale);
	return scale.x;
}

static void field_scale_x_set(obs_sceneitem_t *item, float value)
{
	struct vec2 scale;

	obs_sceneitem_get_scale(item, &scale);
	scale.x = value;
	obs_sceneitem_set_scale(item, &scale);
}

static float field_scale_y(const obs_sceneitem_t *item)
{
	struct vec2 scale;

	obs_sceneitem_get_scale(item, &scale);
	return scale.y;
}

static void field_scale_y_set(obs_sceneitem_t *item, float value)
{
	struct vec2 scale;

	obs_sceneitem_get_scale(item, &scale);
	scale.y = value;
	obs_sceneitem_set_scale(item, &scale);
}

static float field_rotation_get(const obs_sceneitem_t *item)
{
	return obs_sceneitem_get_rot(item);
}

static void field_rotation_set(obs_sceneitem_t *item, float value)
{
	obs_sceneitem_set_rot(item, value);
}

#define OSC_CROP_FIELD(name, side)                                                      \
	static float field_crop_##name(const obs_sceneitem_t *item)                     \
	{                                                                               \
		struct obs_sceneitem_crop crop;                                         \
		obs_sceneitem_get_crop(item, &crop);                                    \
		return (float)crop.side;                                                \
	}                                                                               \
	static void field_crop_##name##_set(obs_sceneitem_t *item, float value)         \
	{                                                                               \
		struct obs_sceneitem_crop crop;                                         \
		obs_sceneitem_get_crop(item, &crop);                                    \
		crop.side = (int)value;                                                 \
		obs_sceneitem_set_crop(item, &crop);                                    \
	}

OSC_CROP_FIELD(left, left)
OSC_CROP_FIELD(top, top)
OSC_CROP_FIELD(right, right)
OSC_CROP_FIELD(bottom, bottom)

#undef OSC_CROP_FIELD

const struct osc_transform_field osc_transform_fields[osc_transform_field_count] = {
	{"x", field_pos_x, field_pos_x_set},           {"y", field_pos_y, field_pos_y_set},
	{"sx", field_scale_x, field_scale_x_set},       {"sy", field_scale_y, field_scale_y_set},
	{"rot", field_rotation_get, field_rotation_set}, {"crop_left", field_crop_left, field_crop_left_set},
	{"crop_top", field_crop_top, field_crop_top_set}, {"crop_right", field_crop_right, field_crop_right_set},
	{"crop_bottom", field_crop_bottom, field_crop_bottom_set},
};
