#include "osc-subscribe.h"

#include "osc-message.h"
#include "osc-net.h"
#include "osc-plugin.h"
#include "osc-server.h"
#include "osc-util.h"
#include "osc-ws.h"

#include <obs-module.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

/* ------------------------------------------------------------------ */
/* Registry state                                                      */
/* ------------------------------------------------------------------ */

namespace {

/* Bounds keep a hostile network from growing the state without end. */
constexpr size_t max_subscribers = 32;
constexpr size_t max_patterns = 64;
constexpr size_t max_listeners = 256;
constexpr size_t max_pattern_len = 256;

std::mutex registry_mutex;

/* Lock-free count for hot-path probes (every feedback event asks
 * "anyone listening?"); mutations publish it under registry_mutex. */
std::atomic<size_t> subscriber_count{0};

struct subscriber {
	osc_net::osc_endpoint endpoint;
	std::vector<std::string> patterns;
};

std::vector<subscriber> &subscribers()
{
	static std::vector<subscriber> s;
	return s;
}

/* Websocket peers are identified by the connection token pointer the
 * websocket server hands out; tokens are only ever resolved through
 * osc_ws::send_binary(), which no-ops once the connection is gone. */
struct listener {
	void *peer;
	std::string path;
};

std::vector<listener> &listeners()
{
	static std::vector<listener> l;
	return l;
}

/* Compares the identity-relevant bytes of two endpoints (family,
 * port, address and - for IPv6 - scope). memcmp over the whole storage
 * is not an option: padding beyond the used length is undefined. */
bool endpoint_equal(const osc_net::osc_endpoint &a, const osc_net::osc_endpoint &b)
{
	if (a.len == 0 || b.len == 0)
		return false;

	const sockaddr *sa = (const sockaddr *)&a.addr;
	const sockaddr *sb = (const sockaddr *)&b.addr;
	if (sa->sa_family != sb->sa_family)
		return false;

	if (sa->sa_family == AF_INET) {
		const auto *ia = (const sockaddr_in *)sa;
		const auto *ib = (const sockaddr_in *)sb;
		return ia->sin_port == ib->sin_port &&
		       memcmp(&ia->sin_addr, &ib->sin_addr, sizeof(in_addr)) == 0;
	}
	if (sa->sa_family == AF_INET6) {
		const auto *ia = (const sockaddr_in6 *)sa;
		const auto *ib = (const sockaddr_in6 *)sb;
		return ia->sin6_port == ib->sin6_port && ia->sin6_scope_id == ib->sin6_scope_id &&
		       memcmp(&ia->sin6_addr, &ib->sin6_addr, sizeof(in6_addr)) == 0;
	}

	return false;
}

void endpoint_set_port(osc_net::osc_endpoint &ep, int port)
{
	auto *sa = (sockaddr *)&ep.addr;
	if (sa->sa_family == AF_INET)
		((sockaddr_in *)sa)->sin_port = htons((uint16_t)port);
	else if (sa->sa_family == AF_INET6)
		((sockaddr_in6 *)sa)->sin6_port = htons((uint16_t)port);
}

} // namespace

/* ------------------------------------------------------------------ */
/* UDP topic subscriptions                                             */
/* ------------------------------------------------------------------ */

void osc_subscribe_add(const osc_net::osc_endpoint &who, const std::string &pattern, int port)
{
	if (pattern.empty() || pattern.size() > max_pattern_len) {
		blog(LOG_WARNING, "[obs-osc] subscribe: invalid pattern");
		return;
	}

	std::lock_guard<std::mutex> lock(registry_mutex);

	for (subscriber &sub : subscribers()) {
		if (!endpoint_equal(sub.endpoint, who))
			continue;

		if (port > 0)
			endpoint_set_port(sub.endpoint, port);

		if (std::find(sub.patterns.begin(), sub.patterns.end(), pattern) ==
		    sub.patterns.end()) {
			if (sub.patterns.size() >= max_patterns) {
				blog(LOG_WARNING, "[obs-osc] subscribe: pattern limit reached");
				return;
			}
			sub.patterns.push_back(pattern);
		}
		return;
	}

	if (subscribers().size() >= max_subscribers) {
		blog(LOG_WARNING, "[obs-osc] subscribe: subscriber limit reached");
		return;
	}

	subscriber sub;
	sub.endpoint = who;
	if (port > 0)
		endpoint_set_port(sub.endpoint, port);
	sub.patterns.push_back(pattern);
	subscribers().push_back(std::move(sub));
	subscriber_count.fetch_add(1, std::memory_order_relaxed);
}

