#include "osc-addresses.h"

namespace osc_addr {

namespace {
const char *const stream_subs[] = {sub_start, sub_stop, sub_toggle};
const char *const record_subs[] = {sub_start, sub_stop, sub_toggle, sub_pause};
const char *const replay_subs[] = {sub_start, sub_stop, sub_save};
const char *const virtualcam_subs[] = {sub_start, sub_stop};
} // namespace

const output_info outputs[output_count] = {
	{output_id::stream, stream, "Streaming control", stream_subs, 3},
	{output_id::record, record, "Recording control", record_subs, 4},
	{output_id::replay, replay, "Replay buffer control", replay_subs, 3},
	{output_id::virtualcam, virtualcam, "Virtual camera control", virtualcam_subs, 2},
};

const media_verb media_verbs[media_verb_count] = {
	{"play", media_action::play},      {"pause", media_action::pause},
	{"toggle", media_action::toggle},  {"stop", media_action::stop},
	{"restart", media_action::restart}, {"next", media_action::next},
	{"prev", media_action::prev},
};

const char *sub_action(const std::string &address, const char *base)
{
	const size_t base_len = strlen(base);

	if (address.compare(0, base_len, base) != 0)
		return nullptr;
	if (address.size() == base_len)
		return "";
	if (address[base_len] == '/')
		return address.c_str() + base_len + 1;
	return nullptr;
}

const char *tail(const std::string &address, const char *prefix)
{
	const size_t prefix_len = strlen(prefix);

	if (address.size() <= prefix_len || address.compare(0, prefix_len, prefix) != 0)
		return nullptr;
	return address.c_str() + prefix_len;
}

const char *leaf(const char *address)
{
	const char *slash = strrchr(address, '/');
	return slash ? slash + 1 : address;
}

} // namespace osc_addr
