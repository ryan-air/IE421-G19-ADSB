#pragma once

// Shared-memory broadcast ring: the same-host feed transport
// (README_UPDATES.md §9.3, §10.1; protocol/feed_spec.md).
//
// One producer, any number of consumers, each with its own cursor. The
// producer never waits: it overwrites the oldest slot. A consumer that falls a
// full lap behind detects it from the sequence numbers and recovers from a
// snapshot (ShmSubscriber does that through the TCP snapshot server).
//
// Layout of the region (all integers little-endian; every field the other
// side reads concurrently is a lock-free 64-bit atomic):
//
//   offset 0     ShmHeader (one 4 KiB page): magic "ADSBSHM1", version,
//                slot size, capacity, session, instance id, first_seq,
//                next_seq (atomic, own cache line), ended (atomic, own line)
//   offset 4096  ShmSlot[capacity], 128 bytes each:
//                  seq      atomic u64  0 = never written; kBusy | s while
//                                       message s is being written; s once it is
//                  len      atomic u64  message length (low 16 bits)
//                  words[9] atomic u64  the message bytes (up to 72)
//
// Slot protocol: a seqlock per slot. The payload is atomic too, so it is
// data-race free under the C++ memory model:
//
//   writer, message s into slot s & mask:
//     seq.store(kBusy | s, relaxed); fence(release)
//     len / words .store(..., relaxed)
//     seq.store(s, release); next_seq.store(s + 1, release)
//
//   reader, expecting message s:
//     v1 = seq.load(acquire)
//     v1 == s:   copy len / words (relaxed); fence(acquire); v2 = seq.load(relaxed)
//                v2 == s ? the copy is intact : the writer lapped us mid-copy
//     v1 < s, 0, or kBusy | s:  not written yet
//     anything later (s + k * capacity, busy or not):  lapped
//
// A torn copy is never returned. If every word read came from message s, the
// acquire of v1 ordered them after the writer's stores. If any word came from
// a later lap, the writer's release fence and the reader's acquire fence make
// the busy marker for that lap happen before v2, so v2 cannot read s and the
// reader reports a lap.
//
// ThreadSanitizer does not model standalone fences (GCC warns, which -Werror
// turns into a build failure), so TSan builds compile the equivalent
// fence-free form: every payload store release, every payload load acquire.
// That form is about 3 ns slower per operation on AArch64 (free on x86). The
// fence form is exercised by the concurrent torn-read test in every other
// build, on ARM and x86 hardware. The ring core works on any memory (RingView), so the unit
// tests run writer and reader on one heap region under ThreadSanitizer, and a
// model checker can run the same code.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>

#include "adsb/common/cache.hpp"
#include "adsb/protocol/moldudp64.hpp"

namespace adsb::feed {

// "ADSBSHM1" read as a little-endian u64 (the header stores it atomically).
inline constexpr std::uint64_t kShmMagic = 0x314D485342534441ULL;
inline constexpr std::uint32_t kShmVersion = 2;  // 2: 72-byte payload (feed v1)
inline constexpr std::size_t kShmHeaderBytes = 4096;
inline constexpr std::size_t kShmSlotBytes = 128;
inline constexpr std::size_t kShmMaxMessage = 72;  // = protocol::TrackUpdate::kWireSize (checked in shm_ring.cpp)
inline constexpr std::uint64_t kShmBusy = 1ULL << 63;
inline constexpr std::uint64_t kShmDefaultSlots = 1ULL << 16;  // 8 MiB of slots
inline constexpr std::uint64_t kShmMaxSlots = 1ULL << 24;      // 2 GiB

#if defined(__SANITIZE_THREAD__)
#define ADSB_SHM_TSAN 1
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define ADSB_SHM_TSAN 1
#endif
#endif
#ifndef ADSB_SHM_TSAN
#define ADSB_SHM_TSAN 0
#endif

static_assert(std::atomic<std::uint64_t>::is_always_lock_free,
              "the shared-memory ring needs lock-free (address-free) 64-bit atomics");

struct alignas(kCacheLine) ShmSlot {
  std::atomic<std::uint64_t> seq{0};
  std::atomic<std::uint64_t> len{0};
  std::atomic<std::uint64_t> words[kShmMaxMessage / 8] = {};
};
static_assert(sizeof(ShmSlot) == kShmSlotBytes);

struct ShmHeader {
  std::atomic<std::uint64_t> magic;  // kShmMagic once formatted (stored last, with release)
  std::uint32_t version;
  std::uint32_t slot_bytes;
  std::uint64_t capacity;  // slots, a power of two
  char session[protocol::kSessionLength];
  char reserved0[6];
  std::uint64_t instance;  // random per created region: tells a replacement apart
  std::uint64_t first_seq; // the session's first sequence number
  std::int64_t writer_pid;
  alignas(kCacheLine) std::atomic<std::uint64_t> next_seq;  // one past the last complete message
  alignas(kCacheLine) std::atomic<std::uint64_t> ended;     // 0 = live; else the end-of-session seq
};
static_assert(sizeof(ShmHeader) <= kShmHeaderBytes);

[[nodiscard]] inline std::size_t shm_region_bytes(std::uint64_t capacity) noexcept {
  return kShmHeaderBytes + static_cast<std::size_t>(capacity) * kShmSlotBytes;
}

// The ring over a region of memory: a header followed by `capacity` slots.
// Works on a mapped shared-memory object or on ordinary memory (tests).
class RingView {
 public:
  RingView() = default;
  RingView(void* base, std::uint64_t capacity) noexcept
      : header_(static_cast<ShmHeader*>(base)),
        slots_(reinterpret_cast<ShmSlot*>(static_cast<std::uint8_t*>(base) + kShmHeaderBytes)),
        mask_(capacity - 1) {}

