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
constexpr char root[] = "/obs/";

/* ---- Bare control addresses (state carried as arguments) ------------ */

constexpr char studio[] = "/obs/studio";
constexpr char screenshot[] = "/obs/screenshot";
constexpr char transition[] = "/obs/transition"; /* no args = poll; with args = trigger */
constexpr char program[] = "/obs/program"; /* feedback + poll */
constexpr char preview[] = "/obs/preview"; /* feedback + poll */

/* Route prefixes that carry a payload after them. */
constexpr char program_prefix[] = "/obs/program/";
constexpr char program_index_prefix[] = "/obs/program/index/";
constexpr char preview_prefix[] = "/obs/preview/";
constexpr char transition_prefix[] = "/obs/transition/";
constexpr char mute_prefix[] = "/obs/mute/";
constexpr char volume_prefix[] = "/obs/volume/";
constexpr char db_prefix[] = "/obs/db/";
constexpr char media_prefix[] = "/obs/media/";
constexpr char profile_prefix[] = "/obs/profile/";
constexpr char collection_prefix[] = "/obs/collection/";

/* Bare collection routes: no argument polls the current selection. */
constexpr char profile[] = "/obs/profile";
constexpr char collection[] = "/obs/collection";

/* Per-topic feedback registration; see osc-subscribe.h. */
constexpr char subscribe[] = "/obs/subscribe";
constexpr char unsubscribe[] = "/obs/unsubscribe";

/* The trigger word inside the transition route. */
constexpr char transition_go[] = "go";

/* ---- Outputs: "<base>/<sub>" ----------------------------------------- */

constexpr char stream[] = "/obs/stream";
constexpr char record[] = "/obs/record";
constexpr char record_paused[] = "/obs/record/paused"; /* feedback + poll */
constexpr char replay[] = "/obs/replay";
constexpr char replay_saved[] = "/obs/replay/saved"; /* feedback only (momentary) */
constexpr char virtualcam[] = "/obs/virtualcam";

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

/* Sub-path after a base route: "stop" for ("/obs/stream/stop", stream),
 * "" for the bare base, nullptr when not under base. */
const char *sub_action(const std::string &address, const char *base);

/* Text following a prefix route: "Mic" for ("/obs/mute/Mic",
 * mute_prefix), nullptr when not prefixed. */
const char *tail(const std::string &address, const char *prefix);

/* Last path segment of an absolute address: "stream" for "/obs/stream".
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

} // namespace osc_addr
