#include "osc-util.h"

#include <obs-frontend-api.h>

#include <cmath>
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

/* Shell-style glob with '*' and '?' for mute patterns. */
bool osc_glob_match(const char *pattern, const char *str)
{
	if (!*pattern)
		return !*str;
	if (*pattern == '*') {
		for (const char *s = str;; s++) {
			if (osc_glob_match(pattern + 1, s))
				return true;
			if (!*s)
				return false;
		}
	}
	if (*str && (*pattern == '?' || *pattern == *str))
		return osc_glob_match(pattern + 1, str + 1);
	return false;
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
