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
