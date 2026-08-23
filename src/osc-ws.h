#pragma once

/*
 * Minimal server-side RFC 6455 websocket support, embedded in the
 * OSCQuery HTTP port so the LISTEN/IGNORE commands of the proposal
 * work exactly as specified: peers subscribe by sending a text frame
 * containing {"COMMAND":"LISTEN","DATA":"/path"} (or "IGNORE" to stop),
 * and matching values are streamed back as binary frames whose payload
 * is a raw OSC packet.
 *
 * Only what the protocol needs is implemented: the upgrade handshake,
 * masked client frames with 7/16/64-bit lengths, continuation
 * assembly, ping/pong and close handling. Client frames larger than
 * osc_ws::max_message are rejected.
 */

#include "osc-net.h"

#include <cstddef>
#include <cstdint>
#include <string>

namespace osc_ws {

constexpr size_t max_message = 8192;

/* Takes over an accepted socket whose HTTP request upgraded to a
 * websocket connection; key is the Sec-WebSocket-Key header value.
 * Sends the 101 handshake, then serves the connection until the peer
 * disconnects or osc_ws_shutdown() closes it. Drops all of the
 * connection's LISTEN registrations on exit and closes fd; the calling
 * thread must not touch fd afterwards. */
void serve(osc_net::socket_t fd, const std::string &key);

/* Streams one raw OSC packet as a binary frame to the connection
 * identified by token. Safe from any thread; silently ignored when the
 * token no longer identifies a live connection. */
void send_binary(void *token, const uint8_t *data, size_t size);

/* Closes every active connection so their serving threads wake up and
 * exit. Called when the OSCQuery server stops or restarts. */
void shutdown_all(void);

} // namespace osc_ws
