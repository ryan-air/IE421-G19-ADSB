#include "adsb/feed/xdp_receiver.hpp"

#include <arpa/inet.h>

#include <array>
#include <cstring>

// ---------------------------------------------------------------- portable

namespace adsb::feed {

namespace {

// eBPF opcodes used by the program (include/uapi/linux/bpf_common.h, bpf.h).
constexpr std::uint8_t kLdxW = 0x61;     // BPF_LDX | BPF_MEM | BPF_W
constexpr std::uint8_t kLdxH = 0x69;     // ... BPF_H
constexpr std::uint8_t kLdxB = 0x71;     // ... BPF_B
constexpr std::uint8_t kMov64X = 0xbf;   // BPF_ALU64 | BPF_MOV | BPF_X
constexpr std::uint8_t kMov64K = 0xb7;   // BPF_ALU64 | BPF_MOV | BPF_K
constexpr std::uint8_t kAdd64K = 0x07;   // BPF_ALU64 | BPF_ADD | BPF_K
constexpr std::uint8_t kAnd64K = 0x57;   // BPF_ALU64 | BPF_AND | BPF_K
constexpr std::uint8_t kJgtX = 0x2d;     // BPF_JMP | BPF_JGT | BPF_X
constexpr std::uint8_t kJneK = 0x55;     // BPF_JMP | BPF_JNE | BPF_K
constexpr std::uint8_t kJne32K = 0x56;   // BPF_JMP32 | BPF_JNE | BPF_K
constexpr std::uint8_t kLdImm64 = 0x18;  // BPF_LD | BPF_IMM | BPF_DW (two slots)
constexpr std::uint8_t kCall = 0x85;     // BPF_JMP | BPF_CALL
constexpr std::uint8_t kExit = 0x95;     // BPF_JMP | BPF_EXIT

constexpr BpfInsn insn(std::uint8_t code, unsigned dst, unsigned src, std::int16_t off, std::int32_t imm) {
  return BpfInsn{code, static_cast<std::uint8_t>((dst & 0xF) | (src & 0xF) << 4), off, imm};
}

// The value a load of these network-order bytes produces (packet loads in
// eBPF are in host order): the bytes copied into a host integer.
std::int32_t as_loaded16(std::uint16_t network_value) noexcept {
  const std::uint8_t b[2] = {static_cast<std::uint8_t>(network_value >> 8),
                             static_cast<std::uint8_t>(network_value)};
  std::uint16_t v;
  std::memcpy(&v, b, 2);
  return v;
}
std::int32_t as_loaded32(std::uint32_t value_be) noexcept {  // already network order in memory
  std::int32_t v;
  std::memcpy(&v, &value_be, 4);
  return v;
}

}  // namespace

std::vector<BpfInsn> xdp_feed_program(int map_fd, std::uint32_t dst_ip_be, std::uint16_t dst_port) {
  // r1 = struct xdp_md *: data @0, data_end @4, rx_queue_index @16 (u32s).
  // Equivalent C:
  //   void *d = (void *)(long)ctx->data, *end = (void *)(long)ctx->data_end;
  //   if (d + 42 > end) return XDP_PASS;
  //   if (eth->h_proto != htons(ETH_P_IP) || ip->version_ihl != 0x45) return XDP_PASS;
  //   if (ip->frag_off & htons(0x3fff)) return XDP_PASS;   // MF or an offset
  //   if (ip->protocol != IPPROTO_UDP) return XDP_PASS;
  //   if (dst_ip && ip->daddr != dst_ip) return XDP_PASS;
  //   if (udp->dest != htons(dst_port)) return XDP_PASS;
  //   return bpf_redirect_map(&xsks, ctx->rx_queue_index, XDP_PASS);
  // Built in fixed arrays (a growing vector trips a GCC stringop-overflow
  // false positive under -Werror).
  struct Code {
    std::array<BpfInsn, 32> insns{};
    std::size_t n = 0;
    void push_back(BpfInsn i) noexcept { insns[n++] = i; }
    [[nodiscard]] std::size_t size() const noexcept { return n; }
  } p;
  std::array<std::size_t, 16> to_pass{};  // jumps to patch
  std::size_t jumps = 0;
  auto jump_to_pass = [&](BpfInsn i) {
    to_pass[jumps++] = p.size();
    p.push_back(i);
  };
  p.push_back(insn(kMov64X, 6, 1, 0, 0));                  // r6 = ctx
  p.push_back(insn(kLdxW, 2, 6, 0, 0));                    // r2 = data
  p.push_back(insn(kLdxW, 3, 6, 4, 0));                    // r3 = data_end
  p.push_back(insn(kMov64X, 4, 2, 0, 0));                  // r4 = data
  p.push_back(insn(kAdd64K, 4, 0, 0, static_cast<std::int32_t>(kUdpFrameHeaderBytes)));
  jump_to_pass(insn(kJgtX, 4, 3, 0, 0));                   // if data + 42 > end: pass
  p.push_back(insn(kLdxH, 5, 2, 12, 0));                   // EtherType
  jump_to_pass(insn(kJneK, 5, 0, 0, as_loaded16(0x0800)));
  p.push_back(insn(kLdxB, 5, 2, 14, 0));                   // version and IHL
  jump_to_pass(insn(kJneK, 5, 0, 0, 0x45));
  p.push_back(insn(kLdxH, 5, 2, 20, 0));                   // flags and fragment offset
  p.push_back(insn(kAnd64K, 5, 0, 0, as_loaded16(0x3FFF)));
  jump_to_pass(insn(kJneK, 5, 0, 0, 0));
  p.push_back(insn(kLdxB, 5, 2, 23, 0));                   // protocol
  jump_to_pass(insn(kJneK, 5, 0, 0, 17));
  if (dst_ip_be != 0) {
    p.push_back(insn(kLdxW, 5, 2, 30, 0));                 // destination address
    jump_to_pass(insn(kJne32K, 5, 0, 0, as_loaded32(dst_ip_be)));  // 32-bit compare: no sign extension
  }
  p.push_back(insn(kLdxH, 5, 2, 36, 0));                   // UDP destination port
  jump_to_pass(insn(kJneK, 5, 0, 0, as_loaded16(dst_port)));
  p.push_back(insn(kLdxW, 2, 6, 16, 0));                   // r2 = rx_queue_index (the map key)
  p.push_back(insn(kLdImm64, 1, kBpfPseudoMapFd, 0, map_fd));  // r1 = the XSKMAP
  p.push_back(insn(0, 0, 0, 0, 0));                        //   (upper 32 bits)
  p.push_back(insn(kMov64K, 3, 0, 0, kXdpPass));           // r3 = fallback action: no socket on this queue
  p.push_back(insn(kCall, 0, 0, 0, kBpfFuncRedirectMap));
  p.push_back(insn(kExit, 0, 0, 0, 0));
  const std::size_t pass = p.size();
  p.push_back(insn(kMov64K, 0, 0, 0, kXdpPass));
  p.push_back(insn(kExit, 0, 0, 0, 0));
  for (std::size_t k = 0; k < jumps; ++k) {
    p.insns[to_pass[k]].off = static_cast<std::int16_t>(pass - (to_pass[k] + 1));
  }
  return std::vector<BpfInsn>(p.insns.begin(), p.insns.begin() + static_cast<std::ptrdiff_t>(p.n));
}

const char* to_string(FrameCheck c) noexcept {
  switch (c) {
    case FrameCheck::kOk: return "ok";
    case FrameCheck::kShort: return "short";
    case FrameCheck::kNotIpv4: return "not IPv4";
    case FrameCheck::kIpOptions: return "IP options";
    case FrameCheck::kFragment: return "fragment";
    case FrameCheck::kNotUdp: return "not UDP";
    case FrameCheck::kWrongDest: return "wrong destination";
    case FrameCheck::kBadChecksum: return "bad IPv4 header checksum";
    case FrameCheck::kBadLength: return "bad length";
  }
  return "?";
}

FrameCheck udp_payload(std::span<const std::uint8_t> f, std::uint32_t dst_ip_be, std::uint16_t dst_port,
                       std::span<const std::uint8_t>& payload) noexcept {
  payload = {};
  if (f.size() < kUdpFrameHeaderBytes) return FrameCheck::kShort;
  auto be16 = [&](std::size_t at) { return static_cast<std::uint16_t>(f[at] << 8 | f[at + 1]); };
  if (be16(12) != 0x0800 || (f[14] >> 4) != 4) return FrameCheck::kNotIpv4;
  if ((f[14] & 0x0F) != 5) return FrameCheck::kIpOptions;
  if ((be16(20) & 0x3FFF) != 0) return FrameCheck::kFragment;
  if (f[23] != 17) return FrameCheck::kNotUdp;
  std::uint32_t daddr;
  std::memcpy(&daddr, f.data() + 30, 4);
  if ((dst_ip_be != 0 && daddr != dst_ip_be) || be16(36) != dst_port) return FrameCheck::kWrongDest;
  // A correct header, checksum included, sums to zero.
  if (inet_checksum(f.subspan(14, 20)) != 0) return FrameCheck::kBadChecksum;
  const std::size_t ip_len = be16(16), udp_len = be16(38);
  // The frame may carry Ethernet padding past the IP datagram (short frames).
  if (ip_len < 28 || 14 + ip_len > f.size() || udp_len < 8 || 20 + udp_len != ip_len) return FrameCheck::kBadLength;
  payload = f.subspan(kUdpFrameHeaderBytes, udp_len - 8);
  return FrameCheck::kOk;
}

bool XdpReceiver::parse_attach(std::string_view s, Attach& out) noexcept {
  if (s == "auto") out = Attach::kAuto;
  else if (s == "driver" || s == "native") out = Attach::kDriver;
  else if (s == "generic" || s == "skb") out = Attach::kGeneric;
  else return false;
  return true;
}

const char* XdpReceiver::attach_name(Attach a) noexcept {
  switch (a) {
    case Attach::kAuto: return "auto";
    case Attach::kDriver: return "driver";
    case Attach::kGeneric: return "generic";
  }
  return "?";
}

}  // namespace adsb::feed

