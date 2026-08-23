#pragma once

/*
 * Single source of truth for the plugin's OSC address namespace.
 *
 * Inbound dispatch (osc-controls.cpp), outbound feedback
 * (osc-feedback.cpp) and the OSCQuery schema (osc-query-schema.cpp)
 * all derive their routes from these definitions. Never hard-code an
 * address or verb anywhere else. README.md documents the same
 * namespace for users.
 */

#include <cstddef>
#include <cstring>
#include <string>

namespace osc_addr {

/* Root of the whole namespace; every route starts with it. */
constexpr char root[] = "/";

/* ---- Bare control addresses (state carried as arguments) ------------ */

constexpr char studio[] = "/studio";
constexpr char screenshot[] = "/screenshot";
constexpr char transition[] = "/transition"; /* no args = poll; with args = trigger */
constexpr char program[] = "/program"; /* feedback + poll */
constexpr char preview[] = "/preview"; /* feedback + poll */

/* Route prefixes that carry a payload after them. */
constexpr char program_prefix[] = "/program/";
constexpr char program_index_prefix[] = "/program/index/";
constexpr char preview_prefix[] = "/preview/";
constexpr char transition_prefix[] = "/transition/";
constexpr char mute_prefix[] = "/mute/";
constexpr char volume_prefix[] = "/volume/";
constexpr char db_prefix[] = "/db/";
constexpr char media_prefix[] = "/media/";
constexpr char profile_prefix[] = "/profile/";
constexpr char collection_prefix[] = "/collection/";

/* Two-level object routes "<parent>/<child>": scenes hold items,
 * sources hold filters. Both segments accept * and ? globs. */
constexpr char visible_prefix[] = "/visible/";
constexpr char locked_prefix[] = "/locked/";
constexpr char order_prefix[] = "/order/";
constexpr char filter_prefix[] = "/filter/";

/* Read-only tally of inputs: rendered in the program chain / on
 * screen at all. Feedback and poll only. */
constexpr char active_prefix[] = "/active/";
constexpr char showing_prefix[] = "/showing/";

/* One-shot hotkey trigger by exact OBS hotkey name; no poll. */
constexpr char hotkey_prefix[] = "/hotkey/";

/* Indexed structure routes "<container>/<index-or-name>/<field>".
 * Digits-only segments select an array element by position (filters:
 * chain order; scene items: stacking order, 0 = bottom), anything
 * else matches a name/glob. These routes are intentionally absent
 * from the OSCQuery tree: indexes shift as arrays change, discovery
 * happens through the name-based routes instead. */
constexpr char transform_prefix[] = "/transform/";

/* Bare collection routes: no argument polls the current selection. */
constexpr char profile[] = "/profile";
constexpr char collection[] = "/collection";

/* Bare object routes: no argument polls every child of every parent
 * (e.g. /visible answers once per item of every scene). */
constexpr char visible[] = "/visible";
constexpr char locked[] = "/locked";
constexpr char order[] = "/order";
constexpr char filter[] = "/filter";
constexpr char active[] = "/active";
constexpr char showing[] = "/showing";
constexpr char transform[] = "/transform";

/* Pseudo-field listing every scalar settings parameter of one filter. */
constexpr char filter_params_node[] = "params";

/* Per-topic feedback registration; see osc-subscribe.h. */
constexpr char subscribe[] = "/subscribe";
constexpr char unsubscribe[] = "/unsubscribe";

/* The trigger word inside the transition route. */
constexpr char transition_go[] = "go";

/* ---- Outputs: "<base>/<sub>" ----------------------------------------- */

constexpr char stream[] = "/stream";
constexpr char record[] = "/record";
constexpr char record_paused[] = "/record/paused"; /* feedback + poll */
constexpr char replay[] = "/replay";
constexpr char replay_saved[] = "/replay/saved"; /* feedback only (momentary) */
constexpr char virtualcam[] = "/virtualcam";

/* Output sub-actions. */
constexpr char sub_start[] = "start";
constexpr char sub_stop[] = "stop";
constexpr char sub_toggle[] = "toggle";
constexpr char sub_save[] = "save";
constexpr char sub_pause[] = "pause";

/* Identifies an output control so dispatch can bind behavior. */
enum class output_id { stream, record, replay, virtualcam };

inline constexpr size_t output_count = 4;

/* One entry per output control, consumed by both the dispatcher and
 * the OSCQuery tree so they can never drift apart. "description" is
 * only used by the OSCQuery schema. */
struct output_info {
	output_id id;
	const char *base;
	const char *description;
	const char *const *subs;
	size_t sub_count;
};

extern const output_info outputs[output_count];

/* ---- Media transport: "<media_prefix><name>/<verb>" ------------------ */

enum class media_action { play, pause, toggle, stop, restart, next, prev };

struct media_verb {
	const char *name;
	media_action action;
};

inline constexpr size_t media_verb_count = 7;

extern const media_verb media_verbs[media_verb_count];

/* Child node under "<media_prefix><name>" reporting playback state. */
constexpr char media_state_node[] = "state";

/* Route segments without a bare address of their own (they always
 * carry a payload after them). Used by the OSCQuery tree for
 * container naming; the matching route prefixes are above. Profile
 * and scene collection have bare poll addresses now, so their
 * containers name themselves after osc_addr::profile/collection via
 * osc_addr::leaf(). */
constexpr char mute[] = "mute";
constexpr char volume[] = "volume";
constexpr char db[] = "db";
constexpr char media[] = "media";

/* ---- Routing helpers -------------------------------------------------- */

/* True when address starts with prefix. */
inline bool has_prefix(const std::string &address, const char *prefix)
{
	return address.compare(0, strlen(prefix), prefix) == 0;
}

/* Sub-path after a base route: "stop" for ("/stream/stop", stream),
 * "" for the bare base, nullptr when not under base. */
const char *sub_action(const std::string &address, const char *base);

/* Text following a prefix route: "Mic" for ("/mute/Mic",
 * mute_prefix), nullptr when not prefixed. */
const char *tail(const std::string &address, const char *prefix);

/* Last path segment of an absolute address: "stream" for "/stream".
 * Used to name OSCQuery containers after their routes. */
const char *leaf(const char *address);

/* ---- Address builders for outgoing feedback --------------------------- */

inline std::string prefixed(const char *prefix, const char *name)
{
	return std::string(prefix) + name;
}

inline std::string mute_for(const char *source_name)
{
	return prefixed(mute_prefix, source_name);
}

inline std::string volume_for(const char *source_name)
{
	return prefixed(volume_prefix, source_name);
}

inline std::string media_state_for(const char *source_name)
{
	return prefixed(media_prefix, source_name) + "/" + media_state_node;
}

/* Two-segment builders: <prefix><parent>/<child>. The parent name must
 * not contain '/' (see README limitations); the child may. */
inline std::string joined_for(const char *prefix, const char *parent, const char *child)
{
	return prefixed(prefix, parent) + "/" + child;
}

inline std::string joined3_for(const char *prefix, const char *a, const char *b, const char *c)
{
	return prefixed(prefix, a) + "/" + b + "/" + c;
}

inline std::string visible_for(const char *scene_name, const char *item_name)
{
	return joined_for(visible_prefix, scene_name, item_name);
}

inline std::string locked_for(const char *scene_name, const char *item_name)
{
	return joined_for(locked_prefix, scene_name, item_name);
}

inline std::string order_for(const char *scene_name, const char *item_name)
{
	return joined_for(order_prefix, scene_name, item_name);
}

inline std::string filter_for(const char *source_name, const char *filter_name)
{
	return joined_for(filter_prefix, source_name, filter_name);
}

inline std::string transform_for(const char *scene_name, const char *item_name, const char *field_name)
{
	return joined3_for(transform_prefix, scene_name, item_name, field_name);
}

} // namespace osc_addr
