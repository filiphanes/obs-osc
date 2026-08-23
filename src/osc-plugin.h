#pragma once

/*
 * obs-osc: Open Sound Control remote control plugin for OBS Studio.
 *
 * Shared configuration and cross-module declarations. See README.md
 * for the complete address namespace; osc-addresses.h for its single
 * in-code definition.
 *
 * This header intentionally does not include osc-server.h: modules
 * that touch g_server's members include the transport header
 * themselves so that platform socket headers stay out of everything
 * else (e.g. the Qt dialog).
 */

#include "osc-message.h"

#include <initializer_list>
#include <string>

namespace osc_net {
struct osc_endpoint; /* platform socket address, defined in osc-net.h */
}

class osc_server;

struct osc_config {
	int port = 9000; /* inbound UDP port */
	bool feedback_enabled = true;
	std::string feedback_host; /* empty = reply to last controller */
	int feedback_port = 0; /* 0 = auto */
	bool query_enabled = true; /* OSCQuery HTTP + Bonjour discovery */
	int query_port = 0; /* 0 = osc port + 1 */
};

extern osc_config g_config;
extern osc_server g_server;

/* Executes an inbound OSC command. Must be called on the UI thread.
 * Zero-argument messages are reads: poll_dispatch answers with the
 * current value(s) sent to reply_to (the datagram's sender). */
void osc_dispatch_command(const osc_message &msg, const osc_net::osc_endpoint &reply_to);

/* Emits one feedback message to the configured controller (if enabled). */
void osc_feedback_send(const char *address, std::initializer_list<osc_argument> args = {});

/* Frontend event + source signal hooks that produce feedback messages. */
void osc_feedback_init(void);
void osc_feedback_shutdown(void);

/* Opens the modal settings dialog (Tools menu). Must run on the UI thread. */
void osc_show_settings_dialog(void);

/* Persists g_config to plugin-config.json. */
void osc_save_config(void);

/* Result of applying settings from the dialog. */
enum class osc_apply_result {
	ok,
	udp_port_failed, /* the OSC port could not be bound */
	query_port_failed, /* the OSCQuery HTTP port could not be bound */
};

/* Transactionally applies new configuration: restarts the UDP listener
 * and/or the OSCQuery server as needed, updates the feedback target and
 * saves. On failure nothing after the failing step is applied, the
 * already-working parts of g_config stay active, and a status is
 * returned so the caller can report which field failed. */
osc_apply_result osc_apply_settings(const osc_config &next);

/* Restarts (or disables) the OSCQuery server per g_config. Returns false
 * when the HTTP port could not be bound. Implemented in osc-query.cpp. */
bool osc_query_restart(void);
