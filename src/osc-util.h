#pragma once

/*
 * Small helpers shared across modules: input-source classification,
 * glob matching for mute patterns, frontend transition iteration and
 * the perceptual fader <-> multiplier volume mapping.
 */

#include <obs.h>

/* True for input sources - the only kind this plugin controls. */
inline bool osc_is_input(obs_source_t *source)
{
	return obs_source_get_type(source) == OBS_SOURCE_TYPE_INPUT;
}

/* Returns an add-ref'd source of the given type looked up by name, or
 * NULL. Release after use. */
obs_source_t *osc_source_of_type(const char *name, enum obs_source_type type);

/* Shell-style glob with '*' and '?' wildcards. */
bool osc_glob_match(const char *pattern, const char *str);

/* Calls cb with each currently configured transition (borrowed refs,
 * UI thread only). */
void osc_enum_transitions(void (*cb)(obs_source_t *transition, void *param), void *param);

/* Perceptual fader mapping between a 0..1 fader position and the OBS
 * volume multiplier (clamped to [0, 16]). */
float osc_volume_position(float multiplier);
float osc_volume_multiplier(float position);