  // Writer: formats an empty ring. Call before any reader looks at it; the
  // magic is written last, so a reader that opens early sees an invalid ring.
  // first_seq must be at least 1: seq 0 marks a slot that was never written.
  void format(const protocol::Session& session, std::uint64_t first_seq, std::uint64_t instance,
              std::int64_t pid) noexcept {
    new (&header_->magic) std::atomic<std::uint64_t>(0);
    header_->version = kShmVersion;
    header_->slot_bytes = kShmSlotBytes;
    header_->capacity = mask_ + 1;
    std::memcpy(header_->session, session.data(), session.size());
    std::memset(header_->reserved0, 0, sizeof header_->reserved0);
    header_->instance = instance;
    header_->first_seq = first_seq;
    header_->writer_pid = pid;
    new (&header_->next_seq) std::atomic<std::uint64_t>(first_seq);
    new (&header_->ended) std::atomic<std::uint64_t>(0);
    for (std::uint64_t i = 0; i <= mask_; ++i) new (&slots_[i]) ShmSlot();  // also pre-faults
    header_->magic.store(kShmMagic, std::memory_order_release);
  }

  // ---- Writer (one thread) -------------------------------------------------

  // Publishes message `seq` (callers number messages consecutively). Never waits.
  void write(std::uint64_t seq, const std::uint8_t* msg, std::size_t len) noexcept {
    ShmSlot& s = slots_[seq & mask_];
    s.seq.store(kShmBusy | seq, std::memory_order_relaxed);
    std::uint64_t w[kShmMaxMessage / 8] = {};
    std::memcpy(w, msg, len < kShmMaxMessage ? len : kShmMaxMessage);
#if ADSB_SHM_TSAN
    constexpr auto kOrder = std::memory_order_release;
#else
    std::atomic_thread_fence(std::memory_order_release);
    constexpr auto kOrder = std::memory_order_relaxed;
#endif
    s.len.store(len, kOrder);
    for (std::size_t i = 0; i < kShmMaxMessage / 8; ++i) s.words[i].store(w[i], kOrder);
    s.seq.store(seq, std::memory_order_release);
    header_->next_seq.store(seq + 1, std::memory_order_release);
  }

  // End of session: messages before `next_seq` are all there is.
  void end(std::uint64_t next_seq) noexcept { header_->ended.store(next_seq, std::memory_order_release); }

  // ---- Readers (any number, any thread or process) -------------------------

  enum class Read : std::uint8_t { kOk, kEmpty, kLapped };

  // Reads message `seq` into `out` (kShmMaxMessage bytes: all of them are
  // written, whatever the length) and its length into `len`.
  [[nodiscard]] Read read(std::uint64_t seq, std::uint8_t* out, std::size_t& len) const noexcept {
    const ShmSlot& s = slots_[seq & mask_];
    const std::uint64_t v1 = s.seq.load(std::memory_order_acquire);
    if (v1 != seq) {
      const std::uint64_t v = v1 & ~kShmBusy;
      return v1 == 0 || v <= seq ? Read::kEmpty : Read::kLapped;  // (kBusy | seq): being written
    }
    std::uint64_t w[kShmMaxMessage / 8];
#if ADSB_SHM_TSAN
    constexpr auto kOrder = std::memory_order_acquire;
#else
    constexpr auto kOrder = std::memory_order_relaxed;
#endif
    const std::uint64_t n = s.len.load(kOrder);
    for (std::size_t i = 0; i < kShmMaxMessage / 8; ++i) w[i] = s.words[i].load(kOrder);
#if !ADSB_SHM_TSAN
    std::atomic_thread_fence(std::memory_order_acquire);
#endif
    if (s.seq.load(std::memory_order_relaxed) != seq) return Read::kLapped;
    len = static_cast<std::size_t>(n & 0xFFFF);
    if (len > kShmMaxMessage) len = kShmMaxMessage;
    std::memcpy(out, w, kShmMaxMessage);
    return Read::kOk;
  }

