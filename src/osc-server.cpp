#include "osc-server.h"

#include "osc-message.h"
#include "osc-net.h"

#include <util/base.h>

#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <vector>

namespace {

/* Bounded outbound queue: enough to absorb feedback bursts (a dragged
 * fader plus tally storms) without ever growing without limit. */
constexpr size_t max_send_queue = 1024;

struct out_packet {
	sockaddr_storage target;
	socklen_t target_len;
	std::vector<uint8_t> data;
};

/* Per-thread poll-reply accumulator; see osc_server::begin_batch(). */
struct reply_batch {
	osc_net::osc_endpoint to = {};
	bool active = false;
	std::vector<std::vector<uint8_t>> messages;
};

reply_batch &thread_batch(void)
{
	static thread_local reply_batch b;
	return b;
}

} // namespace

struct osc_server::impl {
	std::mutex target_mutex;
	osc_net::socket_t fd = osc_net::invalid_socket;

	sockaddr_storage resolved_target = {};
	socklen_t resolved_len = 0;
	sockaddr_storage last_sender = {};
	socklen_t last_sender_len = 0;

	/* Lock-free mirrors of the two fields above for readiness probes;
	 * writers publish under target_mutex, readers sample relaxed. */
	std::atomic<bool> have_target{false};
	std::atomic<bool> have_sender{false};

	/* Outbound queue drained by send_loop(). */
	std::mutex send_mutex;
	std::condition_variable send_cv;
	std::deque<out_packet> send_queue;
	bool send_running = false;
	std::thread send_thread;
};

osc_server::osc_server() : impl_(new impl) {}

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

	if (!impl_)
		impl_.reset(new impl);
	impl_->fd = fd;
	handler_ = std::move(handler);
	running_.store(true);

	{
		std::lock_guard<std::mutex> lock(impl_->send_mutex);
		impl_->send_running = true;
	}
	impl_->send_thread = std::thread(&osc_server::send_loop, this);
	thread_ = std::thread(&osc_server::recv_loop, this);

	blog(LOG_INFO, "[obs-osc] listening on UDP port %d", port);
	return true;
}

void osc_server::stop()
{
	if (!running_.exchange(false))
		return;

	/* Stop the sender and let it drain whatever is already queued,
	 * while the socket is still open. */
	{
		std::lock_guard<std::mutex> lock(impl_->send_mutex);
		impl_->send_running = false;
	}
	impl_->send_cv.notify_all();
	if (impl_->send_thread.joinable())
		impl_->send_thread.join();

	osc_net::socket_t fd;
	{
		/* Mirrors the locked snapshot in recv_loop(): both sides of
		 * impl_->fd synchronize here. */
		std::lock_guard<std::mutex> lock(impl_->target_mutex);
		fd = impl_->fd;
		impl_->fd = osc_net::invalid_socket;
	}

	/* Closing the socket wakes up the blocked recvfrom(). */
	osc_net::close_wake(fd);

	if (thread_.joinable())
		thread_.join();

	handler_ = nullptr;

	{
		/* Run-scoped state dies with the listener; configuration
		 * (resolved feedback target) survives a port change so
		 * callers may configure before or between starts. */
		std::lock_guard<std::mutex> lock(impl_->target_mutex);
		impl_->last_sender = sockaddr_storage{};
		impl_->last_sender_len = 0;
		impl_->have_sender.store(false, std::memory_order_relaxed);
	}

	std::lock_guard<std::mutex> send_lock(impl_->send_mutex);
	impl_->send_queue.clear();

	/* Balances osc_net::init() from start(). */
	osc_net::cleanup();
}

void osc_server::set_feedback_target(const std::string &host, int port)
{
	std::lock_guard<std::mutex> lock(impl_->target_mutex);

	impl_->resolved_len = 0;
	impl_->have_target.store(false, std::memory_order_relaxed);

	if (host.empty() || port <= 0 || port > 65535)
		return;

	sockaddr_storage ss = {};
	socklen_t ss_len = 0;
	if (!osc_net::resolve(host, port, &ss, &ss_len))
		return;

	impl_->resolved_target = ss;
	impl_->resolved_len = ss_len;
	impl_->have_target.store(true, std::memory_order_relaxed);
}

bool osc_server::has_fixed_target(void) const
{
	return impl_->have_target.load(std::memory_order_relaxed);
}

bool osc_server::has_last_sender(void) const
{
	return impl_->have_sender.load(std::memory_order_relaxed);
}

/* Queues one datagram for the sender thread; copies the payload so
 * callers can release their buffers immediately. */
void osc_server::enqueue(const sockaddr_storage &target, socklen_t target_len, const uint8_t *data,
			 size_t size)
{
	if (!target_len || !size)
		return;

	osc_net::socket_t fd;
	{
		std::lock_guard<std::mutex> lock(impl_->target_mutex);
		fd = impl_->fd;
	}
	if (fd == osc_net::invalid_socket)
		return;

	bool wake = false;
	{
		std::lock_guard<std::mutex> lock(impl_->send_mutex);
		if (!impl_->send_running)
			return;

		/* Freshest state wins: dropping the oldest keeps controllers
		 * converging on current values after a stall. */
		if (impl_->send_queue.size() >= max_send_queue) {
			impl_->send_queue.pop_front();
			blog(LOG_WARNING, "[obs-osc] send queue full, dropped oldest datagram");
		}

		impl_->send_queue.push_back(
			{target, target_len, std::vector<uint8_t>(data, data + size)});
		wake = true;
	}
	if (wake)
		impl_->send_cv.notify_one();
}

