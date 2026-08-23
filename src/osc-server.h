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
 *
 * Every outbound datagram is queued to a dedicated sender thread, so
 * UDP sendto() stalls (e.g. first-packet ARP resolution) can never
 * block OBS signal, audio or UI threads. The queue is bounded; under
 * pressure the oldest datagrams are dropped, which is harmless for
 * state-style feedback where only the freshest value matters.
 */

#include "osc-message.h"
#include "osc-net.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstddef>
#include <deque>
#include <functional>
#include <initializer_list>
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

	/* Lock-free readiness probes for feedback senders: skip packet
	 * encoding entirely when no receiver could possibly exist. */
	bool has_fixed_target(void) const;
	bool has_last_sender(void) const;

	/* Sends to the fixed feedback target, when one is configured. */
	void send_fixed(const uint8_t *data, size_t size);

	/* Sends to the sender of the most recent inbound packet; the
	 * legacy single-controller feedback path. */
	void send_last(const uint8_t *data, size_t size);

	/* Sends one datagram to an explicit endpoint, e.g. answering a
	 * poll request or delivering a subscription match. */
	void send_to(const uint8_t *data, size_t size, const osc_net::osc_endpoint &target);

	/* Builds one OSC message and delivers it to an explicit endpoint;
	 * the canonical poll-reply path. Arguments mirror feedback. */
	void reply(const osc_net::osc_endpoint &to, const char *address,
		   std::initializer_list<osc_argument> args = {});

	/* Scoped reply batching: between begin and end, reply() calls to
	 * the same endpoint are collected and delivered as one #bundle
	 * datagram on end_batch(). State is thread-local, so UI thread
	 * and signal threads batch independently. No nesting. */
	void begin_batch(const osc_net::osc_endpoint &to);
	void end_batch(void);

private:
	void recv_loop();
	void send_loop();
	void enqueue(const sockaddr_storage &target, socklen_t target_len, const uint8_t *data,
		     size_t size);

	struct impl;
	std::unique_ptr<impl> impl_;

	std::thread thread_;
	std::atomic<bool> running_{false};
	handler_t handler_;
};

/* RAII helper around begin_batch()/end_batch() for the global server
 * instance; every poll reply made on this thread while it lives is
 * coalesced into a single bundle datagram. Implemented in plugin.cpp
 * next to g_server so this header stays free of module state. */
class osc_reply_batch {
public:
	explicit osc_reply_batch(const osc_net::osc_endpoint &to);
	~osc_reply_batch(void);

	osc_reply_batch(const osc_reply_batch &) = delete;
	osc_reply_batch &operator=(const osc_reply_batch &) = delete;

private:
	bool active_ = false;
};
