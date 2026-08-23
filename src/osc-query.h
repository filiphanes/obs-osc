#pragma once

/*
 * OSCQuery support (https://github.com/Vidvox/oscquery-proposal):
 *
 * - Embedded HTTP server that exposes the plugin's OSC address tree as
 *   JSON documents (HOST_INFO plus one document per node), so controllers
 *   such as TouchOSC can auto-configure their layouts.
 * - Bonjour/mDNS advertisement of the "_oscjson._tcp" service on Apple
 *   platforms so hosts are discovered automatically.
 */

#include <string>

/* Starts the OSCQuery HTTP server on http_port and advertises it. The
 * osc_udp_port is reported through HOST_INFO. */
bool osc_query_start(int http_port, int osc_udp_port);

/* Stops the HTTP server and removes the Bonjour advertisement. */
void osc_query_stop(void);

/* Restarts (or stops/starts) the server according to g_config. Returns
 * false when the HTTP port could not be bound. */
bool osc_query_restart(void);