void osc_server::send_fixed(const uint8_t *data, size_t size)
{
	sockaddr_storage target;
	socklen_t target_len = 0;

	{
		std::lock_guard<std::mutex> lock(impl_->target_mutex);
		if (impl_->resolved_len) {
			target = impl_->resolved_target;
			target_len = impl_->resolved_len;
		}
	}

	enqueue(target, target_len, data, size);
}

void osc_server::send_last(const uint8_t *data, size_t size)
{
	sockaddr_storage target;
	socklen_t target_len = 0;

	{
		std::lock_guard<std::mutex> lock(impl_->target_mutex);
		if (impl_->have_sender.load(std::memory_order_relaxed)) {
			target = impl_->last_sender;
			target_len = impl_->last_sender_len;
		}
	}

	enqueue(target, target_len, data, size);
}

void osc_server::send_to(const uint8_t *data, size_t size, const osc_net::osc_endpoint &target)
{
	enqueue(target.addr, target.len, data, size);
}

void osc_server::send_loop()
{
	std::unique_lock<std::mutex> lock(impl_->send_mutex);

	for (;;) {
		impl_->send_cv.wait(lock, [&] {
			return !impl_->send_queue.empty() || !impl_->send_running;
		});

		if (impl_->send_queue.empty())
			break; /* stopped and fully drained */

		out_packet pkt = std::move(impl_->send_queue.front());
		impl_->send_queue.pop_front();

		osc_net::socket_t fd;
		{
			std::lock_guard<std::mutex> fd_lock(impl_->target_mutex);
			fd = impl_->fd;
		}

		lock.unlock();
		if (fd != osc_net::invalid_socket)
			sendto(fd, (const char *)pkt.data.data(), (int)pkt.data.size(), 0,
			       (const sockaddr *)&pkt.target, pkt.target_len);
		lock.lock();
	}
}

void osc_server::recv_loop()
{
	std::vector<uint8_t> buf(65536);

	while (running_.load()) {
		sockaddr_storage ss;
		socklen_t slen = sizeof(ss);

		/* Snapshot under the lock: stop() mutates impl_->fd while
		 * this thread may be blocked in recvfrom(). */
		osc_net::socket_t fd;
		{
			std::lock_guard<std::mutex> lock(impl_->target_mutex);
			fd = impl_->fd;
		}
		if (fd == osc_net::invalid_socket)
			break;

		const int n = (int)recvfrom(fd, (char *)buf.data(), (int)buf.size(), 0,
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
			impl_->have_sender.store(true, std::memory_order_relaxed);
		}

		if (handler_) {
			osc_net::osc_endpoint sender{ss, slen};
			handler_(buf.data(), (size_t)n, sender);
		}
	}
}

void osc_server::begin_batch(const osc_net::osc_endpoint &to)
{
	reply_batch &b = thread_batch();

	b.to = to;
	b.active = true;
	b.messages.clear();
}

void osc_server::end_batch(void)
{
	reply_batch &b = thread_batch();

	b.active = false;
	if (b.messages.empty())
		return;

	/* A lone reply goes out verbatim; several become one #bundle
	 * datagram: header, zero timetag, then size-prefixed elements. */
	if (b.messages.size() == 1) {
		enqueue(b.to.addr, b.to.len, b.messages[0].data(), b.messages[0].size());
	} else {
		static const uint8_t bundle_header[8] = {'#', 'b', 'u', 'n', 'd', 'l', 'e', 0};
		std::vector<uint8_t> buf;

		size_t total = 16 + b.messages.size() * 4;
		for (const std::vector<uint8_t> &m : b.messages)
			total += m.size();
		buf.reserve(total);

		buf.insert(buf.end(), bundle_header, bundle_header + 8);
		buf.insert(buf.end(), {0, 0, 0, 0, 0, 0, 0, 0});
		for (const std::vector<uint8_t> &m : b.messages) {
			const uint32_t size = (uint32_t)m.size();
			buf.push_back((uint8_t)(size >> 24));
			buf.push_back((uint8_t)(size >> 16));
			buf.push_back((uint8_t)(size >> 8));
			buf.push_back((uint8_t)size);
			buf.insert(buf.end(), m.begin(), m.end());
		}
		enqueue(b.to.addr, b.to.len, buf.data(), buf.size());
	}

	b.messages.clear();
}

void osc_server::reply(const osc_net::osc_endpoint &to, const char *address,
		       std::initializer_list<osc_argument> args)
{
	reply_batch &b = thread_batch();

	/* Inside a batch, coalesce replies aimed at the batch's endpoint;
	 * anything else (should not happen) is sent directly. memcmp over
	 * len bytes is safe: both endpoints came straight from recvfrom.
	 */
	if (b.active && b.to.len == to.len && memcmp(&b.to.addr, &to.addr, to.len) == 0) {
		b.messages.emplace_back();
		osc_build_message(&b.messages.back(), address, args);
		return;
	}

	std::vector<uint8_t> buf;
	osc_build_message(&buf, address, args);
	enqueue(to.addr, to.len, buf.data(), buf.size());
}
