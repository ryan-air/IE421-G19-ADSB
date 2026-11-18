#pragma once

// ConflatedFeed: everything the conflated feed (README_UPDATES.md §10.2)
// needs on the publishing side, as one component for the tools: a UDP sender
// on its own multicast group, its own sequenced Publisher with its own session
// id, its own retransmit and snapshot servers (a recovery thread), and the
// Conflator in front of it. To subscribers it is an ordinary feed: the same
// messages, framing and recovery, so adsb-feed-cli and the gateway subscribe
// to it unchanged.
//
// Use it as a second track-engine sink beside the main publisher, and call
// poll(now_ns) often (after every frame and while idle): it ticks the
// conflator at the configured rate and lets the publisher send heartbeats.

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>

#include "adsb/feed/conflate.hpp"
#include "adsb/feed/net.hpp"
#include "adsb/feed/publisher.hpp"
#include "adsb/feed/recovery.hpp"

namespace adsb::feed {

struct ConflatedFeedConfig {
  Endpoint group{"239.19.0.3", 41910};
  UdpSender::Options send;
  std::string bind_host = "0.0.0.0";      // recovery servers
  std::uint16_t retransmit_port = 41911;
  std::uint16_t snapshot_port = 41912;
  double hz = 10.0;
  std::size_t capacity = 16384;           // aircraft the conflator holds
  protocol::Session session{};            // zero: a unique one ('C' prefix)
  std::uint64_t heartbeat_interval_ns = 1'000'000'000;
  bool stamp_publish_ts = true;
};

class ConflatedFeed {
 public:
  using Pub = Publisher<UdpSender>;

  ConflatedFeed() = default;
  ~ConflatedFeed() { stop(); }
  ConflatedFeed(const ConflatedFeed&) = delete;
  ConflatedFeed& operator=(const ConflatedFeed&) = delete;

  [[nodiscard]] bool open(const ConflatedFeedConfig& cfg, std::string* error) {
    cfg_ = cfg;
    if (!(cfg_.hz >= 0.001 && cfg_.hz <= 1e6)) {  // interval between 1 µs and 1000 s
      if (error) *error = "conflated feed rate must be between 0.001 and 1000000 Hz";
      return false;
    }
    if (cfg_.session == protocol::Session{}) cfg_.session = unique_session('C');
    udp_ = std::make_unique<UdpSender>();
    if (!udp_->open(cfg_.group, cfg_.send, error)) return false;
    log_ = make_message_log();
    RecoveryServerConfig rc;
    rc.recovery.session = cfg_.session;
    rc.bind_host = cfg_.bind_host;
    rc.retransmit_port = cfg_.retransmit_port;
    rc.snapshot_port = cfg_.snapshot_port;
    recovery_ = std::make_unique<RecoveryServer>(rc, *log_);
    if (!recovery_->open(error)) return false;
    PublisherConfig pc;
    pc.session = cfg_.session;
    pc.heartbeat_interval_ns = cfg_.heartbeat_interval_ns;
    pc.stamp_publish_ts = cfg_.stamp_publish_ts;
    // A tick produces its updates all at once: pack them into full datagrams
    // (up to 22 messages) and flush when the tick is done, rather than one
    // datagram per message.
    pc.max_batch_delay_ns = ~std::uint64_t{0} >> 1;
    pub_ = std::make_unique<Pub>(pc, *udp_, log_.get());
    ConflatorConfig cc;
    cc.interval_ns = static_cast<std::uint64_t>(1e9 / cfg_.hz);
    cc.capacity = cfg_.capacity;
    conflator_ = std::make_unique<Conflator<Pub>>(cc, *pub_);
    rec_thread_ = std::thread([this] { recovery_->run(rec_stop_); });
    return true;
  }

  // ---- Track-engine sink --------------------------------------------------
  void on_update(const protocol::TrackUpdate& u) { conflator_->on_update(u); }
  void on_delete(const protocol::TrackDelete& d) { conflator_->on_delete(d); }

  // Ticks the conflator when due, sends anything else pending at once (a
  // tick's batch, or updates that passed through a full table), and lets the
  // publisher send heartbeats while quiet.
  void poll(std::uint64_t now_ns) {
    conflator_->poll(now_ns);
    pub_->flush();
    pub_->poll();
  }

  // Heartbeats and pending sends only, without touching the tick clock (for a
  // replay waiting between records: ticks stay on recording time).
  void poll_publisher() {
    pub_->flush();
    pub_->poll();
  }

  // Sends what is pending and ends the session (the recovery servers keep
  // answering until stop()).
  void end_session() {
    conflator_->tick();
    pub_->end_session();
  }
  // Repeats the end-of-session marker while lingering (see Publisher::poll).
  void poll_ended() { pub_->poll(); }

  void stop() {
    rec_stop_ = true;
    if (rec_thread_.joinable()) rec_thread_.join();
  }

  [[nodiscard]] const ConflatorStats& conflator_stats() const { return conflator_->stats(); }
  [[nodiscard]] const PublisherStats& publisher_stats() const { return pub_->stats(); }
  [[nodiscard]] const RecoveryServer& recovery() const { return *recovery_; }
  [[nodiscard]] const ConflatedFeedConfig& config() const { return cfg_; }
  [[nodiscard]] std::string session_text() const { return std::string(cfg_.session.begin(), cfg_.session.end()); }

 private:
  ConflatedFeedConfig cfg_;
  std::unique_ptr<UdpSender> udp_;
  std::unique_ptr<MessageLog> log_;
  std::unique_ptr<RecoveryServer> recovery_;
  std::unique_ptr<Pub> pub_;
  std::unique_ptr<Conflator<Pub>> conflator_;
  std::atomic<bool> rec_stop_{false};
  std::thread rec_thread_;
};

}  // namespace adsb::feed
