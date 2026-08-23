#pragma once

/*
 * UDP transport for OSC datagrams.
 *
 * Inbound packets are received on a network thread and handed to the
 * handler as raw datagrams together with the sender's endpoint; the
 * handler owns protocol parsing and any further queuing. Outbound
 * traffic splits into three paths: send_fixed() targets the configured
 * feedback host, send_last() the most recent inbound sender (legacy
 * fallback), and send_to() any explicit endpoint - used for poll
 * replies and per-topic subscription delivery.
 *
 * Platform socket state is kept behind an opaque impl so that this
 * header pulls in no system headers.
 */

#include <atomic>
#include <cstdint>
#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <thread>

namespace osc_net {
struct osc_endpoint; /* platform socket address, defined in osc-net.h */
}

class osc_server {
public:
	/* The sender endpoint identifies the datagram's origin so handlers
	 * can answer polls on the exact socket they came from. */
	using handler_t = std::function<void(const uint8_t *data, size_t size,
					     const osc_net::osc_endpoint &sender)>;

	osc_server();
	~osc_server();

	osc_server(const osc_server &) = delete;
	osc_server &operator=(const osc_server &) = delete;

	bool start(int port, handler_t handler);
	void stop();

	void set_feedback_target(const std::string &host, int port);

	/* Sends to the fixed feedback target, when one is configured. */
	void send_fixed(const uint8_t *data, size_t size);

	/* Sends to the sender of the most recent inbound packet; the
	 * legacy single-controller feedback path. */
	void send_last(const uint8_t *data, size_t size);

	/* Sends one datagram to an explicit endpoint, e.g. answering a
	 * poll request or delivering a subscription match. */
	void send_to(const uint8_t *data, size_t size, const osc_net::osc_endpoint &target);

private:
	void recv_loop();

	struct impl;
	std::unique_ptr<impl> impl_;

	std::thread thread_;
	std::atomic<bool> running_{false};
	handler_t handler_;
};
