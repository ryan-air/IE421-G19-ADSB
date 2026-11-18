#include "adsb/feed/shm_subscriber.hpp"

#include <unistd.h>

#include "adsb/common/cache.hpp"
#include "adsb/common/clock.hpp"

namespace adsb::feed {

ShmSubscriber::ShmSubscriber(const ShmSubscriberConfig& cfg, SubscriberHandler* handler)
    : cfg_(cfg), handler_(handler) {}

bool ShmSubscriber::open(std::string* error) {
  // Open into a fresh region and switch only on success: a failed reopen
  // (the ring was replaced again, or unlinked) must not drop the ring we have.
  std::string err;
  ShmRegion fresh;
  if (!fresh.open(cfg_.name, &err)) {
    error_ = err;
    if (error) *error = err;
    if (!ring_.valid()) state_ = State::kNoRing;
    return false;
  }
  region_ = std::move(fresh);
  ring_ = region_.view();
  instance_ = ring_.instance();
  const protocol::Session s = ring_.session();
  if (stats_.rings_opened > 0 || s != session_) reset_book();
  session_ = s;
  ++stats_.rings_opened;
  state_ = State::kNeedSync;
  tried_snapshot_ = false;
  ended_unsynced_since_ns_ = 0;
  return true;
}

void ShmSubscriber::reset_book() {
  book_.clear();
  expected_ = 0;
  if (handler_) handler_->on_reset();
}

bool ShmSubscriber::ended() const noexcept {
  if (state_ != State::kLive) return false;
  const std::uint64_t e = ring_.ended();
  return e != 0 && expected_ >= e;
}

void ShmSubscriber::sync(std::uint64_t now_ns) {
  // The whole session is still in the ring: read it from the start (the
  // state before the first message is empty), no snapshot needed.
  if (expected_ == 0 && ring_.oldest() == ring_.first_seq()) {
    book_.clear();
    expected_ = ring_.first_seq();
    state_ = State::kLive;
    live_since_ns_ = now_ns;
    return;
  }
  // Lapped after the end of the session: a snapshot is the only way to
  // finish, and once the publisher is gone none will come. Give up (and say
  // so) instead of retrying forever.
  if (ring_.ended() != 0) {
    if (ended_unsynced_since_ns_ == 0) ended_unsynced_since_ns_ = now_ns;
    if (now_ns - ended_unsynced_since_ns_ >= cfg_.give_up_after_end_ns) {
      state_ = State::kGaveUp;
      error_ = "lapped after the end of the session and no snapshot was available";
      return;
    }
  }
  if (tried_snapshot_ && now_ns - last_snapshot_try_ns_ < cfg_.snapshot_retry_ns) return;
  tried_snapshot_ = true;
  last_snapshot_try_ns_ = now_ns;
  SnapshotData snap;
  std::string err;
  if (!SubscriberClient::fetch_snapshot(cfg_.snapshot, cfg_.snapshot_timeout_ms, snap, &err)) {
    error_ = err;
    ++stats_.snapshot_failures;
    return;
  }
  // The snapshot server belongs to whichever publisher is running now. If it
  // replaced our ring (even under the same session id, which the spec forbids
  // but nothing enforces), its snapshot must not be paired with our ring's
  // messages: switch to the new ring and join it instead.
  if (ShmRegion::current_instance(cfg_.name) != instance_) {
    error_ = "the ring was replaced while fetching a snapshot";
    ++stats_.snapshot_failures;
    std::string e;
    (void)open(&e);
    return;
  }
  // Usable only for this ring's session, and only if the ring still holds the
  // message after it. (A snapshot can be newer than the ring: the publisher
  // logs a message for recovery before it sends a batch. Then we just wait.)
  if (snap.session != session_ || snap.as_of_seq + 1 < ring_.oldest()) {
    error_ = snap.session != session_ ? "snapshot is from another session" : "snapshot too old for the ring";
    ++stats_.snapshot_failures;
    return;
  }
  book_.clear();
  for (const protocol::TrackUpdate& u : snap.tracks) book_.put(u);
  expected_ = snap.as_of_seq + 1;
  ++stats_.snapshots_loaded;
  state_ = State::kLive;
  live_since_ns_ = now_ns;
  tried_snapshot_ = false;
  if (handler_) handler_->on_snapshot_loaded();
}

void ShmSubscriber::check_replaced(std::uint64_t now_ns) {
  if (now_ns - last_replace_check_ns_ < cfg_.replace_check_ns) return;
  last_replace_check_ns_ = now_ns;
  const std::uint64_t id = ShmRegion::current_instance(cfg_.name);
  if (id != 0 && id != instance_) {
    std::string err;
    (void)open(&err);  // a new session: reset, then join it
  }
}

std::size_t ShmSubscriber::poll(std::uint64_t now_ns, std::size_t max) {
  if (state_ == State::kNoRing) {
    if (last_open_try_ns_ != 0 && now_ns - last_open_try_ns_ < cfg_.reopen_retry_ns) return 0;
    last_open_try_ns_ = now_ns;
    if (!open()) return 0;
  }
  if (state_ == State::kGaveUp) {
    check_replaced(now_ns);  // a new publisher brings a new ring
    return 0;
  }
  if (state_ == State::kNeedSync) {
    sync(now_ns);
    if (state_ != State::kLive) {
      check_replaced(now_ns);
      return 0;
    }
  }
  std::size_t n = 0;
  std::uint8_t msg[kShmMaxMessage];
  while (n < max) {
    std::size_t len = 0;
    const RingView::Read r = ring_.read(expected_, msg, len);
    if (r == RingView::Read::kEmpty) break;
    if (r == RingView::Read::kLapped) {
      ++stats_.laps;
      state_ = State::kNeedSync;
      tried_snapshot_ = false;
      break;
    }
    const ApplyResult ar = book_.apply(std::span<const std::uint8_t>(msg, len));
    if (ar == ApplyResult::kUnknownType || ar == ApplyResult::kMalformed) ++stats_.unknown_messages;
    ++stats_.applied;
    if (handler_) handler_->on_message(expected_, std::span<const std::uint8_t>(msg, len));
    ++expected_;
    ++n;
  }
  if (n == 0) check_replaced(now_ns);
  return n;
}

void ShmSubscriber::run(const std::atomic<bool>& stop, bool stop_at_end, unsigned idle_sleep_us) {
  std::uint64_t last_tick = 0;
  while (!stop.load(std::memory_order_relaxed)) {
    if (stop_at_end && (ended() || gave_up())) break;
    const std::uint64_t now = mono_ns();
    const std::size_t n = poll(now);
    if (handler_ && now - last_tick >= 1'000'000) {
      last_tick = now;
      handler_->on_tick(now);
    }
    if (n == 0) {
      if (idle_sleep_us) ::usleep(idle_sleep_us);
      else cpu_relax();
    }
  }
}

}  // namespace adsb::feed
