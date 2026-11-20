#include "adsb/feed/uring_sender.hpp"

#if ADSB_HAVE_IO_URING

#include <fcntl.h>
#include <liburing.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <vector>

#include "adsb/common/cache.hpp"

namespace adsb::feed {

namespace {
void set_error(std::string* error, const char* what, int err) {
  if (error) *error = std::string(what) + ": " + std::strerror(err);
}
}  // namespace

struct UringSender::Impl {
  UdpSender udp;   // owns the socket and its options (open)
  int adopted = -1;  // or a socket handed over (open_connected), closed after the ring
  io_uring ring{};
  bool ring_ok = false;
  bool sqpoll = false;
  std::vector<std::uint8_t> buffers;   // max_in_flight * kBufferBytes, registered
  std::vector<std::uint32_t> free;     // free buffer indices (a stack)
  std::uint64_t next_id = 0;           // submission counter (user_data high bits)
  std::uint64_t next_done = 0;         // the id the next completion should carry
  std::uint64_t in_flight = 0;

  ~Impl() {
    if (ring_ok) io_uring_queue_exit(&ring);
    close_fd(adopted);
  }
};

UringSender::UringSender() : impl_(std::make_unique<Impl>()) {}

UringSender::~UringSender() {
  if (impl_ && impl_->ring_ok) (void)drain(1000);
}

bool UringSender::built() noexcept { return true; }

int UringSender::fd() const noexcept { return impl_->adopted >= 0 ? impl_->adopted : impl_->udp.fd(); }

bool UringSender::open(const Endpoint& dest, const Options& opt, std::string* error) {
  impl_ = std::make_unique<Impl>();
  stats_ = {};
  Impl& m = *impl_;
  if (opt.max_in_flight == 0 || opt.max_in_flight > 4096) {
    if (error) *error = "max_in_flight must be 1..4096";
    return false;
  }
  if (!m.udp.open(dest, opt.udp, error)) return false;
  // write() needs a connected socket.
  sockaddr_in to{};
  if (!resolve_ipv4(dest, to, error)) return false;
  if (::connect(m.udp.fd(), reinterpret_cast<const sockaddr*>(&to), sizeof to) != 0) {
    set_error(error, "connect", errno);
    return false;
  }
  return setup(m.udp.fd(), opt, error);
}

bool UringSender::open_connected(int fd, const Options& opt, std::string* error) {
  impl_ = std::make_unique<Impl>();
  stats_ = {};
  impl_->adopted = fd;
  if (opt.max_in_flight == 0 || opt.max_in_flight > 4096) {
    if (error) *error = "max_in_flight must be 1..4096";
    return false;
  }
  return setup(fd, opt, error);
}

bool UringSender::setup(int fd, const Options& opt, std::string* error) {
  Impl& m = *impl_;
  // Non-blocking as well, for any direct use of the socket.
  const int fl = ::fcntl(fd, F_GETFL);
  if (fl < 0 || ::fcntl(fd, F_SETFL, fl | O_NONBLOCK) != 0) {
    set_error(error, "fcntl O_NONBLOCK", errno);
    return false;
  }

  io_uring_params p{};
  if (opt.sqpoll) {
    p.flags |= IORING_SETUP_SQPOLL;
    p.sq_thread_idle = opt.sq_idle_ms;
    if (opt.sq_cpu >= 0) {
      p.flags |= IORING_SETUP_SQ_AFF;
      p.sq_thread_cpu = static_cast<unsigned>(opt.sq_cpu);
    }
  }
  // Room for every write in flight, plus their completions (CQ is 2x by default).
  const int rc = io_uring_queue_init_params(opt.max_in_flight, &m.ring, &p);
  if (rc < 0) {
    set_error(error, opt.sqpoll ? "io_uring_queue_init (SQPOLL)" : "io_uring_queue_init", -rc);
    return false;
  }
  m.ring_ok = true;
  m.sqpoll = opt.sqpoll;

  if (const int r = io_uring_register_files(&m.ring, &fd, 1); r < 0) {
    set_error(error, "io_uring_register_files", -r);
    return false;
  }
  // Plain buffers, not registered: a non-zero-copy send copies the data when
  // it is issued (registration only pays off for IORING_OP_SEND_ZC). They
  // must still live until the completion: with SQPOLL the issue happens later,
  // on the kernel thread.
  m.buffers.assign(static_cast<std::size_t>(opt.max_in_flight) * kBufferBytes, 0);
  m.free.clear();
  m.free.reserve(opt.max_in_flight);  // never grows past this: no allocation in send()
  for (unsigned i = opt.max_in_flight; i-- > 0;) m.free.push_back(i);
  return true;
}

namespace {

// user_data = id << 16 | buffer index (max_in_flight <= 4096 < 2^16).
inline std::uint64_t pack(std::uint64_t id, std::uint32_t buf) noexcept { return id << 16 | buf; }

}  // namespace

void UringSender::reap(bool wait) noexcept {
  Impl& m = *impl_;
  Stats& st = stats_;
  io_uring_cqe* cqe = nullptr;
  if (wait && io_uring_wait_cqe(&m.ring, &cqe) < 0) return;
  unsigned head;
  unsigned n = 0;
  io_uring_for_each_cqe(&m.ring, head, cqe) {
    const std::uint64_t ud = io_uring_cqe_get_data64(cqe);
    const std::uint64_t id = ud >> 16;
    const auto buf = static_cast<std::uint32_t>(ud & 0xFFFF);
    if (id != m.next_done) ++st.reordered;
    m.next_done = id + 1;
    // A datagram write is all or nothing: res is its length, or -errno.
    if (cqe->res <= 0) {
      ++st.failed;
      st.last_error = cqe->res < 0 ? -cqe->res : EIO;
    }
    m.free.push_back(buf);
    --m.in_flight;
    ++st.completed;
    ++n;
  }
  io_uring_cq_advance(&m.ring, n);
}

bool UringSender::send(std::span<const std::uint8_t> dgram) noexcept {
  Impl& m = *impl_;
  if (!m.ring_ok || dgram.size() > kBufferBytes || dgram.empty()) return false;
  reap(false);
  if (m.free.empty()) {
    ++stats_.buffer_waits;
    while (m.free.empty()) reap(true);
  }
  io_uring_sqe* sqe = io_uring_get_sqe(&m.ring);
  if (sqe == nullptr) {
    // SQPOLL thread behind (or asleep): every SQE is still unconsumed.
    ++stats_.sq_full;
    while ((sqe = io_uring_get_sqe(&m.ring)) == nullptr) {
      (void)io_uring_submit(&m.ring);  // wakes the thread if it sleeps
      cpu_relax();
    }
  }
  const std::uint32_t buf = m.free.back();
  m.free.pop_back();
  std::uint8_t* p = m.buffers.data() + static_cast<std::size_t>(buf) * kBufferBytes;
  std::memcpy(p, dgram.data(), dgram.size());
  // MSG_DONTWAIT makes the request no-wait: a full socket buffer completes it
  // with -EAGAIN instead of parking it until the socket is writable, which
  // would let later datagrams overtake it (a plain write on an O_NONBLOCK
  // socket is parked; the tests caught that).
  io_uring_prep_send(sqe, 0, p, dgram.size(), MSG_DONTWAIT);
  sqe->flags |= IOSQE_FIXED_FILE;
  io_uring_sqe_set_data64(sqe, pack(m.next_id++, buf));
  ++m.in_flight;
  ++stats_.queued;
  if (m.sqpoll) {
    // io_uring_submit only enters the kernel when the thread needs waking.
    if (IO_URING_READ_ONCE(*m.ring.sq.kflags) & IORING_SQ_NEED_WAKEUP) ++stats_.wakeups;
  } else {
    ++stats_.submit_calls;
  }
  (void)io_uring_submit(&m.ring);
  return true;
}

bool UringSender::drain(unsigned timeout_ms) noexcept {
  Impl& m = *impl_;
  if (!m.ring_ok) return true;
  const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (m.in_flight > 0 && std::chrono::steady_clock::now() < end) {
    (void)io_uring_submit(&m.ring);
    reap(false);
    if (m.in_flight > 0) cpu_relax();
  }
  return m.in_flight == 0;
}

}  // namespace adsb::feed

#else  // !ADSB_HAVE_IO_URING

namespace adsb::feed {

struct UringSender::Impl {};

UringSender::UringSender() : impl_(std::make_unique<Impl>()) {}
UringSender::~UringSender() = default;
bool UringSender::built() noexcept { return false; }
int UringSender::fd() const noexcept { return -1; }
bool UringSender::open(const Endpoint&, const Options&, std::string* error) {
  if (error) *error = "this build has no io_uring support (Linux with liburing only)";
  return false;
}
bool UringSender::open_connected(int fd, const Options&, std::string* error) {
  close_fd(fd);
  if (error) *error = "this build has no io_uring support (Linux with liburing only)";
  return false;
}
bool UringSender::setup(int, const Options&, std::string*) { return false; }
bool UringSender::send(std::span<const std::uint8_t>) noexcept { return false; }
bool UringSender::drain(unsigned) noexcept { return true; }

}  // namespace adsb::feed

#endif
