#pragma once

/*
 * Minimal HTTP/1.x responder used by the OSCQuery browser interface.
 *
 * GET requests only: the query string is stripped and the path is
 * percent-decoded before the handler sees it. An empty handler result
 * is answered with 404.
 */

#include <functional>
#include <string>

using osc_http_handler = std::function<std::string(const std::string &decoded_path)>;

/* Starts serving on all interfaces at the given port. Returns false
 * when the TCP port cannot be bound. */
bool osc_http_start(int port, osc_http_handler handler);

/* Stops serving; blocks until the accept thread has exited. */
void osc_http_stop(void);
