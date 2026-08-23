#pragma once

/*
 * Object-access layer: one home for reading and writing the libobs
 * objects behind the routes added after the original control verbs -
 * scene items (visibility, lock, stacking order, transform), filters
 * and their settings parameters, input tally state, and hotkeys.
 *
 * Dispatch (osc-controls.cpp), feedback (osc-feedback.cpp) and the
 * OSCQuery schema stay free of object walking; every function here
 * must run on the UI thread. Selectors follow one rule across all
 * routes: a digits-only segment addresses an array element by position
 * (filters: chain order; scene items: stacking order, 0 = bottom),
 * anything else matches a name with '*'/'?' globs.
 */

#include "osc-message.h"
#include "osc-net.h"
#include "osc-util.h"

namespace osc_objects {

/* The boolean scene-item properties addressable over OSC. */
enum class item_prop { visible, locked };

/* Shows or hides / locks or unlocks every matched item. value == null
 * toggles each match individually. */
void set_item_flag(const char *scene_pattern, const char *item_selector, item_prop prop, const bool *value);

/* Answers one reply per matched item with its current state. */
void poll_item_flag(const osc_net::osc_endpoint &to, const char *scene_pattern, const char *item_selector,
		    item_prop prop);

/* Moves every matched item to the same position (0 = bottom); with
 * multiple matches the final stacking is undefined - prefer exact
 * selectors. */
void set_item_order(const char *scene_pattern, const char *item_selector, int position);

/* Answers one reply per matched item with its stacking position. */
void poll_item_order(const osc_net::osc_endpoint &to, const char *scene_pattern, const char *item_selector);

/* Writes one transform field of every matched item. */
void write_transform(const char *scene_pattern, const char *item_selector, const char *field_name, float value);

/* Answers replies for one transform field, or every field when
 * field_name is null; item_selector may be null to cover all items. */
void poll_transform(const osc_net::osc_endpoint &to, const char *scene_pattern, const char *item_selector,
		    const char *field_name);

/* Enables/disables every matched filter of any source type (scenes
 * included). value == null toggles each match individually. */
void set_filter_enabled(const char *source_pattern, const char *selector, const bool *value);

/* Answers one reply per matched filter with its enabled state. */
void poll_filter_enabled(const osc_net::osc_endpoint &to, const char *source_pattern, const char *selector);

/* Reports every scalar settings parameter of each matched filter;
 * non-scalar property types are skipped silently. */
void poll_filter_params(const osc_net::osc_endpoint &to, const char *source_pattern, const char *selector);

/* Reads or writes one settings parameter addressed by its property
 * name. Only scalar property types map to OSC arguments; anything
 * else is rejected with a warning. */
void poll_filter_param(const osc_net::osc_endpoint &to, const char *source_pattern, const char *selector,
		       const char *param);
void write_filter_param(const osc_message &msg, const char *source_pattern, const char *selector,
			const char *param);

/* Answers once per matching input with its tally state: rendered in
 * the program chain ('active') or shown on screen at all ('showing'). */
void poll_input_state(const osc_net::osc_endpoint &to, const char *pattern, bool showing);

/* Press-and-release of the first hotkey registered under this exact
 * OBS hotkey name (e.g. "OBSBasic.StartRecording"). */
void trigger_hotkey(const char *name);

} // namespace osc_objects
