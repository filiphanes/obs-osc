#pragma once

/*
 * Change-notification registries and the single fan-out point for
 * unsolicited feedback:
 *
 *  - UDP topic subscriptions: a controller registers glob patterns via
 *    /subscribe; matching messages are delivered to its endpoint
 *    as ordinary OSC datagrams.
 *  - OSCQuery LISTEN: websocket peers pin exact addresses; matching
 *    messages are streamed to them as binary websocket frames.
 *
 * All state is mutex-guarded. Delivery works on snapshots so publisher
 * threads (OBS signals, UI thread, network thread) never hold registry
 * locks while writing to sockets.
 */

#include <cstddef>
#include <string>

namespace osc_net {
struct osc_endpoint;
}

struct osc_message;

/* ---- UDP topic subscriptions ----------------------------------------- */

/* Adds pattern to the sender's subscription set (deduplicated). A port
 * in [1, 65535] redirects delivery to that port while keeping the
 * sender's IP, for clients listening on another local port. */
void osc_subscribe_add(const osc_net::osc_endpoint &who, const std::string &pattern, int port);

/* Removes one pattern from the sender's set; pattern == null removes
 * the client entirely. Unknown patterns or clients are ignored. */
void osc_subscribe_remove(const osc_net::osc_endpoint &who, const char *pattern);

/* True when at least one UDP topic subscription exists. While it does,
 * the legacy reply-to-last-controller broadcast is suspended so that
 * selective filtering is not defeated by implicit full feedback. */
bool osc_has_subscribers(void);

/* ---- OSCQuery LISTEN registrations ----------------------------------- */

/* Pins an exact address for a websocket peer (token owned by the
 * websocket server). Unknown paths are accepted and simply never fire;
 * duplicates are ignored. */
void osc_listen_add(void *peer, const std::string &path);

/* Drops one path of a peer. */
void osc_listen_remove(void *peer, const std::string &path);

/* Drops every registration of a peer (websocket disconnect). */
void osc_listen_remove_peer(void *peer);

/* Executes /subscribe and /unsubscribe messages. Runs on the
 * UI thread like every other dispatched command. */
void osc_handle_subscription(const osc_message &msg, const osc_net::osc_endpoint &who,
			     bool unsubscribe);

/* ---- fan-out ----------------------------------------------------------- */

/* Delivers one already encoded OSC message: address is matched against
 * every subscriber pattern (UDP datagrams) and listener path (websocket
 * binary frames). Returns the number of UDP subscribers served. */
int osc_publish(const char *address, const uint8_t *data, size_t size);
