#include "adsb/feed/xdp_sender.hpp"

#include <arpa/inet.h>

#include <cerrno>
#include <cstdio>
#include <cstring>

// ---------------------------------------------------------------- portable

namespace adsb::feed {

namespace {
int hex_digit(char c) noexcept {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}
void put16(std::uint8_t* p, std::uint16_t v) noexcept {
  p[0] = static_cast<std::uint8_t>(v >> 8);
  p[1] = static_cast<std::uint8_t>(v);
}
// Sum of big-endian 16-bit words (an odd last byte is padded with zero).
std::uint64_t word_sum(const std::uint8_t* p, std::size_t n) noexcept {
  std::uint64_t s = 0;
  std::size_t i = 0;
  for (; i + 1 < n; i += 2) s += static_cast<std::uint64_t>(p[i]) << 8 | p[i + 1];
  if (i < n) s += static_cast<std::uint64_t>(p[i]) << 8;
  return s;
}
std::uint16_t fold(std::uint64_t s) noexcept {
  while (s >> 16) s = (s & 0xFFFF) + (s >> 16);
  return static_cast<std::uint16_t>(~s & 0xFFFF);
}
}  // namespace

bool parse_mac(std::string_view text, MacAddress& out) noexcept {
  if (text.size() != 17) return false;
  MacAddress m{};
  for (std::size_t i = 0; i < 6; ++i) {
    const int hi = hex_digit(text[i * 3]), lo = hex_digit(text[i * 3 + 1]);
    if (hi < 0 || lo < 0) return false;
    if (i < 5 && text[i * 3 + 2] != ':') return false;
    m[i] = static_cast<std::uint8_t>(hi << 4 | lo);
  }
  out = m;
  return true;
}

std::string to_string(const MacAddress& m) {
  char buf[18];
  std::snprintf(buf, sizeof buf, "%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
  return buf;
}

MacAddress multicast_mac(std::uint32_t group_be) noexcept {
  const std::uint32_t g = ntohl(group_be);
  return {0x01, 0x00, 0x5e, static_cast<std::uint8_t>((g >> 16) & 0x7F),
          static_cast<std::uint8_t>(g >> 8), static_cast<std::uint8_t>(g)};
}

std::uint16_t inet_checksum(std::span<const std::uint8_t> data, std::uint32_t sum) noexcept {
  return fold(word_sum(data.data(), data.size()) + sum);
}

void UdpFrameBuilder::init(const MacAddress& src_mac, const MacAddress& dst_mac, std::uint32_t src_ip_be,
                           std::uint32_t dst_ip_be, std::uint16_t src_port, std::uint16_t dst_port,
                           std::uint8_t ttl) noexcept {
  std::uint8_t* h = tmpl_.data();
  std::memcpy(h, dst_mac.data(), 6);
  std::memcpy(h + 6, src_mac.data(), 6);
  put16(h + 12, 0x0800);  // IPv4
  std::uint8_t* ip = h + 14;
  ip[0] = 0x45;           // version 4, 5-word header
  ip[1] = 0;              // DSCP/ECN
  put16(ip + 2, 0);       // total length: per frame
  put16(ip + 4, 0);       // id 0: DF is set and we never fragment (RFC 6864)
  put16(ip + 6, 0x4000);  // DF
  ip[8] = ttl;
  ip[9] = 17;             // UDP
  put16(ip + 10, 0);      // header checksum: per frame
  std::memcpy(ip + 12, &src_ip_be, 4);
  std::memcpy(ip + 16, &dst_ip_be, 4);
  std::uint8_t* udp = ip + 20;
  put16(udp, src_port);
  put16(udp + 2, dst_port);
  put16(udp + 4, 0);  // length: per frame
  put16(udp + 6, 0);  // checksum: per frame
  // Pseudo-header (addresses, protocol) + the fixed UDP header words (ports).
  pseudo_sum_ = static_cast<std::uint32_t>(word_sum(ip + 12, 8) + 17 + src_port + dst_port);
}

std::size_t UdpFrameBuilder::build(std::span<const std::uint8_t> payload, std::uint8_t* out) const noexcept {
  std::memcpy(out, tmpl_.data(), kHeaderBytes);
  if (!payload.empty()) std::memcpy(out + kHeaderBytes, payload.data(), payload.size());  // data() may be null
  const auto udp_len = static_cast<std::uint16_t>(8 + payload.size());
  std::uint8_t* ip = out + 14;
  put16(ip + 2, static_cast<std::uint16_t>(20 + udp_len));
  put16(ip + 10, inet_checksum({ip, 20}));
  std::uint8_t* udp = ip + 20;
  put16(udp + 4, udp_len);
  // Pseudo-header length + UDP length field (the same value twice) + data.
  std::uint16_t c = fold(word_sum(payload.data(), payload.size()) + pseudo_sum_ + 2u * udp_len);
  if (c == 0) c = 0xFFFF;  // 0 means "no checksum" in IPv4 UDP
  put16(udp + 6, c);
  return kHeaderBytes + payload.size();
}

bool arp_lookup(const std::string& ipv4, const std::string& device, MacAddress& out, const char* table) {
  std::FILE* f = std::fopen(table, "r");
  if (!f) return false;
  char line[256];
  bool found = false;
  bool header = true;  // the first line names the columns
  while (!found && std::fgets(line, sizeof line, f)) {
    if (header) {
      header = false;
      continue;
    }
    char ip[64], mac[64], dev[64];
    unsigned type = 0, flags = 0;
    if (std::sscanf(line, "%63s 0x%x 0x%x %63s %*s %63s", ip, &type, &flags, mac, dev) != 5) continue;
    found = ipv4 == ip && device == dev && (flags & 0x2) != 0 && parse_mac(mac, out);
  }
  std::fclose(f);
  return found;
}

bool XdpSender::parse_mode(std::string_view s, Mode& out) noexcept {
  if (s == "auto") out = Mode::kAuto;
  else if (s == "copy") out = Mode::kCopy;
  else if (s == "zerocopy") out = Mode::kZeroCopy;
  else return false;
  return true;
}

const char* XdpSender::mode_name(Mode m) noexcept {
  switch (m) {
    case Mode::kAuto: return "auto";
    case Mode::kCopy: return "copy";
    case Mode::kZeroCopy: return "zerocopy";
  }
  return "?";
}

}  // namespace adsb::feed

// ---------------------------------------------------------- Linux devices

#if defined(__linux__)

#include <ifaddrs.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

namespace adsb::feed {

bool interface_for_address(const std::string& ipv4, InterfaceInfo& out, std::string* error) {
  in_addr want{};
  if (::inet_pton(AF_INET, ipv4.c_str(), &want) != 1) {
    if (error) *error = "not an IPv4 address: " + ipv4;
    return false;
  }
  ifaddrs* list = nullptr;
  if (::getifaddrs(&list) != 0) {
    if (error) *error = std::string("getifaddrs: ") + std::strerror(errno);
    return false;
  }
  std::string name;
  for (ifaddrs* a = list; a; a = a->ifa_next) {
    if (a->ifa_addr && a->ifa_addr->sa_family == AF_INET &&
        reinterpret_cast<const sockaddr_in*>(a->ifa_addr)->sin_addr.s_addr == want.s_addr) {
      name = a->ifa_name;
      break;
    }
  }
  ::freeifaddrs(list);
  if (name.empty() || name.size() >= IFNAMSIZ) {
    if (error) *error = "no interface has address " + ipv4;
    return false;
  }
  const int s = ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
  if (s < 0) {
    if (error) *error = std::string("socket: ") + std::strerror(errno);
    return false;
  }
  ifreq r{};
  std::memcpy(r.ifr_name, name.c_str(), name.size() + 1);
  bool ok = ::ioctl(s, SIOCGIFHWADDR, &r) == 0;
  if (ok) std::memcpy(out.mac.data(), r.ifr_hwaddr.sa_data, 6);
  ok = ok && ::ioctl(s, SIOCGIFMTU, &r) == 0;
  if (ok) out.mtu = static_cast<unsigned>(r.ifr_mtu);
  const int e = errno;
  ::close(s);
  if (!ok) {
    if (error) *error = "ioctl on " + name + ": " + std::strerror(e);
    return false;
  }
  out.name = name;
  out.index = ::if_nametoindex(name.c_str());
  return out.index != 0;
}


}  // namespace adsb::feed

#else

namespace adsb::feed {
bool interface_for_address(const std::string&, InterfaceInfo&, std::string* error) {
  if (error) *error = "interface lookup is Linux only";
  return false;
}
}  // namespace adsb::feed

#endif

// -------------------------------------------------------------- AF_XDP

#if ADSB_HAVE_AF_XDP

#include <net/if.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <thread>
#include <vector>

#include "xsk_ring.hpp"

namespace adsb::feed {

using xsk::load_acquire;
using xsk::Ring;
using xsk::set_error;
using xsk::store_release;

struct XdpSender::Impl {
  int fd = -1;
  void* umem = MAP_FAILED;
  std::size_t umem_len = 0;
  Ring tx, cq;
  std::vector<std::uint64_t> free;  // free frame addresses (a stack)
  std::uint32_t tx_prod = 0;        // our TX producer index
  std::uint32_t cq_cons = 0;        // our completion consumer index
  std::uint64_t in_flight = 0;
  UdpFrameBuilder builder;
  InterfaceInfo dev;
  Mode bound = Mode::kAuto;
  std::size_t max_payload = 0;

