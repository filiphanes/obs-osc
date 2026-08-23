#pragma once

/*
 * Small helpers shared across modules: input-source classification,
 * glob matching, source/scene-item visitors for the object routes,
 * index-or-name selector resolution, frontend transition iteration,
 * the perceptual fader <-> multiplier volume mapping, and the scalar
 * scene-item transform field table.
 */

#include <cstddef>
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

/* True when the segment addresses an array element by position instead
 * of a name: "2" selects the third filter or scene item, while "Mic"
 * stays a name/glob. Sources named purely digits cannot be addressed
 * as names on routes using selectors. */
bool osc_index_segment(const char *segment);

/* Resolves one selector against a name and its array position:
 * digits compare the position, anything else globs the name. */
bool osc_selector_matches(const char *selector, const char *name, size_t index);

/* Source kinds accepted by osc_visit_sources. */
enum class osc_source_kind { any, input, scene };

/* Calls visit once per non-private source of the given kind whose name
 * matches the glob. Borrowed refs; UI thread only. */
void osc_visit_sources(const char *pattern, osc_source_kind kind,
		       bool (*visit)(obs_source_t *source, void *param), void *param);

/* Calls visit once per item of every scene whose name matches the
 * scene pattern. When item_selector is null every item is visited;
 * otherwise it resolves per item via osc_selector_matches against the
 * stacking order (digits) or the item's source name (glob).
 * Borrowed refs; UI thread only. */
void osc_visit_scene_items(const char *scene_pattern, const char *item_selector,
			   bool (*visit)(const char *scene_name, obs_sceneitem_t *item, void *param),
			   void *param);

/* Calls cb with each currently configured transition (borrowed refs,
 * UI thread only). */
void osc_enum_transitions(void (*cb)(obs_source_t *transition, void *param), void *param);

/* Perceptual fader mapping between a 0..1 fader position and the OBS
 * volume multiplier (clamped to [0, 16]). */
float osc_volume_position(float multiplier);
float osc_volume_multiplier(float position);

/* One entry per scalar scene-item transform property, addressable as
 * "/transform/<scene>/<item>/<field>". Shared by the control plane
 * (lookup by field name) and the feedback plane (reporting every
 * field after a transform change). */
struct osc_transform_field {
	const char *name;
	float (*get)(const obs_sceneitem_t *item);
	void (*set)(obs_sceneitem_t *item, float value);
};

inline constexpr size_t osc_transform_field_count = 9;

extern const struct osc_transform_field osc_transform_fields[osc_transform_field_count];