void osc_subscribe_remove(const osc_net::osc_endpoint &who, const char *pattern)
{
	std::lock_guard<std::mutex> lock(registry_mutex);

	auto sub = subscribers().begin();
	while (sub != subscribers().end()) {
		if (!endpoint_equal(sub->endpoint, who)) {
			++sub;
			continue;
		}

		if (pattern) {
			auto p = std::find(sub->patterns.begin(), sub->patterns.end(),
					   std::string(pattern));
			if (p != sub->patterns.end())
				sub->patterns.erase(p);
		} else {
			sub->patterns.clear();
		}

		if (sub->patterns.empty()) {
			sub = subscribers().erase(sub);
			subscriber_count.fetch_sub(1, std::memory_order_relaxed);
		} else
			++sub;
		break; /* endpoints are unique in the registry */
	}
}

bool osc_has_subscribers(void)
{
	return subscriber_count.load(std::memory_order_relaxed) != 0;
}

/* ---- OSCQuery LISTEN registrations ----------------------------------- */

void osc_listen_add(void *peer, const std::string &path)
{
	if (path.empty() || path.size() > max_pattern_len)
		return;

	std::lock_guard<std::mutex> lock(registry_mutex);

	for (const listener &l : listeners()) {
		if (l.peer == peer && l.path == path)
			return;
	}
	if (listeners().size() >= max_listeners) {
		blog(LOG_WARNING, "[obs-osc] LISTEN: listener limit reached");
		return;
	}
	listeners().push_back({peer, path});
}

void osc_listen_remove(void *peer, const std::string &path)
{
	std::lock_guard<std::mutex> lock(registry_mutex);

	for (auto l = listeners().begin(); l != listeners().end(); ++l) {
		if (l->peer == peer && l->path == path) {
			listeners().erase(l);
			return;
		}
	}
}

void osc_listen_remove_peer(void *peer)
{
	std::lock_guard<std::mutex> lock(registry_mutex);

	for (auto l = listeners().begin(); l != listeners().end();) {
		if (l->peer == peer)
			l = listeners().erase(l);
		else
			++l;
	}
}

void osc_handle_subscription(const osc_message &msg, const osc_net::osc_endpoint &who,
			     bool unsubscribe)
{
	if (unsubscribe) {
		if (!msg.args.empty() && msg.args[0].type == 's')
			osc_subscribe_remove(who, msg.args[0].s.c_str());
		else
			osc_subscribe_remove(who, nullptr);
		return;
	}

	/* subscribe: ,s <pattern> [,i port] */
	if (msg.args.empty() || msg.args[0].type != 's' || msg.args[0].s.empty()) {
		blog(LOG_WARNING, "[obs-osc] /subscribe needs a string topic pattern");
		return;
	}

	int port = 0;
	if (msg.args.size() >= 2) {
		const int32_t want = msg.arg_int(1, 0);
		port = (want > 0 && want <= 65535) ? (int)want : 0;
		if (port == 0)
			blog(LOG_WARNING, "[obs-osc] ignoring invalid delivery port %d", want);
	}

	osc_subscribe_add(who, msg.args[0].s, port);
	blog(LOG_INFO, "[obs-osc] subscription added for pattern '%s'%s",
	     msg.args[0].s.c_str(), port > 0 ? " (redirected port)" : "");
}

/* ------------------------------------------------------------------ */
/* Fan-out                                                             */
/* ------------------------------------------------------------------ */

int osc_publish(const char *address, const uint8_t *data, size_t size)
{
	std::vector<osc_net::osc_endpoint> targets;
	std::vector<void *> peers;

	{
		std::lock_guard<std::mutex> lock(registry_mutex);

		for (const subscriber &sub : subscribers()) {
			for (const std::string &pattern : sub.patterns) {
				if (osc_glob_match(pattern.c_str(), address)) {
					targets.push_back(sub.endpoint);
					break;
				}
			}
		}

		for (const listener &l : listeners()) {
			if (l.path == address)
				peers.push_back(l.peer);
		}
	}

	for (const osc_net::osc_endpoint &ep : targets)
		g_server.send_to(data, size, ep);
	for (void *peer : peers)
		osc_ws::send_binary(peer, data, size);

	return (int)targets.size();
}
