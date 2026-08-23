#include "osc-plugin.h"
#include "osc-net.h" /* osc_endpoint definition for the dispatch closure */
#include "osc-query.h"
#include "osc-server.h"
#include "osc-ui-task.h"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <util/platform.h>

#include <utility>
#include <vector>

osc_config g_config;
osc_server g_server;

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-osc", "en-US")

/* Runs on the network thread: parse the datagram, then hand ALL of its
 * messages to the UI thread as one task. Bundles stay ordered together
 * and bursty controllers cost one queue hop instead of one per message;
 * the closure moves the messages in, so only one heap task remains.
 * The sender's endpoint rides along so polls can be answered on the
 * socket they arrived from. */
static void handle_datagram(const uint8_t *data, size_t size, const osc_net::osc_endpoint &sender)
{
	std::vector<osc_message> msgs;
	if (!osc_parse_packet(data, size, &msgs))
		return;

	osc_ui_post(
		[msgs = std::move(msgs), sender] {
			for (const osc_message &msg : msgs)
				osc_dispatch_command(msg, sender);
		});
}

static void load_config(void)
{
	char *path = obs_module_config_path("plugin-config.json");
	if (!path)
		return;

	obs_data_t *data = obs_data_create_from_json_file_safe(path, "bak");
	if (data) {
		g_config.port = (int)obs_data_get_int(data, "port");
		g_config.feedback_enabled = obs_data_get_bool(data, "feedback_enabled");
		g_config.feedback_host = obs_data_get_string(data, "feedback_host");
		g_config.feedback_port = (int)obs_data_get_int(data, "feedback_port");
		g_config.query_enabled = obs_data_get_bool(data, "query_enabled");
		g_config.query_port = (int)obs_data_get_int(data, "query_port");
		obs_data_release(data);
	}

	if (g_config.port <= 0 || g_config.port > 65535)
		g_config.port = 9000;
	if (g_config.feedback_port < 0 || g_config.feedback_port > 65535)
		g_config.feedback_port = 0;
	if (g_config.query_port < 0 || g_config.query_port > 65535)
		g_config.query_port = 0;

	bfree(path);
}

void osc_save_config(void)
{
	char *path = obs_module_config_path("plugin-config.json");
	if (!path)
		return;

	std::string directory = path;
	const size_t slash = directory.find_last_of("/\\");
	if (slash != std::string::npos) {
		directory.resize(slash);
		os_mkdirs(directory.c_str());
	}

	obs_data_t *data = obs_data_create();
	obs_data_set_int(data, "port", g_config.port);
	obs_data_set_bool(data, "feedback_enabled", g_config.feedback_enabled);
	obs_data_set_string(data, "feedback_host", g_config.feedback_host.c_str());
	obs_data_set_int(data, "feedback_port", g_config.feedback_port);
	obs_data_set_bool(data, "query_enabled", g_config.query_enabled);
	obs_data_set_int(data, "query_port", g_config.query_port);
	obs_data_save_json_safe(data, path, "tmp", "bak");
	obs_data_release(data);

	bfree(path);
}

/* Restarts the UDP listener on another port. Returns false when the new
 * port cannot be bound; the previous port stays active in that case. */
static bool osc_apply_port(int port)
{
	const int previous = g_config.port;
	if (port == previous)
		return true;

	g_server.stop();
	if (g_server.start(port, handle_datagram)) {
		g_config.port = port;
		blog(LOG_INFO, "[osc-osc] listening port changed to %d", port);

		/* The default OSCQuery port is derived from the OSC port. */
		if (g_config.query_enabled && g_config.query_port == 0)
			osc_query_restart();
		return true;
	}

	blog(LOG_WARNING, "[obs-osc] cannot bind UDP port %d, keeping port %d", port, previous);
	g_server.start(previous, handle_datagram);
	return false;
}

osc_apply_result osc_apply_settings(const osc_config &next)
{
	if (next.port != g_config.port && !osc_apply_port(next.port)) {
		/* osc_apply_port() restored the previous listener already. */
		return osc_apply_result::udp_port_failed;
	}

	if (next.query_port != g_config.query_port ||
	    next.query_enabled != g_config.query_enabled) {
		const int old_query_port = g_config.query_port;
		const bool old_query_enabled = g_config.query_enabled;

		g_config.query_port = next.query_port;
		g_config.query_enabled = next.query_enabled;

		if (!osc_query_restart()) {
			g_config.query_port = old_query_port;
			g_config.query_enabled = old_query_enabled;
			osc_query_restart();
			return osc_apply_result::query_port_failed;
		}
	}

	g_config.feedback_enabled = next.feedback_enabled;
	g_config.feedback_host = next.feedback_host;
	g_config.feedback_port = next.feedback_port;
	g_server.set_feedback_target(g_config.feedback_host, g_config.feedback_port);
	osc_save_config();

	return osc_apply_result::ok;
}

static void settings_menu_cb(void *param)
{
	UNUSED_PARAMETER(param);
	osc_show_settings_dialog();
}

bool obs_module_load(void)
{
	load_config();

	/* Hooks are always installed; osc_feedback_send() gates on this flag. */
	osc_feedback_init();

	if (!g_server.start(g_config.port, handle_datagram))
		blog(LOG_ERROR, "[obs-osc] OSC server not started, remote control unavailable");

	/* After start(): socket setup must exist before the feedback host
	 * can resolve (Winsock on Windows). The target also outlives later
	 * listener restarts, so ordering here is the only requirement. */
	g_server.set_feedback_target(g_config.feedback_host, g_config.feedback_port);

	const char *menu_name = "OSC Settings...";
	obs_module_get_string("Settings", &menu_name);
	obs_frontend_add_tools_menu_item(menu_name, settings_menu_cb, nullptr);

	osc_query_restart();

	return true;
}

void obs_module_unload(void)
{
	osc_query_stop();
	g_server.stop();
	osc_feedback_shutdown();
	osc_save_config();
}

/* ------------------------------------------------------------------ */
/* Scoped poll-reply batching                                          */
/* ------------------------------------------------------------------ */

/* Defined here because g_server lives here; osc-server.h only declares
 * the class so it stays free of module state. */
osc_reply_batch::osc_reply_batch(const osc_net::osc_endpoint &to)
{
	g_server.begin_batch(to);
	active_ = true;
}

osc_reply_batch::~osc_reply_batch(void)
{
	if (active_)
		g_server.end_batch();
}