// -------------------------------------------------------------- AF_XDP

#if ADSB_HAVE_AF_XDP

#include <linux/bpf.h>
#include <linux/if_link.h>
#include <linux/if_xdp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <bit>
#include <chrono>
#include <thread>

#include "xsk_ring.hpp"

static_assert(std::endian::native == std::endian::little,
              "BpfInsn packs registers as on little-endian hosts");

namespace adsb::feed {

namespace {

// The kernel's headroom in front of each received frame (XDP_PACKET_HEADROOM).
constexpr std::size_t kXdpHeadroom = 256;

using xsk::load_acquire;
using xsk::Ring;
using xsk::set_error;
using xsk::store_release;

// The kernel rejects nonzero bytes past the fields a command uses.
void zero(bpf_attr& a) noexcept { std::memset(&a, 0, sizeof a); }

int sys_bpf(int cmd, bpf_attr& attr) noexcept {
  return static_cast<int>(::syscall(__NR_bpf, cmd, &attr, sizeof attr));
}

std::uint64_t ptr_u64(const void* p) noexcept { return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(p)); }

struct Fd {
  int fd = -1;
  Fd() = default;
  Fd(const Fd&) = delete;
  Fd& operator=(const Fd&) = delete;
  ~Fd() {
    if (fd >= 0) ::close(fd);
  }
};

}  // namespace

