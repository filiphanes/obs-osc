#include "osc-query-http.h"

#include "osc-net.h"
#include "osc-ws.h"

#include <util/base.h>

#include <atomic>
#include <cctype>
#include <cstring>
#include <thread>

static const char method_error_json[] = "{\"STATUS\":\"error\",\"DESCRIPTION\":\"method not supported\"}";
static const char not_found_json[] = "{\"STATUS\":\"error\",\"DESCRIPTION\":\"not found\"}";

static std::atomic<bool> http_running{false};
static osc_http_handler http_handler;
static osc_net::socket_t listen_fd = osc_net::invalid_socket;
static std::thread accept_thread;

static std::string url_decode(const std::string &in)
{
	std::string out;

	for (size_t i = 0; i < in.size(); i++) {
		if (in[i] == '%' && i + 2 < in.size() && isxdigit((unsigned char)in[i + 1]) &&
		    isxdigit((unsigned char)in[i + 2])) {
			auto hex = [](char c) -> char {
				if (c >= '0' && c <= '9')
					return (char)(c - '0');
				return (char)((c | 0x20) - 'a' + 10);
			};
			out += (char)((hex(in[i + 1]) << 4) | hex(in[i + 2]));
			i += 2;
		} else {
			out += in[i];
		}
	}
	return out;
}

static void send_all(osc_net::socket_t fd, const char *data, size_t size)
{
	size_t sent = 0;

	while (sent < size) {
		const ssize_t n = send(fd, data + sent, (int)(size - sent), 0);
		if (n <= 0)
			return;
		sent += (size_t)n;
	}
}

static void respond(osc_net::socket_t fd, const std::string &body, bool ok)
{
	std::string header = ok ? "HTTP/1.1 200 OK\r\n" : "HTTP/1.1 404 Not Found\r\n";
	header += "Content-Type: application/json\r\n";
	header += "Access-Control-Allow-Origin: *\r\n";
	header += "Connection: close\r\n";
	header += "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n";

	send_all(fd, header.c_str(), header.size());
	send_all(fd, body.c_str(), body.size());
}

#ifdef _WIN32
#define osc_strncasecmp _strnicmp
#else
#define osc_strncasecmp strncasecmp
#endif

/* Case-insensitive search of the header block for a line prefix.
 * Returns the value start and sets *end to the value's last byte + 1,
 * or returns null when the header is absent. */
static const char *find_header_value(const std::string &request, const char *name,
				     const char **end)
{
	size_t pos = 0;
	const size_t name_len = strlen(name);

	while ((pos = request.find('\n', pos)) != std::string::npos) {
		pos++;
		if (pos + name_len > request.size())
			break;
		if (osc_strncasecmp(request.c_str() + pos, name, name_len) != 0)
			continue;

		pos += name_len;
		while (pos < request.size() && (request[pos] == ' ' || request[pos] == ':'))
			pos++;
		const size_t value_end = request.find('\r', pos);
		if (value_end == std::string::npos)
			return nullptr;
		*end = request.c_str() + value_end;
		return request.c_str() + pos;
	}
	return nullptr;
#undef osc_strncasecmp
}

/* Detects an RFC 6455 upgrade request and hands the socket over to
 * the websocket server, which owns it from there. Returns true when
 * the request was an upgrade (fd consumed). */
static bool try_upgrade(osc_net::socket_t fd, const std::string &request)
{
	const char *key_end = nullptr;
	const char *upgrade_end = nullptr;
	const char *key = find_header_value(request, "sec-websocket-key", &key_end);

	if (!key || !find_header_value(request, "upgrade", &upgrade_end))
		return false;

	osc_ws::serve(fd, std::string(key, (size_t)(key_end - key)));
	return true;
}

static void handle_client(osc_net::socket_t fd)
{
	std::string request;
	char buf[2048];

	while (request.find("\r\n\r\n") == std::string::npos && request.size() < 8192) {
		const ssize_t n = recv(fd, buf, sizeof(buf), 0);
		if (n <= 0)
			break;
		request.append(buf, (size_t)n);
	}

	/* A websocket upgrade consumes the connection - osc_ws::serve owns
	 * and closes the fd. Everything else is a one-shot GET answered
	 * and closed below. */
	if (try_upgrade(fd, request))
		return;

	if (request.compare(0, 4, "GET ") != 0) {
		respond(fd, method_error_json, false);
	} else {
		const size_t end = request.find(' ', 4);
		const size_t length =
			end == std::string::npos ? request.size() - 4 : end - 4;
		std::string path = request.substr(4, length);

		const size_t query = path.find('?');
		if (query != std::string::npos)
			path.erase(query);

		const std::string decoded = path == "/" ? "/" : url_decode(path);
		const std::string body = http_handler(decoded);
		if (body.empty())
			respond(fd, not_found_json, false);
		else
			respond(fd, body, true);
	}

	osc_net::close_socket(fd);
}

static void accept_loop(osc_net::socket_t fd)
{
	while (http_running.load()) {
		sockaddr_storage ss;
		socklen_t slen = sizeof(ss);

		const osc_net::socket_t client = accept(fd, (sockaddr *)&ss, &slen);
		if (client == osc_net::invalid_socket || !http_running.load())
			break;

		osc_net::set_recv_timeout(client, 2000);

		std::thread(handle_client, client).detach();
	}
}

bool osc_http_start(int port, osc_http_handler handler)
{
	if (http_running.load())
		return true;

	const osc_net::socket_t fd = osc_net::listen_tcp(port, 8);
	if (fd == osc_net::invalid_socket)
		return false;

	http_handler = std::move(handler);
	http_running.store(true);
	listen_fd = fd;
	accept_thread = std::thread(accept_loop, fd);

	return true;
}

void osc_http_stop(void)
{
	if (!http_running.exchange(false))
		return;

	/* Wake and drop any live websocket peers before the listener
	 * goes away, so their serving threads exit promptly. */
	osc_ws::shutdown_all();

	const osc_net::socket_t fd = listen_fd;
	listen_fd = osc_net::invalid_socket;

	/* Closing the socket wakes up the blocked accept(). */
	osc_net::close_wake(fd);

	if (accept_thread.joinable())
		accept_thread.join();
}
