#include "osc-net.h"

#include <util/base.h>

#include <cstring>

namespace osc_net {

bool init(void)
{
#ifdef _WIN32
	WSADATA wsa;
	if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
		blog(LOG_ERROR, "[obs-osc] WSAStartup failed");
		return false;
	}
#endif
	return true;
}

void cleanup(void)
{
#ifdef _WIN32
	WSACleanup();
#endif
}

static socket_t bind_listen(int port, int type, int log_level, const char *name, int backlog)
{
	const socket_t fd = socket(AF_INET, type, type == SOCK_DGRAM ? IPPROTO_UDP : IPPROTO_TCP);
	if (fd == invalid_socket) {
		blog(log_level, "[obs-osc] failed to create %s socket", name);
		return invalid_socket;
	}

	int reuse = 1;
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char *)&reuse, sizeof(reuse));

	sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_ANY);
	addr.sin_port = htons((uint16_t)port);

	if (bind(fd, (const sockaddr *)&addr, sizeof(addr)) != 0 ||
	    (backlog >= 0 && listen(fd, backlog) != 0)) {
		blog(log_level, "[obs-osc] failed to bind %s port %d", name, port);
		close_socket(fd);
		return invalid_socket;
	}

	return fd;
}

socket_t listen_udp(int port)
{
	return bind_listen(port, SOCK_DGRAM, LOG_ERROR, "UDP", -1);
}

socket_t listen_tcp(int port, int backlog)
{
	return bind_listen(port, SOCK_STREAM, LOG_WARNING, "TCP", backlog);
}

void set_recv_timeout(socket_t fd, int milliseconds)
{
#ifdef _WIN32
	DWORD tv = (DWORD)milliseconds;
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof(tv));
#else
	timeval tv = {milliseconds / 1000, (suseconds_t)(milliseconds % 1000) * 1000};
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
}

void close_socket(socket_t fd)
{
	if (fd == invalid_socket)
		return;

#ifdef _WIN32
	closesocket(fd);
#else
	close(fd);
#endif
}

void close_wake(socket_t fd)
{
#ifndef _WIN32
	/* Windows wakes blocked calls from closesocket(); POSIX needs an
	 * explicit shutdown first. */
	shutdown(fd, SHUT_RDWR);
#endif
	close_socket(fd);
}

bool resolve(const std::string &host, int port, sockaddr_storage *ss, socklen_t *ss_len)
{
	addrinfo hints;
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_DGRAM;

	addrinfo *res = nullptr;
	if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0 || !res) {
		blog(LOG_WARNING, "[obs-osc] cannot resolve feedback host '%s:%d'", host.c_str(), port);
		return false;
	}

	memcpy(ss, res->ai_addr, res->ai_addrlen);
	*ss_len = (socklen_t)res->ai_addrlen;
	freeaddrinfo(res);
	return true;
}

} // namespace osc_net
