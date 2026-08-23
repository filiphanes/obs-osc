#pragma once

/*
 * Tiny platform socket abstraction shared by the OSC transport (UDP)
 * and the OSCQuery server (TCP).
 *
 * This is the only module that touches platform socket APIs directly;
 * everything else goes through these helpers so that startup/cleanup
 * pairing, close semantics and Windows/POSIX differences live in one
 * place.
 */

#include <cstddef>
#include <cstdint>
#include <string>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

/* Where an inbound datagram came from, so a reply can be sent back
 * to the exact sender. Defined in this header because it owns platform
 * socket types; everything else passes it around by reference and
 * forward-declares the type to keep system headers out. */
namespace osc_net {

struct osc_endpoint {
	sockaddr_storage addr = {};
	socklen_t len = 0;
};

#ifdef _WIN32
using socket_t = SOCKET;
constexpr socket_t invalid_socket = INVALID_SOCKET;
#else
using socket_t = int;
constexpr socket_t invalid_socket = -1;
#endif

/* Winsock startup bookkeeping; no-op off Windows. Each start()/stop()
 * pair must call init()/cleanup() symmetrically. */
bool init(void);
void cleanup(void);

/* Creates a socket, binds it to INADDR_ANY:port (with SO_REUSEADDR)
 * and - for TCP - puts it into listening state. Logs and returns
 * invalid_socket on failure. */
socket_t listen_udp(int port);
socket_t listen_tcp(int port, int backlog);

/* Sets the receive timeout used for accepted client sockets. */
void set_recv_timeout(socket_t fd, int milliseconds);

/* Closes an ordinary connected socket. */
void close_socket(socket_t fd);

/* Closes a listener socket, first waking any thread blocked in
 * recvfrom()/accept() on it. */
void close_wake(socket_t fd);

/* Resolves an IPv4 host/port pair for sendto(); logs on failure. */
bool resolve(const std::string &host, int port, sockaddr_storage *ss, socklen_t *ss_len);

} // namespace osc_net
