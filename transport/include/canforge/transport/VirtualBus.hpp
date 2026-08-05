// SPDX-License-Identifier: MIT
#ifndef CANFORGE_TRANSPORT_VIRTUALBUS_HPP
#define CANFORGE_TRANSPORT_VIRTUALBUS_HPP

/// A fully in-process CAN bus.
///
/// The point of this backend is that the whole test suite runs on any
/// operating system with no kernel modules and no root, while still behaving
/// like a CAN bus rather than like a message queue. Specifically it models:
///
///   * Bit timing. A frame occupies the medium for a realistic number of
///     microseconds derived from the bitrate, the identifier format, the
///     payload length and the worst-case stuff bits (see `frame_timing`).
///     Nothing is delivered before that time has passed.
///
///   * Arbitration. When several participants want to transmit at the same
///     moment, exactly one wins: the frame with the numerically lowest
///     identifier, comparing the base identifier first and letting a standard
///     frame beat an extended one with the same base, because the IDE bit is
///     dominant. The losers do not lose their frame -- they keep it at the
///     head of their queue and retry at the next arbitration point, which is
///     what a real controller does and what makes priority inversion and
///     starvation observable.
///
/// Time is virtual and advanced explicitly, so tests are deterministic and
/// take no wall-clock time. `receive()` advances the clock by up to its
/// timeout, which makes the backend a drop-in for a real one in single
/// threaded code without any test having to know about the clock.

#include <chrono>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <vector>

#include "canforge/transport/IBus.hpp"

namespace canforge::transport {

class VirtualBus;

/// Records who won and who lost an arbitration, for tests and for the
/// dashboard's bus-health view.
struct ArbitrationEvent {
  std::uint64_t at_ns = 0;
  CanId winner;
  std::string winner_name;
  std::vector<CanId> losers;
};

/// The shared medium. Participants attach to it; it owns the clock.
class VirtualMedium : public std::enable_shared_from_this<VirtualMedium> {
 public:
  static std::shared_ptr<VirtualMedium> create(BusTiming timing = {});

  /// Attach a new participant. Ownership is the caller's; the medium keeps a
  /// non-owning reference and drops it when the participant is destroyed.
  std::unique_ptr<VirtualBus> attach(std::string name);

  std::uint64_t now_ns() const noexcept { return now_ns_; }
  BusTiming timing() const noexcept { return timing_; }

  /// Move the virtual clock forward, transmitting whatever fits.
  void advance(std::chrono::nanoseconds by);

  /// Run until every queued frame has been transmitted. Returns the time that
  /// took. Guards against a participant that queues frames from a callback.
  std::chrono::nanoseconds drain(std::size_t max_frames = 100000);

  std::size_t pending() const noexcept;

  const std::vector<ArbitrationEvent>& arbitration_log() const noexcept {
    return arbitration_log_;
  }
  void clear_arbitration_log() { arbitration_log_.clear(); }

  /// SocketCAN delivers a transmitted frame back to the sending socket by
  /// default. Turn this off to model a controller with local loopback
  /// disabled.
  void set_loopback(bool on) noexcept { loopback_ = on; }
  bool loopback() const noexcept { return loopback_; }

  /// How many frames a participant's receive queue holds before it starts
  /// dropping, which is how a real overrun is produced on demand.
  void set_receive_queue_limit(std::size_t n) noexcept { rx_limit_ = n; }
  std::size_t receive_queue_limit() const noexcept { return rx_limit_; }

  /// Depth of a participant's transmit queue. Separate from the receive limit:
  /// a controller's mailbox count has nothing to do with its FIFO depth, and
  /// conflating the two makes an overrun test unable to queue enough frames.
  void set_transmit_queue_limit(std::size_t n) noexcept { tx_limit_ = n; }
  std::size_t transmit_queue_limit() const noexcept { return tx_limit_; }

  bool busy() const noexcept { return in_flight_; }

  std::size_t participant_count() const noexcept { return participants_.size(); }

 private:
  friend class VirtualBus;

  explicit VirtualMedium(BusTiming timing) : timing_(timing) {}

  void detach(VirtualBus* who) noexcept;
  /// Arbitrate and begin a transmission if the medium is idle and anything is
  /// queued. The frame is not delivered until it finishes; a frame is on the
  /// wire for its whole transmission time, which is what makes the timing
  /// model observable.
  bool start_transmission();
  void complete_transmission();
  void deliver(const FdFrame& frame, VirtualBus* sender);

  BusTiming timing_;
  std::uint64_t now_ns_ = 0;
  std::uint64_t busy_until_ns_ = 0;
  bool loopback_ = true;
  bool in_flight_ = false;
  FdFrame current_{};
  VirtualBus* current_sender_ = nullptr;
  std::size_t rx_limit_ = 4096;
  std::size_t tx_limit_ = 4096;
  std::vector<VirtualBus*> participants_;  // non-owning; see attach()/detach()
  std::vector<ArbitrationEvent> arbitration_log_;
};

class VirtualBus final : public IBus {
 public:
  ~VirtualBus() override;

  Status open() override;
  void close() noexcept override;
  bool is_open() const noexcept override { return open_; }

  Status send(const FdFrame& frame) override;
  using IBus::send;

  /// Pops a frame that has already been delivered. If none is waiting, the
  /// virtual clock is advanced in steps of up to `timeout` so that queued
  /// traffic can flow, and the queue is re-checked.
  Result<FdFrame> receive(std::chrono::nanoseconds timeout) override;

  Status set_filters(const std::vector<Filter>& filters) override;
  const std::vector<Filter>& filters() const noexcept override { return filters_; }

  const BusStatistics& statistics() const noexcept override { return stats_; }
  void reset_statistics() noexcept override { stats_ = {}; }

  std::string_view name() const noexcept override { return name_; }
  BusTiming timing() const noexcept override { return medium_->timing(); }

  /// Frames this participant has queued but not yet won arbitration for.
  std::size_t pending() const noexcept { return outbox_.size(); }
  std::size_t available() const noexcept { return inbox_.size(); }
  const std::shared_ptr<VirtualMedium>& medium() const noexcept { return medium_; }

 private:
  friend class VirtualMedium;
  VirtualBus(std::shared_ptr<VirtualMedium> medium, std::string name)
      : medium_(std::move(medium)), name_(std::move(name)) {}

  std::shared_ptr<VirtualMedium> medium_;
  std::string name_;
  std::deque<FdFrame> outbox_;
  std::deque<FdFrame> inbox_;
  std::vector<Filter> filters_;
  BusStatistics stats_;
  bool open_ = false;
};

}  // namespace canforge::transport

#endif  // CANFORGE_TRANSPORT_VIRTUALBUS_HPP