  [[nodiscard]] std::uint64_t next_seq() const noexcept {
    return header_->next_seq.load(std::memory_order_acquire);
  }
  // Where a reader resumes after read(seq) reported kLapped. next_seq() alone
  // can lag behind the lap the reader just saw, even below seq: a lap seen
  // through the writer's busy marker (or the re-check after the copy) does not
  // synchronize with the writer's next_seq store. The GenMC model
  // (spec/memory-model/shm_slot.c) found this. Message seq is gone in either
  // case, so never move backwards: at least seq + 1.
  [[nodiscard]] std::uint64_t resume_after_lap(std::uint64_t seq) const noexcept {
    const std::uint64_t n = next_seq();
    return n > seq ? n : seq + 1;
  }
  [[nodiscard]] std::uint64_t ended() const noexcept { return header_->ended.load(std::memory_order_acquire); }
  // Oldest message that can still be in the ring (it may already be overwritten
  // by the time it is read; read() reports that).
  [[nodiscard]] std::uint64_t oldest() const noexcept {
    const std::uint64_t n = next_seq();
    const std::uint64_t first = header_->first_seq;
    return n - first > mask_ + 1 ? n - (mask_ + 1) : first;
  }
  [[nodiscard]] std::uint64_t capacity() const noexcept { return mask_ + 1; }
  [[nodiscard]] std::uint64_t first_seq() const noexcept { return header_->first_seq; }
  [[nodiscard]] std::uint64_t instance() const noexcept { return header_->instance; }
  [[nodiscard]] protocol::Session session() const noexcept {
    protocol::Session s{};
    std::memcpy(s.data(), header_->session, s.size());
    return s;
  }
  [[nodiscard]] bool valid() const noexcept { return header_ != nullptr; }

 private:
  ShmHeader* header_ = nullptr;
  ShmSlot* slots_ = nullptr;
  std::uint64_t mask_ = 0;
};

// A POSIX shared-memory object mapped into this process.
class ShmRegion {
 public:
  ShmRegion() = default;
  ~ShmRegion();
  ShmRegion(const ShmRegion&) = delete;
  ShmRegion& operator=(const ShmRegion&) = delete;
  ShmRegion(ShmRegion&& o) noexcept { *this = std::move(o); }
  ShmRegion& operator=(ShmRegion&& o) noexcept;

  // Writer: replaces any existing object of that name (a previous session;
  // readers that still map it keep their mapping), maps it read-write and
  // formats an empty ring for `session` starting at `first_seq`.
  // Names follow shm_open rules ("/adsb-feed"; at most 31 bytes on macOS).
  [[nodiscard]] bool create(const std::string& name, std::uint64_t capacity,
                            const protocol::Session& session, std::uint64_t first_seq, std::string* error);
  // Reader: maps an existing ring read-only and validates its header.
  [[nodiscard]] bool open(const std::string& name, std::string* error);
  // Removes the name (the writer does this when it is destroyed).
  static void unlink(const std::string& name) noexcept;
  // The instance id of the ring currently behind `name`, or 0 if none.
  [[nodiscard]] static std::uint64_t current_instance(const std::string& name) noexcept;

  [[nodiscard]] RingView view() const noexcept { return view_; }
  [[nodiscard]] bool huge_pages() const noexcept { return huge_; }
  [[nodiscard]] const std::string& name() const noexcept { return name_; }
  void close() noexcept;

 private:
  void* base_ = nullptr;
  std::size_t bytes_ = 0;
  bool owner_ = false;  // created it: unlink on destruction
  bool huge_ = false;
  std::string name_;
  RingView view_;
};

// Publisher `Out` for the shared-memory ring: takes the publisher's datagrams
// and writes each message into the ring under its own sequence number, so the
// publisher's sequencing, batching and recovery log are shared with UDP.
class ShmOut {
 public:
  explicit ShmOut(RingView ring) noexcept : ring_(ring) {}
  bool send(std::span<const std::uint8_t> dgram) noexcept {
    const auto pv = protocol::parse_packet(dgram);
    if (!pv) return false;
    switch (pv->kind) {
      case protocol::PacketKind::kData:
        pv->for_each([&](std::uint64_t seq, std::span<const std::uint8_t> msg) {
          ring_.write(seq, msg.data(), msg.size());
        });
        return true;
      case protocol::PacketKind::kEndOfSession:
        ring_.end(pv->header.seq);
        return true;
      case protocol::PacketKind::kHeartbeat:
        return true;  // readers see next_seq directly
    }
    return true;
  }

 private:
  RingView ring_;
};

// Sends every datagram to two `Out`s, first `a` then `b`; either may be null.
// Put the cheaper one first (the shm ring before UDP): the second waits for
// the first.
template <typename A, typename B>
class DualOut {
 public:
  DualOut(A* a, B* b) noexcept : a_(a), b_(b) {}
  bool send(std::span<const std::uint8_t> dgram) noexcept {
    bool ok = true;
    if (a_) ok = a_->send(dgram) && ok;
    if (b_) ok = b_->send(dgram) && ok;
    return ok;
  }

 private:
  A* a_;
  B* b_;
};

}  // namespace adsb::feed