  // The rings unmap after this body (member destructors); the kernel keeps
  // the UMEM pages pinned until the socket is fully released, so the order
  // of these unmaps does not matter.
  ~Impl() {
    if (fd >= 0) ::close(fd);
    if (umem != MAP_FAILED) ::munmap(umem, umem_len);
  }
};

XdpSender::XdpSender() = default;

XdpSender::~XdpSender() {
  if (impl_ && impl_->fd >= 0) (void)drain(1000);
}

bool XdpSender::built() noexcept { return true; }

XdpSender::Mode XdpSender::bound_mode() const noexcept { return impl_ ? impl_->bound : Mode::kAuto; }

const InterfaceInfo& XdpSender::device() const noexcept {
  static const InterfaceInfo none;
  return impl_ ? impl_->dev : none;
}

std::size_t XdpSender::max_payload() const noexcept { return impl_ ? impl_->max_payload : 0; }

bool XdpSender::open(const Endpoint& dest, const Options& opt, std::string* error) {
  impl_.reset();
  stats_ = {};
  auto m = std::make_unique<Impl>();
  const unsigned n = opt.frames;
  if (n < 2 || n > 65536 || (n & (n - 1)) != 0) {
    if (error) *error = "frames must be a power of two, 2..65536";
    return false;
  }
  sockaddr_in to{}, from{};
  if (!resolve_ipv4(dest, to, error)) return false;
  if (!resolve_ipv4(Endpoint{opt.udp.interface_host, 0}, from, error)) return false;
  if (from.sin_addr.s_addr == htonl(INADDR_ANY)) {
    if (error) *error = "AF_XDP needs the sending device's address as the interface (not 0.0.0.0)";
    return false;
  }
  if (!interface_for_address(opt.udp.interface_host, m->dev, error)) return false;
  MacAddress dst{};
  if (is_multicast(to)) {
    dst = multicast_mac(to.sin_addr.s_addr);
  } else if (!opt.dst_mac.empty()) {
    if (!parse_mac(opt.dst_mac, dst)) {
      if (error) *error = "bad destination MAC: " + opt.dst_mac;
      return false;
    }
  } else if (!arp_lookup(dest.host, m->dev.name, dst)) {
    if (error) {
      *error = "no ARP entry for " + dest.host + " on " + m->dev.name +
               " (pass the destination MAC, or reach the host once with kernel UDP/ping)";
    }
    return false;
  }
  const auto port = ntohs(to.sin_port);
  m->builder.init(m->dev.mac, dst, from.sin_addr.s_addr, to.sin_addr.s_addr, opt.src_port ? opt.src_port : port,
                  port, static_cast<std::uint8_t>(opt.udp.ttl));
  const std::size_t by_mtu = m->dev.mtu > 28 ? m->dev.mtu - 28 : 0;
  const std::size_t by_frame = kFrameBytes - UdpFrameBuilder::kHeaderBytes;
  m->max_payload = by_mtu < by_frame ? by_mtu : by_frame;

  m->fd = ::socket(AF_XDP, SOCK_RAW | SOCK_CLOEXEC, 0);
  if (m->fd < 0) {
    set_error(error, "AF_XDP socket (needs CAP_NET_RAW)", errno);
    return false;
  }
  m->umem_len = static_cast<std::size_t>(n) * kFrameBytes;
  m->umem = ::mmap(nullptr, m->umem_len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (m->umem == MAP_FAILED) {
    set_error(error, "mmap UMEM", errno);
    return false;
  }
  xdp_umem_reg reg{};
  reg.addr = reinterpret_cast<std::uint64_t>(m->umem);
  reg.len = m->umem_len;
  reg.chunk_size = kFrameBytes;
  reg.headroom = 0;
  if (::setsockopt(m->fd, SOL_XDP, XDP_UMEM_REG, &reg, sizeof reg) != 0) {
    xsk::set_umem_error(error, m->umem_len, errno);
    return false;
  }
  // The fill ring feeds RX; a TX-only socket still needs one to bind.
  const int fill = 64, ring = static_cast<int>(n);
  if (::setsockopt(m->fd, SOL_XDP, XDP_UMEM_FILL_RING, &fill, sizeof fill) != 0 ||
      ::setsockopt(m->fd, SOL_XDP, XDP_UMEM_COMPLETION_RING, &ring, sizeof ring) != 0 ||
      ::setsockopt(m->fd, SOL_XDP, XDP_TX_RING, &ring, sizeof ring) != 0) {
    set_error(error, "XDP ring sizes", errno);
    return false;
  }
  xdp_mmap_offsets off{};
  socklen_t len = sizeof off;
  if (::getsockopt(m->fd, SOL_XDP, XDP_MMAP_OFFSETS, &off, &len) != 0) {
    set_error(error, "XDP_MMAP_OFFSETS", errno);
    return false;
  }
  if (!m->tx.mmap_ring(m->fd, off.tx, n, sizeof(xdp_desc), XDP_PGOFF_TX_RING) ||
      !m->cq.mmap_ring(m->fd, off.cr, n, sizeof(std::uint64_t), static_cast<off_t>(XDP_UMEM_PGOFF_COMPLETION_RING))) {
    set_error(error, "mmap XDP rings", errno);
    return false;
  }
  sockaddr_xdp sx{};
  sx.sxdp_family = AF_XDP;
  sx.sxdp_ifindex = m->dev.index;
  sx.sxdp_queue_id = opt.queue;
  sx.sxdp_flags = XDP_USE_NEED_WAKEUP;
  if (opt.mode == Mode::kCopy) sx.sxdp_flags |= XDP_COPY;
  if (opt.mode == Mode::kZeroCopy) sx.sxdp_flags |= XDP_ZEROCOPY;
  // A queue takes one XSK. The kernel lets go of a closed socket's queue
  // asynchronously (deferred work), so a publisher restarted at once (or a
  // second open in one process) briefly sees EBUSY: retry for up to 1 s.
  const auto bind_until = std::chrono::steady_clock::now() + std::chrono::seconds(1);
  while (::bind(m->fd, reinterpret_cast<const sockaddr*>(&sx), sizeof sx) != 0) {
    const int e = errno;
    if (e == EBUSY && std::chrono::steady_clock::now() < bind_until) {
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
      continue;
    }
    set_error(error, "bind AF_XDP to " + m->dev.name + " queue " + std::to_string(opt.queue) + " (" +
                         mode_name(opt.mode) + (e == EBUSY ? "; another socket holds this queue" : "") + ")",
              e);
    return false;
  }
  xdp_options xo{};
  len = sizeof xo;
  m->bound = ::getsockopt(m->fd, SOL_XDP, XDP_OPTIONS, &xo, &len) == 0 && (xo.flags & XDP_OPTIONS_ZEROCOPY)
                 ? Mode::kZeroCopy
                 : Mode::kCopy;
  m->free.reserve(n);
  for (unsigned i = n; i-- > 0;) m->free.push_back(static_cast<std::uint64_t>(i) * kFrameBytes);
  impl_ = std::move(m);
  return true;
}

void XdpSender::reap() noexcept {
  Impl& m = *impl_;
  const std::uint32_t prod = load_acquire(m.cq.producer);
  const auto* addrs = static_cast<const std::uint64_t*>(m.cq.desc);
  for (std::uint32_t c = m.cq_cons; c != prod; ++c) {
    // In aligned mode the kernel may hand back an address inside the frame.
    m.free.push_back(addrs[c & m.cq.mask] & ~static_cast<std::uint64_t>(kFrameBytes - 1));
  }
  const std::uint32_t done = prod - m.cq_cons;
  if (done != 0) {
    m.cq_cons = prod;
    store_release(m.cq.consumer, prod);
    stats_.completed += done;
    m.in_flight -= done;
  }
}

void XdpSender::kick() noexcept {
  Impl& m = *impl_;
  if ((__atomic_load_n(m.tx.flags, __ATOMIC_RELAXED) & XDP_RING_NEED_WAKEUP) == 0) return;
  ++stats_.kicks;
  // The kernel sends at most a batch per call and keeps what it could not
  // send on the ring; retry a few times so nothing waits for the next send.
  for (int i = 0; i < 8; ++i) {
    if (::sendto(m.fd, nullptr, 0, MSG_DONTWAIT, nullptr, 0) >= 0) return;
    const int e = errno;
    if (e != EAGAIN && e != EBUSY && e != ENOBUFS) {
      ++stats_.kick_errors;
      stats_.last_error = e;
      return;
    }
    ++stats_.kick_retries;
    if (load_acquire(m.tx.consumer) == m.tx_prod) return;
  }
}

bool XdpSender::send(std::span<const std::uint8_t> dgram) noexcept {
  if (!impl_ || dgram.empty() || dgram.size() > impl_->max_payload) return false;
  Impl& m = *impl_;
  reap();
  if (m.free.empty()) {
    ++stats_.frame_waits;
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
    while (m.free.empty()) {
      kick();
      reap();
      if (m.free.empty() && std::chrono::steady_clock::now() > end) return false;
    }
  }
  const std::uint64_t addr = m.free.back();
  m.free.pop_back();
  const std::size_t len = m.builder.build(dgram, static_cast<std::uint8_t*>(m.umem) + addr);
  auto* d = static_cast<xdp_desc*>(m.tx.desc) + (m.tx_prod & m.tx.mask);
  d->addr = addr;
  d->len = static_cast<std::uint32_t>(len);
  d->options = 0;
  store_release(m.tx.producer, ++m.tx_prod);
  ++stats_.queued;
  ++m.in_flight;
  kick();
  return true;
}

bool XdpSender::drain(unsigned timeout_ms) noexcept {
  if (!impl_) return true;
  const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  for (;;) {
    reap();
    if (impl_->in_flight == 0) return true;
    if (std::chrono::steady_clock::now() > end) return false;
    kick();
    std::this_thread::yield();
  }
}

void XdpSender::refresh_kernel_stats() noexcept {
  if (!impl_) return;
  xdp_statistics st{};
  socklen_t len = sizeof st;
  if (::getsockopt(impl_->fd, SOL_XDP, XDP_STATISTICS, &st, &len) == 0) stats_.tx_invalid = st.tx_invalid_descs;
}

}  // namespace adsb::feed

#else  // no AF_XDP in this build

namespace adsb::feed {

struct XdpSender::Impl {};

XdpSender::XdpSender() = default;
XdpSender::~XdpSender() = default;
bool XdpSender::built() noexcept { return false; }
bool XdpSender::open(const Endpoint&, const Options&, std::string* error) {
  if (error) *error = "this build has no AF_XDP support (Linux only)";
  return false;
}
bool XdpSender::send(std::span<const std::uint8_t>) noexcept { return false; }
bool XdpSender::drain(unsigned) noexcept { return true; }
void XdpSender::refresh_kernel_stats() noexcept {}
XdpSender::Mode XdpSender::bound_mode() const noexcept { return Mode::kAuto; }
const InterfaceInfo& XdpSender::device() const noexcept {
  static const InterfaceInfo none;
  return none;
}
std::size_t XdpSender::max_payload() const noexcept { return 0; }
void XdpSender::reap() noexcept {}
void XdpSender::kick() noexcept {}

}  // namespace adsb::feed

#endif
