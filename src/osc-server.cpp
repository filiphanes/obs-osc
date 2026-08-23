#include "osc-server.h"

#include "osc-message.h"
#include "osc-net.h"

#include <util/base.h>

#include <cstring>
#include <mutex>
#include <vector>

struct osc_server::impl {
	std::mutex target_mutex;
	osc_net::socket_t fd = osc_net::invalid_socket;

	sockaddr_storage resolved_target = {};
	socklen_t resolved_len = 0;
	sockaddr_storage last_sender = {};
	socklen_t last_sender_len = 0;
	bool have_sender = false;
};

osc_server::osc_server() = default;

osc_server::~osc_server()
{
	stop();
}

bool osc_server::start(int port, handler_t handler)
{
	if (running_.load())
		return true;

	if (!osc_net::init())
		return false;

	const osc_net::socket_t fd = osc_net::listen_udp(port);
	if (fd == osc_net::invalid_socket) {
		osc_net::cleanup();
		return false;
	}

	impl_.reset(new impl);
	impl_->fd = fd;
	handler_ = std::move(handler);
	running_.store(true);
	thread_ = std::thread(&osc_server::recv_loop, this);

	blog(LOG_INFO, "[obs-osc] listening on UDP port %d", port);
	return true;
}

void osc_server::stop()
{
	if (!running_.exchange(false))
		return;

	const osc_net::socket_t fd = impl_->fd;
	impl_->fd = osc_net::invalid_socket;

	/* Closing the socket wakes up the blocked recvfrom(). */
	osc_net::close_wake(fd);

	if (thread_.joinable())
		thread_.join();

	handler_ = nullptr;
	impl_.reset();
	osc_net::cleanup();
}

void osc_server::set_feedback_target(const std::string &host, int port)
{
	std::lock_guard<std::mutex> lock(impl_->target_mutex);

	impl_->resolved_len = 0;

	if (host.empty() || port <= 0 || port > 65535)
		return;

	sockaddr_storage ss = {};
	socklen_t ss_len = 0;
	if (!osc_net::resolve(host, port, &ss, &ss_len))
		return;

	impl_->resolved_target = ss;
	impl_->resolved_len = ss_len;
}

void osc_server::send_fixed(const uint8_t *data, size_t size)
{
	sockaddr_storage target;
	socklen_t target_len = 0;
	osc_net::socket_t fd;

	{
		std::lock_guard<std::mutex> lock(impl_->target_mutex);
		if (impl_->resolved_len) {
			target = impl_->resolved_target;
			target_len = impl_->resolved_len;
		}
		fd = impl_->fd;
	}

	if (!target_len || !size || fd == osc_net::invalid_socket)
		return;

	sendto(fd, (const char *)data, (int)size, 0, (const sockaddr *)&target, target_len);
}

void osc_server::send_last(const uint8_t *data, size_t size)
{
	sockaddr_storage target;
	socklen_t target_len = 0;
	osc_net::socket_t fd;

	{
		std::lock_guard<std::mutex> lock(impl_->target_mutex);
		if (impl_->have_sender) {
			target = impl_->last_sender;
			target_len = impl_->last_sender_len;
		}
		fd = impl_->fd;
	}

	if (!target_len || !size || fd == osc_net::invalid_socket)
		return;

	sendto(fd, (const char *)data, (int)size, 0, (const sockaddr *)&target, target_len);
}

void osc_server::send_to(const uint8_t *data, size_t size, const osc_net::osc_endpoint &target)
{
	osc_net::socket_t fd;

	{
		std::lock_guard<std::mutex> lock(impl_->target_mutex);
		fd = impl_->fd;
	}

	if (!target.len || !size || fd == osc_net::invalid_socket)
		return;

	sendto(fd, (const char *)data, (int)size, 0, (const sockaddr *)&target.addr, target.len);
}

void osc_server::recv_loop()
{
	std::vector<uint8_t> buf(65536);

	while (running_.load()) {
		sockaddr_storage ss;
		socklen_t slen = sizeof(ss);

		const int n = (int)recvfrom(impl_->fd, (char *)buf.data(), (int)buf.size(), 0,
					    (sockaddr *)&ss, &slen);
		if (n <= 0) {
			if (!running_.load())
				break;
			continue;
		}

		{
			std::lock_guard<std::mutex> lock(impl_->target_mutex);
			impl_->last_sender = ss;
			impl_->last_sender_len = slen;
			impl_->have_sender = true;
		}

		if (handler_) {
			osc_net::osc_endpoint sender{ss, slen};
			handler_(buf.data(), (size_t)n, sender);
		}
	}
}

void osc_server::reply(const osc_net::osc_endpoint &to, const char *address,
		       std::initializer_list<osc_argument> args)
{
	std::vector<uint8_t> buf;
	osc_build_message(&buf, address, args);
	send_to(buf.data(), buf.size(), to);
}
