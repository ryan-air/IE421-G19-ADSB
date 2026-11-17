#include "adsb/feed/recovery.hpp"

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

#include "adsb/common/clock.hpp"
#include "adsb/feed/net.hpp"

namespace adsb::feed {

namespace {

std::size_t round_up_pow2(std::size_t n) {
  if (n > RetransmitStore::kMaxCapacity) n = RetransmitStore::kMaxCapacity;
  std::size_t p = 1;
  while (p < n) p <<= 1;
  return p;
}

#if defined(MSG_NOSIGNAL)
constexpr int kSendFlags = MSG_NOSIGNAL;
#else
constexpr int kSendFlags = 0;  // SO_NOSIGPIPE is set on the socket instead
#endif

}  // namespace

// ---------------------------------------------------------------------------
// RetransmitStore
// ---------------------------------------------------------------------------

RetransmitStore::RetransmitStore(std::size_t capacity, std::uint64_t first_seq)
    : slots_(round_up_pow2(capacity < 1 ? 1 : capacity)),
      mask_(slots_.size() - 1),
      first_(first_seq),
      next_(first_seq) {}

bool RetransmitStore::add(std::uint64_t seq, std::span<const std::uint8_t> msg) {
  if (seq != next_ || msg.size() > kMaxMessageBytes) return false;
  LogEntry& e = slots_[seq & mask_];
  e.seq = seq;
  e.len = static_cast<std::uint16_t>(msg.size());
  std::memcpy(e.bytes, msg.data(), msg.size());
  ++next_;
  return true;
}

std::uint64_t RetransmitStore::oldest() const noexcept {
  const std::uint64_t held = next_ - first_;
  return held > slots_.size() ? next_ - slots_.size() : first_;
}

const LogEntry* RetransmitStore::get(std::uint64_t seq) const noexcept {
  if (seq < oldest() || seq >= next_) return nullptr;
  return &slots_[seq & mask_];
}

// ---------------------------------------------------------------------------
// RecoveryState
// ---------------------------------------------------------------------------

RecoveryState::RecoveryState(const RecoveryConfig& cfg)
    : cfg_(cfg), store_(cfg.ring_capacity, cfg.first_seq) {
  if (cfg_.max_reply_messages == 0) cfg_.max_reply_messages = 1;
}

void RecoveryState::on_message(std::uint64_t seq, std::span<const std::uint8_t> msg) {
  if (!store_.add(seq, msg)) {
    ++stats_.log_errors;
    return;
  }
  ++stats_.logged;
  book_.apply(msg);
  // Event time of the last message, for SnapshotBegin.event_ts_ns. Both
  // TrackUpdate and TrackDelete carry event_ts_ns at offset 8.
  if (msg.size() >= 16 && (msg[0] == static_cast<std::uint8_t>(protocol::TrackUpdate::kTypeChar) ||
                           msg[0] == static_cast<std::uint8_t>(protocol::TrackDelete::kTypeChar))) {
    std::memcpy(&last_event_ts_, msg.data() + 8, 8);
  }
}

std::size_t RecoveryState::drain(MessageLog& log) {
  std::size_t n = 0;
  LogEntry e;
  while (log.try_pop(e)) {
    on_message(e.seq, std::span<const std::uint8_t>(e.bytes, e.len));
    ++n;
  }
  return n;
}

void RecoveryState::make_snapshot(std::vector<std::uint8_t>& out) {
  ++stats_.snapshots;
  serialize_snapshot(cfg_.session, last_seq(), last_event_ts_, book_, out);
}

// ---------------------------------------------------------------------------
// RecoveryServer
// ---------------------------------------------------------------------------

RecoveryServer::RecoveryServer(const RecoveryServerConfig& cfg, MessageLog& log)
    : cfg_(cfg), log_(log), state_(cfg.recovery), reply_loss_(cfg.reply_loss) {}

RecoveryServer::~RecoveryServer() {
  close_fd(rtx_fd_);
  close_fd(listen_fd_);
  for (Client& c : clients_) close_fd(c.fd);
}

bool RecoveryServer::open(std::string* error) {
  rtx_fd_ = open_udp(cfg_.bind_host, cfg_.retransmit_port, 4 << 20, error);
  if (rtx_fd_ < 0) return false;
  listen_fd_ = open_tcp_listener(cfg_.bind_host, cfg_.snapshot_port, 16, error);
  if (listen_fd_ < 0) return false;
  rtx_port_ = local_port(rtx_fd_);
  snap_port_ = local_port(listen_fd_);
  const int sndbuf = 4 << 20;
  setsockopt(rtx_fd_, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof sndbuf);
  return true;
}

void RecoveryServer::drain() {
  const std::size_t n = state_.drain(log_);
  if (n) counters_.logged.fetch_add(n, std::memory_order_relaxed);
}

void RecoveryServer::serve_requests() {
  std::uint8_t buf[2048];
  // Bounded per pass, so a flood of requests cannot starve snapshot clients
  // or the stop flag.
  for (int pass = 0; pass < 64; ++pass) {
    sockaddr_in from{};
    socklen_t flen = sizeof from;
    const ssize_t n = ::recvfrom(rtx_fd_, buf, sizeof buf, 0, reinterpret_cast<sockaddr*>(&from), &flen);
    if (n < 0) return;  // EAGAIN (or a transient error): nothing more now
    drain();            // answer from the freshest log
    const RecoveryStats before = state_.stats();
    state_.handle_request(std::span<const std::uint8_t>(buf, static_cast<std::size_t>(n)),
                          [&](std::span<const std::uint8_t> d) {
                            if (reply_loss_.drop()) {
                              counters_.replies_dropped.fetch_add(1, std::memory_order_relaxed);
                              return;
                            }
                            (void)::sendto(rtx_fd_, d.data(), d.size(), 0,
                                           reinterpret_cast<const sockaddr*>(&from), flen);
                          });
    const RecoveryStats& after = state_.stats();
    counters_.requests.fetch_add(1, std::memory_order_relaxed);
    counters_.naks.fetch_add(after.naks - before.naks, std::memory_order_relaxed);
    counters_.served_messages.fetch_add(after.served_messages - before.served_messages,
                                        std::memory_order_relaxed);
  }
}

void RecoveryServer::accept_clients() {
  for (;;) {
    const int fd = ::accept(listen_fd_, nullptr, nullptr);
    if (fd < 0) return;
    if (static_cast<int>(clients_.size()) >= cfg_.max_snapshot_clients) {
      ::close(fd);
      counters_.snapshot_clients_dropped.fetch_add(1, std::memory_order_relaxed);
      continue;
    }
#if defined(SO_NOSIGPIPE)
    const int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
    const int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    drain();  // the snapshot is as of the latest logged message
    Client c;
    c.fd = fd;
    c.start_ns = mono_ns();
    state_.make_snapshot(c.buf);
    counters_.snapshots.fetch_add(1, std::memory_order_relaxed);
    clients_.push_back(std::move(c));
  }
}

void RecoveryServer::write_clients(const std::vector<short>& revents, std::size_t first_index) {
  const std::uint64_t now = mono_ns();
  const std::uint64_t timeout_ns = std::uint64_t{cfg_.snapshot_client_timeout_ms} * 1'000'000;
  std::size_t keep = 0;
  for (std::size_t i = 0; i < clients_.size(); ++i) {
    Client& c = clients_[i];
    bool done = false;
    const short ev = first_index + i < revents.size() ? revents[first_index + i] : short{0};
    if (ev & (POLLERR | POLLHUP | POLLNVAL)) {
      done = true;
    } else if (ev & POLLOUT) {
      while (c.off < c.buf.size()) {
        const ssize_t n = ::send(c.fd, c.buf.data() + c.off, c.buf.size() - c.off, kSendFlags);
        if (n > 0) {
          c.off += static_cast<std::size_t>(n);
          continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) break;
        done = true;  // peer went away
        break;
      }
      if (c.off == c.buf.size()) done = true;
    }
    if (!done && now - c.start_ns > timeout_ns) {
      done = true;
      counters_.snapshot_clients_dropped.fetch_add(1, std::memory_order_relaxed);
    }
    if (done) {
      if (c.off == c.buf.size()) ::shutdown(c.fd, SHUT_WR);
      ::close(c.fd);
    } else {
      if (keep != i) clients_[keep] = std::move(c);
      ++keep;
    }
  }
  clients_.resize(keep);
}

void RecoveryServer::run(const std::atomic<bool>& stop) {
  std::vector<pollfd> fds;
  std::vector<short> revents;
  while (!stop.load(std::memory_order_relaxed)) {
    fds.clear();
    fds.push_back(pollfd{rtx_fd_, POLLIN, 0});
    fds.push_back(pollfd{listen_fd_, POLLIN, 0});
    for (const Client& c : clients_) fds.push_back(pollfd{c.fd, POLLOUT, 0});
    const int rc = ::poll(fds.data(), static_cast<nfds_t>(fds.size()), 1);
    drain();
    if (rc > 0) {
      if (fds[0].revents & POLLIN) serve_requests();
      if (fds[1].revents & POLLIN) accept_clients();
    }
    revents.assign(fds.size(), 0);
    for (std::size_t i = 0; i < fds.size(); ++i) revents[i] = fds[i].revents;
    // Clients accepted in this iteration are not in `fds` yet; they get
    // POLLOUT on the next pass.
    write_clients(revents, 2);
  }
  drain();
}

}  // namespace adsb::feed