namespace {
struct Umem {
  void* p = MAP_FAILED;
  std::size_t len = 0;
  Umem() = default;
  Umem(const Umem&) = delete;
  Umem& operator=(const Umem&) = delete;
  ~Umem() {
    if (p != MAP_FAILED) ::munmap(p, len);
  }
};
}  // namespace

struct XdpReceiver::Impl {
  // Members are destroyed in reverse order: the link first (the program
  // detaches: no more redirects), then the program and map, the rings, the
  // socket, and the UMEM last.
  Umem umem;
  Fd xsk;
  Ring rx, fill;
  Fd map, prog, link;
  std::uint32_t rx_cons = 0;    // our RX consumer index
  std::uint32_t fill_prod = 0;  // our fill producer index
  std::uint32_t dst_ip_be = 0;
  std::uint16_t dst_port = 0;
  InterfaceInfo dev;
  Mode bound = Mode::kAuto;
  Attach attached = Attach::kAuto;
};

XdpReceiver::XdpReceiver() = default;
XdpReceiver::~XdpReceiver() = default;

bool XdpReceiver::built() noexcept { return true; }

int XdpReceiver::fd() const noexcept { return impl_ ? impl_->xsk.fd : -1; }

XdpReceiver::Mode XdpReceiver::bound_mode() const noexcept { return impl_ ? impl_->bound : Mode::kAuto; }

