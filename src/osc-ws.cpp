#include "osc-ws.h"

#include "osc-subscribe.h"

#include <QCryptographicHash>

#include <obs-module.h>

#include <atomic>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace osc_ws {

namespace {

constexpr char handshake_guid[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

constexpr uint8_t op_continuation = 0x0;
constexpr uint8_t op_text = 0x1;
constexpr uint8_t op_binary = 0x2;
constexpr uint8_t op_close = 0x8;
constexpr uint8_t op_ping = 0x9;
constexpr uint8_t op_pong = 0xA;

/* One websocket connection. The serving thread owns the receive side;
 * publisher threads share the send side through write_mutex. Tokens
 * handed to the listener registry stay valid because every holder goes
 * through the registry map below, which owns one shared_ptr. */
struct conn {
	osc_net::socket_t fd;
	std::mutex write_mutex;
	std::atomic<bool> open{false};

	explicit conn(osc_net::socket_t socket) : fd(socket) {}
};

std::mutex conns_mutex;
std::map<void *, std::shared_ptr<conn>> &conns()
{
	static std::map<void *, std::shared_ptr<conn>> c;
	return c;
}

/* ---- socket helpers ------------------------------------------------ */

bool send_all(osc_net::socket_t fd, const char *data, size_t size)
{
	size_t sent = 0;

	while (sent < size) {
		const ssize_t n = send(fd, data + sent, (int)(size - sent), 0);
		if (n <= 0)
			return false;
		sent += (size_t)n;
	}
	return true;
}

/* Reads exactly n bytes; false on EOF, error or shutdown wake-up. */
bool recv_exact(osc_net::socket_t fd, void *buf, size_t n)
{
	auto *out = (char *)buf;
	size_t got = 0;

	while (got < n) {
		const ssize_t r = recv(fd, out + got, (int)(n - got), 0);
		if (r <= 0)
			return false;
		got += (size_t)r;
	}
	return true;
}

/* Closes the connection's socket exactly once, even when the serving
 * thread and shutdown_all() race: both paths run under write_mutex and
 * reset fd afterwards. */
void close_conn(const std::shared_ptr<conn> &c)
{
	std::lock_guard<std::mutex> lock(c->write_mutex);

	if (!c->open.exchange(false))
		return;

	osc_net::close_wake(c->fd);
	c->fd = osc_net::invalid_socket;
}

/* ---- frame layer ----------------------------------------------------- */

/* Builds one unmasked server frame; payload may be null when empty. */
std::vector<uint8_t> build_frame(uint8_t opcode, const uint8_t *payload, size_t size)
{
	std::vector<uint8_t> frame;
	frame.reserve(size + 10);

	frame.push_back((uint8_t)(0x80 | opcode));
	if (size < 126) {
		frame.push_back((uint8_t)size);
	} else if (size <= 0xFFFF) {
		frame.push_back(126);
		frame.push_back((uint8_t)(size >> 8));
		frame.push_back((uint8_t)size);
	} else {
		frame.push_back(127);
		for (int shift = 56; shift >= 0; shift -= 8)
			frame.push_back((uint8_t)((uint64_t)size >> shift));
	}
	if (payload && size)
		frame.insert(frame.end(), payload, payload + size);
	return frame;
}

bool send_frame(const std::shared_ptr<conn> &c, uint8_t opcode, const uint8_t *payload,
		size_t size)
{
	std::lock_guard<std::mutex> lock(c->write_mutex);

	if (!c->open.load())
		return false;

	const std::vector<uint8_t> frame = build_frame(opcode, payload, size);
	if (!send_all(c->fd, (const char *)frame.data(), frame.size())) {
		c->open.store(false);
		return false;
	}
	return true;
}

/* Header without the payload: b0, b1 and the optional extended
 * length. Masking key is handled by read_masked_payload. */
struct frame_header {
	uint8_t opcode = 0;
	bool fin = false;
	bool masked = false;
	uint64_t length = 0;
};

bool read_header(osc_net::socket_t fd, frame_header *hdr)
{
	uint8_t head[2] = {0, 0};
	if (!recv_exact(fd, head, 2))
		return false;

	hdr->fin = (head[0] & 0x80) != 0;
	hdr->opcode = head[0] & 0x0F;
	hdr->masked = (head[1] & 0x80) != 0;
	hdr->length = head[1] & 0x7F;

	if (hdr->length == 126) {
		uint8_t ext[2] = {0, 0};
		if (!recv_exact(fd, ext, 2))
			return false;
		hdr->length = ((uint64_t)ext[0] << 8) | ext[1];
	} else if (hdr->length == 127) {
		uint8_t ext[8] = {0};
		if (!recv_exact(fd, ext, 8))
			return false;
		hdr->length = 0;
		for (uint8_t byte : ext)
			hdr->length = (hdr->length << 8) | byte;
	}

	if (hdr->length > max_message)
		return false;
	return true;
}

bool read_masked_payload(osc_net::socket_t fd, const frame_header &hdr, std::vector<uint8_t> *out)
{
	uint8_t mask[4] = {0, 0, 0, 0};
	if (hdr.masked && !recv_exact(fd, mask, 4))
		return false;

	out->resize((size_t)hdr.length);
	if (hdr.length && !recv_exact(fd, out->data(), out->size()))
		return false;

	if (hdr.masked && hdr.length) {
		for (size_t i = 0; i < out->size(); i++)
			(*out)[i] ^= mask[i % 4];
	}
	return true;
}

/* ---- protocol handling ---------------------------------------------- */

std::string compute_accept_key(const std::string &key)
{
	QCryptographicHash hash(QCryptographicHash::Sha1);
	hash.addData(QByteArrayView(key.data(), (qsizetype)key.size()));
	hash.addData(
		QByteArrayView(handshake_guid, (qsizetype)(sizeof(handshake_guid) - 1)));
	return hash.result().toBase64().toStdString();
}

void handle_text(conn *c, const std::vector<uint8_t> &message)
{
	std::string text((const char *)message.data(), message.size());
	obs_data_t *data = obs_data_create_from_json(text.c_str());
	if (!data)
		return;

	const std::string command = obs_data_get_string(data, "COMMAND");
	const std::string path = obs_data_get_string(data, "DATA");

	if (!path.empty() && path[0] == '/') {
		if (command == "LISTEN")
			osc_listen_add(c, path);
		else if (command == "IGNORE")
			osc_listen_remove(c, path);
		else
			blog(LOG_DEBUG, "[obs-osc] websocket: unknown COMMAND '%s'",
			     command.c_str());
	} else {
		blog(LOG_DEBUG, "[obs-osc] websocket: %s without a path", command.c_str());
	}
}

} // namespace

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

void serve(osc_net::socket_t fd, const std::string &key)
{
	/* Complete the upgrade before anything can interleave. */
	const std::string accept_key = compute_accept_key(key);
	std::string response = "HTTP/1.1 101 Switching Protocols\r\n"
			       "Upgrade: websocket\r\n"
			       "Connection: Upgrade\r\n"
			       "Sec-WebSocket-Accept: " +
			       accept_key + "\r\n\r\n";
	if (!send_all(fd, response.c_str(), response.size())) {
		osc_net::close_socket(fd);
		return;
	}

	auto c = std::make_shared<conn>(fd);
	c->open.store(true);

	{
		std::lock_guard<std::mutex> lock(conns_mutex);
		conns()[c.get()] = c;
	}

	/* Long-lived connection: no recv timeout. Shutdown wakes the
	 * blocking recv through close_wake on this fd. */
	osc_net::set_recv_timeout(fd, 0);

	frame_header header;
	std::vector<uint8_t> payload;
	std::vector<uint8_t> assembled;
	uint8_t assembling = 0;

	for (;;) {
		if (!read_header(fd, &header))
			break;

		payload.clear();
		if (!read_masked_payload(fd, header, &payload))
			break;

		switch (header.opcode) {
		case op_close:
			send_frame(c, op_close, payload.empty() ? nullptr : payload.data(),
				   payload.size());
			goto done;

		case op_ping:
			if (!send_frame(c, op_pong, payload.empty() ? nullptr : payload.data(),
					payload.size()))
				goto done;
			break;

		case op_pong:
		case op_binary:
			break; /* clients do not stream values to a server */

		case op_text:
		case op_continuation:
			if (header.opcode == op_text) {
				assembled = payload;
				assembling = op_text;
			} else if (assembling == op_text) {
				if (assembled.size() + payload.size() > max_message) {
					blog(LOG_WARNING, "[obs-osc] websocket message too long");
					goto done;
				}
				assembled.insert(assembled.end(), payload.begin(), payload.end());
			} else {
				blog(LOG_DEBUG, "[obs-osc] unexpected websocket continuation");
				goto done;
			}

			if (header.fin) {
				handle_text(c.get(), assembled);
				assembled.clear();
				assembling = 0;
			}
			break;

		default:
			blog(LOG_DEBUG, "[obs-osc] websocket: unsupported opcode %u", header.opcode);
			goto done;
		}
	}

done:
	osc_listen_remove_peer(c.get());

	{
		std::lock_guard<std::mutex> lock(conns_mutex);
		conns().erase(c.get());
	}

	close_conn(c);
}

void send_binary(void *token, const uint8_t *data, size_t size)
{
	std::shared_ptr<conn> c;

	{
		std::lock_guard<std::mutex> lock(conns_mutex);
		const auto it = conns().find(token);
		if (it == conns().end())
			return;
		c = it->second;
	}

	send_frame(c, op_binary, data, size);
}

void shutdown_all(void)
{
	std::vector<std::shared_ptr<conn>> dying;

	{
		std::lock_guard<std::mutex> lock(conns_mutex);
		dying.reserve(conns().size());
		for (auto &[token, c] : conns())
			dying.push_back(c);
		conns().clear();
	}

	for (const std::shared_ptr<conn> &c : dying) {
		osc_listen_remove_peer(c.get());
		close_conn(c);
	}
}

} // namespace osc_ws
