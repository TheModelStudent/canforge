// SPDX-License-Identifier: MIT
#include "canforge/transport/VirtualBus.hpp"

#include <algorithm>

namespace canforge::transport {

std::shared_ptr<VirtualMedium> VirtualMedium::create(BusTiming timing) {
  // make_shared cannot reach the private constructor, so the allocation is
  // done here and handed straight to a shared_ptr; nothing owns a raw pointer
  // at any point.
  return std::shared_ptr<VirtualMedium>(new VirtualMedium(timing));
}

std::unique_ptr<VirtualBus> VirtualMedium::attach(std::string name) {
  auto bus = std::unique_ptr<VirtualBus>(
      new VirtualBus(shared_from_this(), std::move(name)));
  participants_.push_back(bus.get());
  return bus;
}

void VirtualMedium::detach(VirtualBus* who) noexcept {
  participants_.erase(
      std::remove(participants_.begin(), participants_.end(), who),
      participants_.end());
}

std::size_t VirtualMedium::pending() const noexcept {
  std::size_t n = 0;
  for (const VirtualBus* p : participants_) {
    n += p->outbox_.size();
  }
  return n;
}

void VirtualMedium::deliver(const FdFrame& frame, VirtualBus* sender) {
  for (VirtualBus* p : participants_) {
    if (p == sender && !loopback_) {
      continue;
    }
    if (!p->open_) {
      continue;
    }
    if (!accepted_by(p->filters_, frame.id())) {
      // A hardware acceptance filter discards the frame before it ever
      // reaches a queue, so this is not counted as a drop.
      continue;
    }
    if (p->inbox_.size() >= rx_limit_) {
      ++p->stats_.dropped;
      ++p->stats_.receive_errors;
      continue;
    }
    p->inbox_.push_back(frame);
    ++p->stats_.frames_received;
    p->stats_.bytes_received += frame.size();
    if (frame.is_error_frame()) {
      ++p->stats_.error_frames;
    }
    if (p->stats_.first_timestamp_ns == 0) {
      p->stats_.first_timestamp_ns = frame.timestamp_ns();
    }
    p->stats_.last_timestamp_ns = frame.timestamp_ns();
  }
}

bool VirtualMedium::start_transmission() {
  if (in_flight_) {
    return false;  // the medium is still carrying a frame
  }

  // Arbitration. Every participant with something queued presents the head of
  // its queue simultaneously; the lowest arbitration key wins.
  VirtualBus* winner = nullptr;
  std::uint64_t best_key = 0;
  std::size_t contenders = 0;
  for (VirtualBus* p : participants_) {
    if (!p->open_ || p->outbox_.empty()) {
      continue;
    }
    ++contenders;
    const std::uint64_t key = p->outbox_.front().id().arbitration_key();
    if (winner == nullptr || key < best_key) {
      winner = p;
      best_key = key;
    }
    // A tie means two nodes started transmitting the same identifier with
    // different data, which on a real bus destroys both frames and raises an
    // error. Modelling that faithfully would need bit-level simulation; here
    // the first participant to have attached wins and the collision shows up
    // as an arbitration loss for the other, which is enough to make the
    // duplicate-transmitter mistake visible in a test.
  }
  if (winner == nullptr) {
    return false;
  }

  FdFrame frame = winner->outbox_.front();
  winner->outbox_.pop_front();

  if (contenders > 1) {
    ArbitrationEvent event;
    event.at_ns = now_ns_;
    event.winner = frame.id();
    event.winner_name = winner->name_;
    for (VirtualBus* p : participants_) {
      if (p == winner || !p->open_ || p->outbox_.empty()) {
        continue;
      }
      event.losers.push_back(p->outbox_.front().id());
      // The loser keeps its frame at the head of the queue and contends again
      // as soon as the bus is free.
      ++p->stats_.arbitration_losses;
    }
    if (!event.losers.empty()) {
      arbitration_log_.push_back(std::move(event));
    }
  }

  const FrameTiming t = frame_timing(frame, timing_);
  busy_until_ns_ = now_ns_ + t.nanoseconds;
  // The receive timestamp is the end of frame, which is when a controller
  // raises its receive interrupt.
  frame.set_timestamp_ns(busy_until_ns_);

  ++winner->stats_.frames_sent;
  winner->stats_.bytes_sent += frame.size();
  winner->stats_.wire_bits += t.total_bits();
  if (winner->stats_.first_timestamp_ns == 0) {
    winner->stats_.first_timestamp_ns = now_ns_;
  }
  winner->stats_.last_timestamp_ns = busy_until_ns_;

  current_ = frame;
  current_sender_ = winner;
  in_flight_ = true;
  return true;
}

void VirtualMedium::complete_transmission() {
  if (!in_flight_) {
    return;
  }
  in_flight_ = false;
  deliver(current_, current_sender_);
  current_sender_ = nullptr;
}

void VirtualMedium::advance(std::chrono::nanoseconds by) {
  const std::uint64_t target =
      now_ns_ + static_cast<std::uint64_t>(by.count() < 0 ? 0 : by.count());
  for (;;) {
    if (in_flight_ && busy_until_ns_ <= now_ns_) {
      complete_transmission();
    }
    if (!in_flight_) {
      start_transmission();
    }
    if (in_flight_ && busy_until_ns_ <= target) {
      now_ns_ = busy_until_ns_;
      continue;  // the frame lands inside the interval; deliver and go again
    }
    now_ns_ = target;
    if (in_flight_ && busy_until_ns_ <= now_ns_) {
      complete_transmission();
    }
    return;
  }
}

std::chrono::nanoseconds VirtualMedium::drain(std::size_t max_frames) {
  const std::uint64_t start = now_ns_;
  std::size_t transmitted = 0;
  while (transmitted < max_frames) {
    if (in_flight_) {
      now_ns_ = busy_until_ns_;
      complete_transmission();
    }
    if (!start_transmission()) {
      break;
    }
    ++transmitted;
  }
  if (in_flight_) {
    now_ns_ = busy_until_ns_;
    complete_transmission();
  }
  return std::chrono::nanoseconds(now_ns_ - start);
}

VirtualBus::~VirtualBus() {
  if (medium_) {
    medium_->detach(this);
  }
}

Status VirtualBus::open() {
  open_ = true;
  return core::ok();
}

void VirtualBus::close() noexcept {
  open_ = false;
  outbox_.clear();
  inbox_.clear();
}

Status VirtualBus::send(const FdFrame& frame) {
  if (!open_) {
    return core::Error(core::ErrorCode::TransportNotOpen,
                       "the virtual bus participant is not open");
  }
  if (frame.is_fd() && medium_->timing().data_bitrate == 0) {
    return core::Error(core::ErrorCode::TransportUnsupported,
                       "this medium has no CAN FD data bitrate configured");
  }
  if (outbox_.size() >= medium_->transmit_queue_limit()) {
    ++stats_.send_errors;
    return core::Error(core::ErrorCode::TransportOverflow,
                       "the transmit queue is full");
  }
  outbox_.push_back(frame);
  return core::ok();
}

Result<FdFrame> VirtualBus::receive(std::chrono::nanoseconds timeout) {
  if (!open_) {
    return core::Error(core::ErrorCode::TransportNotOpen,
                       "the virtual bus participant is not open");
  }
  if (inbox_.empty()) {
    // Let the medium run for up to the timeout so that anything queued gets a
    // chance to be transmitted. This is what lets a test write the same
    // send/receive code it would write against SocketCAN.
    if (timeout.count() > 0) {
      medium_->advance(timeout);
    }
  }
  if (inbox_.empty()) {
    return core::Error(core::ErrorCode::TransportTimeout,
                       "no frame arrived before the timeout expired");
  }
  FdFrame frame = inbox_.front();
  inbox_.pop_front();
  return frame;
}

Status VirtualBus::set_filters(const std::vector<Filter>& filters) {
  filters_ = filters;
  return core::ok();
}

}  // namespace canforge::transport