XdpReceiver::Attach XdpReceiver::attach_mode() const noexcept { return impl_ ? impl_->attached : Attach::kAuto; }

const InterfaceInfo& XdpReceiver::device() const noexcept {
  static const InterfaceInfo none;
  return impl_ ? impl_->dev : none;
}

bool XdpReceiver::open(const Endpoint& feed, const Options& opt, std::string* error) {
  impl_.reset();
  stats_ = {};
  auto m = std::make_unique<Impl>();
  const unsigned n = opt.frames;
  if (n < 64 || n > 65536 || (n & (n - 1)) != 0) {
    if (error) *error = "frames must be a power of two, 64..65536";
    return false;
  }
  sockaddr_in addr{};
  if (!resolve_ipv4(feed, addr, error)) return false;
  m->dst_ip_be = addr.sin_addr.s_addr;
  m->dst_port = feed.port;
  if (!interface_for_address(opt.interface_host, m->dev, error)) return false;
  if (m->dev.mtu + 14 > XdpSender::kFrameBytes - kXdpHeadroom) {
    if (error) {
      *error = m->dev.name + "'s MTU " + std::to_string(m->dev.mtu) + " does not fit a " +
               std::to_string(XdpSender::kFrameBytes) + "-byte UMEM frame after the kernel's headroom (max MTU " +
               std::to_string(XdpSender::kFrameBytes - kXdpHeadroom - 14) + ")";
    }
    return false;
  }

  // The socket and its UMEM; every frame starts on the fill ring.
  m->xsk.fd = ::socket(AF_XDP, SOCK_RAW, 0);
  if (m->xsk.fd < 0) {
    set_error(error, "socket(AF_XDP) (needs CAP_NET_RAW)", errno);
    return false;
  }
  m->umem.len = static_cast<std::size_t>(n) * XdpSender::kFrameBytes;
  m->umem.p = ::mmap(nullptr, m->umem.len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (m->umem.p == MAP_FAILED) {
    set_error(error, "mmap UMEM", errno);
    return false;
  }
  xdp_umem_reg reg{};
  reg.addr = ptr_u64(m->umem.p);
  reg.len = m->umem.len;
  reg.chunk_size = XdpSender::kFrameBytes;
  if (::setsockopt(m->xsk.fd, SOL_XDP, XDP_UMEM_REG, &reg, sizeof reg) != 0) {
    xsk::set_umem_error(error, m->umem.len, errno);
    return false;
  }
  // A completion ring is required even for an RX-only socket.
  const int ring = static_cast<int>(n), small = 64;
  if (::setsockopt(m->xsk.fd, SOL_XDP, XDP_UMEM_FILL_RING, &ring, sizeof ring) != 0 ||
      ::setsockopt(m->xsk.fd, SOL_XDP, XDP_UMEM_COMPLETION_RING, &small, sizeof small) != 0 ||
      ::setsockopt(m->xsk.fd, SOL_XDP, XDP_RX_RING, &ring, sizeof ring) != 0) {
    set_error(error, "XDP ring sizes", errno);
    return false;
  }
  xdp_mmap_offsets off{};
  socklen_t len = sizeof off;
  if (::getsockopt(m->xsk.fd, SOL_XDP, XDP_MMAP_OFFSETS, &off, &len) != 0) {
    set_error(error, "XDP_MMAP_OFFSETS", errno);
    return false;
  }
  if (!m->rx.mmap_ring(m->xsk.fd, off.rx, n, sizeof(xdp_desc), XDP_PGOFF_RX_RING) ||
      !m->fill.mmap_ring(m->xsk.fd, off.fr, n, sizeof(std::uint64_t),
                         static_cast<off_t>(XDP_UMEM_PGOFF_FILL_RING))) {
    set_error(error, "mmap XDP rings", errno);
    return false;
  }
  auto* fill = static_cast<std::uint64_t*>(m->fill.desc);
  for (unsigned i = 0; i < n; ++i) fill[i] = static_cast<std::uint64_t>(i) * XdpSender::kFrameBytes;
  m->fill_prod = n;
  store_release(m->fill.producer, n);

  sockaddr_xdp sx{};
  sx.sxdp_family = AF_XDP;
  sx.sxdp_ifindex = m->dev.index;
  sx.sxdp_queue_id = opt.queue;
  sx.sxdp_flags = XDP_USE_NEED_WAKEUP;
  if (opt.mode == Mode::kCopy) sx.sxdp_flags |= XDP_COPY;
  if (opt.mode == Mode::kZeroCopy) sx.sxdp_flags |= XDP_ZEROCOPY;
  // As on the publisher side: a closed socket's queue is released
  // asynchronously, so an immediate restart can see EBUSY for a moment.
  const auto bind_until = std::chrono::steady_clock::now() + std::chrono::seconds(1);
  while (::bind(m->xsk.fd, reinterpret_cast<const sockaddr*>(&sx), sizeof sx) != 0) {
    const int e = errno;
    if (e == EBUSY && std::chrono::steady_clock::now() < bind_until) {
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
      continue;
    }
    set_error(error, "bind AF_XDP to " + m->dev.name + " queue " + std::to_string(opt.queue) + " (" +
                         XdpSender::mode_name(opt.mode) + (e == EBUSY ? "; another socket holds this queue" : "") + ")",
              e);
    return false;
  }
  xdp_options xo{};
  len = sizeof xo;
  m->bound = ::getsockopt(m->xsk.fd, SOL_XDP, XDP_OPTIONS, &xo, &len) == 0 && (xo.flags & XDP_OPTIONS_ZEROCOPY)
                 ? Mode::kZeroCopy
                 : Mode::kCopy;

  // The map, with our socket at our queue, then the program.
  bpf_attr a;
  zero(a);
  a.map_type = BPF_MAP_TYPE_XSKMAP;
  a.key_size = 4;
  a.value_size = 4;
  a.max_entries = opt.queue + 1;
  m->map.fd = sys_bpf(BPF_MAP_CREATE, a);
  if (m->map.fd < 0) {
    set_error(error, "bpf(BPF_MAP_CREATE XSKMAP) (needs CAP_BPF and CAP_NET_ADMIN)", errno);
    return false;
  }
  const std::uint32_t key = opt.queue;
  const std::uint32_t value = static_cast<std::uint32_t>(m->xsk.fd);
  zero(a);
  a.map_fd = static_cast<std::uint32_t>(m->map.fd);
  a.key = ptr_u64(&key);
  a.value = ptr_u64(&value);
  if (sys_bpf(BPF_MAP_UPDATE_ELEM, a) != 0) {
    set_error(error, "bpf(BPF_MAP_UPDATE_ELEM)", errno);
    return false;
  }
  const std::vector<BpfInsn> code = xdp_feed_program(m->map.fd, m->dst_ip_be, m->dst_port);
  static const char kLicense[] = "Dual MIT/GPL";
  std::vector<char> log(64 * 1024);
  auto load = [&](bool with_log) {
    zero(a);
    a.prog_type = BPF_PROG_TYPE_XDP;
    a.insn_cnt = static_cast<std::uint32_t>(code.size());
    a.insns = ptr_u64(code.data());
    a.license = ptr_u64(kLicense);
    a.expected_attach_type = BPF_XDP;
    if (with_log) {
      a.log_level = 1;
      a.log_buf = ptr_u64(log.data());
      a.log_size = static_cast<std::uint32_t>(log.size());
    }
    return sys_bpf(BPF_PROG_LOAD, a);
  };
  m->prog.fd = load(false);
  if (m->prog.fd < 0) {
    const int e = errno;
    std::string why = "bpf(BPF_PROG_LOAD)";
    if (e == EPERM) why += " (needs CAP_BPF and CAP_NET_ADMIN)";
    if (e == EINVAL || e == EACCES) {
      m->prog.fd = load(true);  // run the verifier again for its explanation
      if (m->prog.fd < 0 && log[0] != '\0') {
        why += " verifier: ";
        why += log.data();
      }
    }
    if (m->prog.fd < 0) {  // (a retry that passed is kept)
      set_error(error, why, e);
      return false;
    }
  }

  // Attach through a link: it detaches when closed, even if we crash.
  auto attach = [&](std::uint32_t flags) {
    zero(a);
    a.link_create.prog_fd = static_cast<std::uint32_t>(m->prog.fd);
    a.link_create.target_ifindex = m->dev.index;
    a.link_create.attach_type = BPF_XDP;
    a.link_create.flags = flags;
    return sys_bpf(BPF_LINK_CREATE, a);
  };
  int e = 0;
  if (opt.attach != Attach::kGeneric) {
    m->link.fd = attach(XDP_FLAGS_DRV_MODE);
    if (m->link.fd >= 0) m->attached = Attach::kDriver;
    else e = errno;
  }
  if (m->link.fd < 0 && opt.attach != Attach::kDriver) {
    m->link.fd = attach(XDP_FLAGS_SKB_MODE);
    if (m->link.fd >= 0) m->attached = Attach::kGeneric;
    else e = errno;
  }
  if (m->link.fd < 0) {
    std::string why = "attach XDP program to " + m->dev.name + " (" + attach_name(opt.attach) + ")";
    if (e == EBUSY || e == EEXIST) why += ": another XDP program is attached to this device";
    set_error(error, why, e);
    return false;
  }
  impl_ = std::move(m);
  return true;
}

std::size_t XdpReceiver::receive(Callback cb, void* ctx, std::size_t max) noexcept {
  if (!impl_) return 0;
  Impl& m = *impl_;
  const std::uint32_t prod = load_acquire(m.rx.producer);
  std::uint32_t avail = prod - m.rx_cons;
  if (avail > max) avail = static_cast<std::uint32_t>(max);
  if (avail != 0) {
    const auto* descs = static_cast<const xdp_desc*>(m.rx.desc);
    auto* fill = static_cast<std::uint64_t*>(m.fill.desc);
    const auto* base = static_cast<const std::uint8_t*>(m.umem.p);
    for (std::uint32_t i = 0; i < avail; ++i) {
      const xdp_desc& d = descs[(m.rx_cons + i) & m.rx.mask];
      std::span<const std::uint8_t> payload;
      if (d.addr + d.len <= m.umem.len &&
          udp_payload({base + d.addr, d.len}, m.dst_ip_be, m.dst_port, payload) == FrameCheck::kOk) {
        ++stats_.received;
        cb(ctx, payload);
      } else {
        ++stats_.rejected;
      }
      // Back to the fill ring (the frame's start: the kernel's address
      // includes the headroom). The fill ring holds every frame, so there
      // is always room for the ones we return.
      fill[m.fill_prod & m.fill.mask] = d.addr & ~static_cast<std::uint64_t>(XdpSender::kFrameBytes - 1);
      ++m.fill_prod;
    }
    m.rx_cons += avail;
    store_release(m.rx.consumer, m.rx_cons);
    store_release(m.fill.producer, m.fill_prod);
  }
  if (__atomic_load_n(m.fill.flags, __ATOMIC_RELAXED) & XDP_RING_NEED_WAKEUP) {
    ++stats_.wakeups;
    (void)::recvfrom(m.xsk.fd, nullptr, 0, MSG_DONTWAIT, nullptr, nullptr);
  }
  return avail;
}

void XdpReceiver::refresh_kernel_stats() noexcept {
  if (!impl_) return;
  xdp_statistics st{};
  socklen_t len = sizeof st;
  if (::getsockopt(impl_->xsk.fd, SOL_XDP, XDP_STATISTICS, &st, &len) == 0) {
    stats_.rx_dropped = st.rx_dropped;
    stats_.rx_invalid = st.rx_invalid_descs;
    stats_.rx_ring_full = st.rx_ring_full;
    stats_.rx_fill_ring_empty = st.rx_fill_ring_empty_descs;
  }
}

}  // namespace adsb::feed

#else  // !ADSB_HAVE_AF_XDP

namespace adsb::feed {

struct XdpReceiver::Impl {};

XdpReceiver::XdpReceiver() = default;
XdpReceiver::~XdpReceiver() = default;
bool XdpReceiver::built() noexcept { return false; }
bool XdpReceiver::open(const Endpoint&, const Options&, std::string* error) {
  if (error) *error = "AF_XDP is not available in this build (Linux with kernel 5.9+ headers only)";
  return false;
}
int XdpReceiver::fd() const noexcept { return -1; }
std::size_t XdpReceiver::receive(Callback, void*, std::size_t) noexcept { return 0; }
void XdpReceiver::refresh_kernel_stats() noexcept {}
XdpReceiver::Mode XdpReceiver::bound_mode() const noexcept { return Mode::kAuto; }
XdpReceiver::Attach XdpReceiver::attach_mode() const noexcept { return Attach::kAuto; }
const InterfaceInfo& XdpReceiver::device() const noexcept {
  static const InterfaceInfo none;
  return none;
}

}  // namespace adsb::feed

#endif
