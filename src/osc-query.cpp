#include "osc-query.h"

#include "osc-net.h"
#include "osc-plugin.h"
#include "osc-query-bonjour.h"
#include "osc-query-http.h"
#include "osc-query-schema.h"

#include <obs-module.h>

#include <atomic>
#include <string>

static std::atomic<bool> query_running{false};
static int current_udp_port = 0;

static std::string host_info_json(void)
{
	/* Hand-built: obs_data arrays only hold objects, not plain strings.
	 * LISTEN implies IGNORE per the proposal. */
	return "{\"NAME\":\"OBS Studio\",\"PORT\":" + std::to_string(current_udp_port) +
	       ",\"EXTENSIONS\":[\"SERVER\",\"LISTEN\"]}";
}

/* Runs on an HTTP connection thread; the path is decoded already. */
static std::string handle_request(const std::string &path)
{
	if (path == "/HOST_INFO")
		return host_info_json();
	return osc_query_schema_lookup(path);
}

bool osc_query_start(int http_port, int osc_udp_port)
{
	if (query_running.load())
		return true;

	current_udp_port = osc_udp_port;

	if (!osc_net::init())
		return false;

	if (!osc_http_start(http_port, handle_request)) {
		osc_net::cleanup();
		return false;
	}

	query_running.store(true);
	osc_bonjour_register(http_port);

	blog(LOG_INFO, "[obs-osc] OSCQuery server on http://*:%d (OSC on UDP %d)", http_port,
	     osc_udp_port);

	return true;
}

void osc_query_stop(void)
{
	if (!query_running.exchange(false))
		return;

	osc_http_stop();

	osc_bonjour_unregister();
	osc_net::cleanup();
}

bool osc_query_restart(void)
{
	osc_query_stop();

	if (!g_config.query_enabled)
		return true;

	const int http_port = g_config.query_port > 0 ? g_config.query_port : g_config.port + 1;
	return osc_query_start(http_port, g_config.port);
}
