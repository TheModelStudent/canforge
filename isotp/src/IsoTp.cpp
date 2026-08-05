// SPDX-License-Identifier: MIT
#include "canforge/isotp/IsoTp.hpp"

#include <algorithm>
#include <cstring>

namespace canforge::isotp {
namespace {

using core::Error;
using core::ErrorCode;
using core::FrameFlags;

/// Round a payload length up to something a CAN FD frame can actually carry.
std::size_t fd_padded_length(std::size_t want) noexcept {
  static constexpr std::size_t kLengths[] = {0, 1,  2,  3,  4,  5,  6,  7,
                                             8, 12, 16, 20, 24, 32, 48, 64};
  for (const std::size_t n : kLengths) {
    if (n >= want) {
      return n;
    }
  }
  return 64;
}

}  // namespace

const char* to_string(TransferResult r) noexcept {
  switch (r) {
      // clang-format off
    case TransferResult::InProgress:     return "in progress";
    case TransferResult::Ok:             return "ok";
    case TransferResult::TimeoutA:       return "N_As/N_Ar timeout";
    case TransferResult::TimeoutBs:      return "N_Bs timeout: no flow control";
    case TransferResult::TimeoutCr:      return "N_Cr timeout: no consecutive frame";
    case TransferResult::WrongSequence:  return "wrong sequence number";
    case TransferResult::UnexpectedPdu:  return "unexpected protocol data unit";
    case TransferResult::BufferOverflow: return "receiver buffer overflow";
    case TransferResult::WaitOverrun:    return "too many wait frames";
    case TransferResult::InvalidPdu:     return "malformed protocol data unit";
    case TransferResult::Aborted:        return "aborted";
      // clang-format on
  }
  return "unknown";
}

const char* to_string(FlowStatus s) noexcept {
  switch (s) {
      // clang-format off
    case FlowStatus::ContinueToSend: return "continue to send";
    case FlowStatus::Wait:           return "wait";
    case FlowStatus::Overflow:       return "overflow";
      // clang-format on
  }
  return "reserved";
}

std::uint64_t st_min_to_ns(std::uint8_t raw) noexcept {
  if (raw <= 0x7Fu) {
    return static_cast<std::uint64_t>(raw) * 1000000ULL;  // 0..127 ms
  }
  if (raw >= 0xF1u && raw <= 0xF9u) {
    // 0xF1..0xF9 encode 100..900 microseconds.
    return static_cast<std::uint64_t>(raw - 0xF0u) * 100000ULL;
  }
  // Reserved. ISO 15765-2 says to treat a reserved value as 0x7F rather than
  // reject the frame, so a peer with a buggy encoder still works, slowly.
  return 127ULL * 1000000ULL;
}

std::uint8_t ns_to_st_min(std::uint64_t ns) noexcept {
  if (ns == 0) {
    return 0;
  }
  if (ns < 1000000ULL) {
    // Sub-millisecond: round up into the 100 us grid, clamped to 0xF9.
    const std::uint64_t hundreds = (ns + 99999ULL) / 100000ULL;
    const std::uint64_t clamped =
        std::min<std::uint64_t>(std::max<std::uint64_t>(hundreds, 1), 9);
    return static_cast<std::uint8_t>(0xF0u + clamped);
  }
  const std::uint64_t ms = std::min<std::uint64_t>((ns + 999999ULL) / 1000000ULL, 127);
  return static_cast<std::uint8_t>(ms);
}

Status Config::validate() const noexcept {
  if (n_bs_ns == 0 || n_cr_ns == 0) {
    return Error(ErrorCode::InvalidArgument, "N_Bs and N_Cr must be non-zero");
  }
  // ISO 15765-2 constrains the *measured* durations:
  //
  //     N_Br + N_Ar < 0.9 * N_Bs        N_Cs + N_As < 0.9 * N_Cr
  //
  // A subtlety that is easy to get wrong: N_Ar and N_As in those inequalities
  // are how long a CAN frame actually took to go out, not the timeout maxima.
  // Substituting the maxima -- 1000 ms each by default -- makes the inequality
  // unsatisfiable using the standard's own default values, so a check written
  // that way rejects every conforming configuration.
  //
  // What this implementation actually controls is the N_Br and N_Cs delays it
  // chooses to insert. Those are what is checked here; the remaining slack up
  // to 0.9 * N_Bs (or N_Cr) is the budget left for the link layer.
  if (n_br_ns >= (n_bs_ns * 9ULL) / 10ULL) {
    return Error(ErrorCode::InvalidArgument,
                 "N_Br leaves no room under 0.9 * N_Bs for the link layer");
  }
  if (n_cs_ns >= (n_cr_ns * 9ULL) / 10ULL) {
    return Error(ErrorCode::InvalidArgument,
                 "N_Cs leaves no room under 0.9 * N_Cr for the link layer");
  }
  if (max_receive_length == 0) {
    return Error(ErrorCode::InvalidArgument, "max_receive_length must be non-zero");
  }
  return core::ok();
}

FdFrame Sender::make_frame(const std::uint8_t* body, std::size_t body_size) const {
  std::array<std::uint8_t, 64> buffer{};
  std::size_t n = 0;
  if (config_.address.mode == AddressingMode::Extended) {
    buffer[n++] = config_.address.tx_extension;
  }
  std::memcpy(buffer.data() + n, body, body_size);
  n += body_size;

  std::size_t length = n;
  if (config_.pad_frames) {
    length = config_.use_can_fd ? fd_padded_length(n) : std::size_t{8};
    if (length < n) {
      length = n;
    }
    for (std::size_t i = n; i < length; ++i) {
      buffer[i] = config_.padding_byte;
    }
  }

  FrameFlags flags = FrameFlags::None;
  if (config_.use_can_fd) {
    flags |= FrameFlags::Fd;
    if (config_.fd_bit_rate_switch) {
      flags |= FrameFlags::Brs;
    }
    length = fd_padded_length(length);
  }
  return FdFrame::make(config_.address.tx_id, buffer.data(), length, flags).value();
}

Status Sender::begin(const std::uint8_t* data, std::size_t size, std::uint64_t now_ns) {
  if (state_ != State::Idle) {
    return Error(ErrorCode::InvalidArgument, "a transfer is already running");
  }
  const Status valid = config_.validate();
  if (!valid) {
    return valid;
  }
  // The 32-bit escape length caps a transfer at 4 GiB - 1.
  if (size > 0xFFFFFFFFULL) {
    return Error(ErrorCode::FramePayloadTooLarge,
                 "ISO-TP cannot describe a message this long");
  }
  payload_.assign(data, data + size);
  offset_ = 0;
  sequence_ = 0;
  wait_count_ = 0;
  frames_in_block_ = 0;
  result_ = TransferResult::InProgress;

  // A Single Frame carries the length in the low nibble of the PCI. Classic
  // CAN leaves 7 payload bytes (6 with extended addressing); CAN FD adds an
  // escape where the nibble is zero and a length byte follows, reaching 62.
  const std::size_t header = config_.address.header_size();
  const std::size_t single_max = 7u - header;
  state_ = size <= single_max ? State::SendingSingle : State::SendingFirst;
  next_send_ns_ = now_ns;
  return core::ok();
}

std::vector<FdFrame> Sender::poll(std::uint64_t now_ns) {
  std::vector<FdFrame> out;
  if (done()) {
    return out;
  }

  switch (state_) {
    case State::Idle:
      break;

    case State::SendingSingle: {
      std::array<std::uint8_t, 8> body{};
      body[0] = static_cast<std::uint8_t>(payload_.size());  // PCI 0x0N
      std::memcpy(body.data() + 1, payload_.data(), payload_.size());
      out.push_back(make_frame(body.data(), payload_.size() + 1));
      offset_ = payload_.size();
      result_ = TransferResult::Ok;
      state_ = State::Finished;
      break;
    }

    case State::SendingFirst: {
      std::array<std::uint8_t, 8> body{};
      std::size_t body_size = 0;
      const std::size_t header = config_.address.header_size();
      if (payload_.size() <= kClassicMaxLength) {
        body[0] = static_cast<std::uint8_t>(0x10u | ((payload_.size() >> 8u) & 0x0Fu));
        body[1] = static_cast<std::uint8_t>(payload_.size() & 0xFFu);
        body_size = 2;
      } else {
        // Escape sequence: FF_DL of zero, then a 32-bit length.
        body[0] = 0x10u;
        body[1] = 0x00u;
        const auto len = static_cast<std::uint32_t>(payload_.size());
        body[2] = static_cast<std::uint8_t>((len >> 24u) & 0xFFu);
        body[3] = static_cast<std::uint8_t>((len >> 16u) & 0xFFu);
        body[4] = static_cast<std::uint8_t>((len >> 8u) & 0xFFu);
        body[5] = static_cast<std::uint8_t>(len & 0xFFu);
        body_size = 6;
      }
      const std::size_t room = 8u - header - body_size;
      const std::size_t take = std::min(room, payload_.size());
      std::memcpy(body.data() + body_size, payload_.data(), take);
      out.push_back(make_frame(body.data(), body_size + take));
      offset_ = take;
      sequence_ = 1;
      state_ = State::WaitingFlowControl;
      deadline_ns_ = now_ns + config_.n_bs_ns;
      break;
    }

    case State::WaitingFlowControl:
      if (now_ns >= deadline_ns_) {
        result_ = TransferResult::TimeoutBs;
        state_ = State::Finished;
      }
      break;

    case State::SendingConsecutive: {
      if (now_ns < next_send_ns_) {
        break;  // STmin has not elapsed
      }
      const std::size_t header = config_.address.header_size();
      const std::size_t room = 8u - header - 1u;
      std::array<std::uint8_t, 8> body{};
      body[0] = static_cast<std::uint8_t>(0x20u | (sequence_ & 0x0Fu));
      const std::size_t take = std::min(room, payload_.size() - offset_);
      std::memcpy(body.data() + 1, payload_.data() + offset_, take);
      out.push_back(make_frame(body.data(), take + 1));
      offset_ += take;
      sequence_ = static_cast<std::uint8_t>((sequence_ + 1u) & 0x0Fu);
      ++frames_in_block_;
      next_send_ns_ = now_ns + st_min_ns_;

      if (offset_ >= payload_.size()) {
        result_ = TransferResult::Ok;
        state_ = State::Finished;
      } else if (block_size_ != 0 && frames_in_block_ >= block_size_) {
        // The block is complete; wait for the next Flow Control.
        frames_in_block_ = 0;
        state_ = State::WaitingFlowControl;
        deadline_ns_ = now_ns + config_.n_bs_ns;
      }
      break;
    }

    case State::Finished:
      break;
  }
  return out;
}

void Sender::on_frame(const FdFrame& frame, std::uint64_t now_ns) {
  if (done() || frame.id() != config_.address.rx_id) {
    return;
  }
  const std::size_t header = config_.address.header_size();
  if (frame.size() <= header) {
    return;
  }
  if (config_.address.mode == AddressingMode::Extended &&
      frame.data()[0] != config_.address.rx_extension) {
    return;  // addressed to a different peer on the same identifier
  }
  const std::uint8_t* body = frame.data() + header;
  const std::size_t body_size = frame.size() - header;
  const auto pci = static_cast<std::uint8_t>((body[0] >> 4u) & 0x0Fu);

  if (pci != static_cast<std::uint8_t>(Pci::FlowControl)) {
    // Anything other than a Flow Control while sending is out of place.
    result_ = TransferResult::UnexpectedPdu;
    state_ = State::Finished;
    return;
  }
  if (state_ != State::WaitingFlowControl) {
    result_ = TransferResult::UnexpectedPdu;
    state_ = State::Finished;
    return;
  }
  if (body_size < 3) {
    result_ = TransferResult::InvalidPdu;
    state_ = State::Finished;
    return;
  }

  const auto status = static_cast<std::uint8_t>(body[0] & 0x0Fu);
  switch (status) {
    case static_cast<std::uint8_t>(FlowStatus::ContinueToSend):
      block_size_ = body[1];
      st_min_ns_ = st_min_to_ns(body[2]);
      wait_count_ = 0;
      frames_in_block_ = 0;
      state_ = State::SendingConsecutive;
      // N_Cs is a budget, not a timeout: it bounds how long this side may take
      // to get the next CF out. Honouring it as a deliberate delay keeps the
      // peer's N_Cr comfortable.
      next_send_ns_ = now_ns + config_.n_cs_ns;
      break;

    case static_cast<std::uint8_t>(FlowStatus::Wait):
      ++wait_count_;
      if (config_.wft_max != 0 && wait_count_ > config_.wft_max) {
        result_ = TransferResult::WaitOverrun;
        state_ = State::Finished;
        break;
      }
      // Still waiting: restart N_Bs and keep listening.
      deadline_ns_ = now_ns + config_.n_bs_ns;
      break;

    case static_cast<std::uint8_t>(FlowStatus::Overflow):
      result_ = TransferResult::BufferOverflow;
      state_ = State::Finished;
      break;

    default:
      result_ = TransferResult::InvalidPdu;
      state_ = State::Finished;
      break;
  }
}

void Receiver::reset() noexcept {
  payload_.clear();
  state_ = State::Idle;
  result_ = TransferResult::InProgress;
  expected_ = 0;
  sequence_ = 0;
  frames_in_block_ = 0;
  deadline_ns_ = 0;
  fc_due_ns_ = 0;
}

FdFrame Receiver::make_flow_control(FlowStatus status) const {
  std::array<std::uint8_t, 64> buffer{};
  std::size_t n = 0;
  if (config_.address.mode == AddressingMode::Extended) {
    buffer[n++] = config_.address.tx_extension;
  }
  buffer[n++] = static_cast<std::uint8_t>(0x30u | static_cast<std::uint8_t>(status));
  buffer[n++] = config_.block_size;
  buffer[n++] = config_.st_min_raw;

  std::size_t length = n;
  if (config_.pad_frames) {
    length = config_.use_can_fd ? fd_padded_length(n) : std::size_t{8};
    for (std::size_t i = n; i < length; ++i) {
      buffer[i] = config_.padding_byte;
    }
  }
  FrameFlags flags = FrameFlags::None;
  if (config_.use_can_fd) {
    flags |= FrameFlags::Fd;
    if (config_.fd_bit_rate_switch) {
      flags |= FrameFlags::Brs;
    }
    length = fd_padded_length(length);
  }
  return FdFrame::make(config_.address.tx_id, buffer.data(), length, flags).value();
}

std::vector<FdFrame> Receiver::poll(std::uint64_t now_ns) {
  std::vector<FdFrame> out;
  switch (state_) {
    case State::SendFlowControl:
      // N_Br: the budget between receiving a First Frame (or completing a
      // block) and putting the Flow Control on the wire.
      if (now_ns >= fc_due_ns_) {
        out.push_back(make_flow_control(pending_status_));
        if (pending_status_ == FlowStatus::Overflow) {
          result_ = TransferResult::BufferOverflow;
          state_ = State::Finished;
        } else {
          state_ = State::Receiving;
          frames_in_block_ = 0;
          deadline_ns_ = now_ns + config_.n_cr_ns;
        }
      }
      break;

    case State::Receiving:
      if (now_ns >= deadline_ns_) {
        result_ = TransferResult::TimeoutCr;
        state_ = State::Finished;
      }
      break;

    case State::Idle:
    case State::Finished:
      break;
  }
  return out;
}

void Receiver::on_frame(const FdFrame& frame, std::uint64_t now_ns) {
  if (frame.id() != config_.address.rx_id) {
    return;
  }
  if (state_ == State::Finished) {
    return;
  }
  const std::size_t header = config_.address.header_size();
  if (frame.size() <= header) {
    return;
  }
  if (config_.address.mode == AddressingMode::Extended &&
      frame.data()[0] != config_.address.rx_extension) {
    return;
  }
  const std::uint8_t* body = frame.data() + header;
  const std::size_t body_size = frame.size() - header;
  const auto pci = static_cast<std::uint8_t>((body[0] >> 4u) & 0x0Fu);

  switch (static_cast<Pci>(pci)) {
    case Pci::SingleFrame: {
      if (state_ != State::Idle) {
        result_ = TransferResult::UnexpectedPdu;
        state_ = State::Finished;
        return;
      }
      std::size_t length = body[0] & 0x0Fu;
      std::size_t data_at = 1;
      if (length == 0) {
        // CAN FD escape: the length is in the following byte, allowing a
        // single frame of up to 62 bytes.
        if (body_size < 2) {
          result_ = TransferResult::InvalidPdu;
          state_ = State::Finished;
          return;
        }
        length = body[1];
        data_at = 2;
      }
      if (length == 0 || data_at + length > body_size) {
        result_ = TransferResult::InvalidPdu;
        state_ = State::Finished;
        return;
      }
      payload_.assign(body + data_at, body + data_at + length);
      expected_ = length;
      result_ = TransferResult::Ok;
      state_ = State::Finished;
      return;
    }

    case Pci::FirstFrame: {
      if (state_ != State::Idle) {
        result_ = TransferResult::UnexpectedPdu;
        state_ = State::Finished;
        return;
      }
      if (body_size < 2) {
        result_ = TransferResult::InvalidPdu;
        state_ = State::Finished;
        return;
      }
      std::size_t length = (static_cast<std::size_t>(body[0] & 0x0Fu) << 8u) | body[1];
      std::size_t data_at = 2;
      if (length == 0) {
        // Escape sequence for messages above 4095 bytes.
        if (body_size < 6) {
          result_ = TransferResult::InvalidPdu;
          state_ = State::Finished;
          return;
        }
        length = (static_cast<std::size_t>(body[2]) << 24u) |
                 (static_cast<std::size_t>(body[3]) << 16u) |
                 (static_cast<std::size_t>(body[4]) << 8u) |
                 static_cast<std::size_t>(body[5]);
        data_at = 6;
      }
      if (length <= 7u - header) {
        // A message that would fit in a Single Frame must not be segmented.
        result_ = TransferResult::InvalidPdu;
        state_ = State::Finished;
        return;
      }
      expected_ = length;

      if (length > config_.max_receive_length) {
        pending_status_ = FlowStatus::Overflow;
        state_ = State::SendFlowControl;
        fc_due_ns_ = now_ns + config_.n_br_ns;
        return;
      }
      payload_.assign(body + data_at, body + body_size);
      if (payload_.size() > expected_) {
        payload_.resize(expected_);
      }
      sequence_ = 1;
      pending_status_ = FlowStatus::ContinueToSend;
      state_ = State::SendFlowControl;
      fc_due_ns_ = now_ns + config_.n_br_ns;
      return;
    }

    case Pci::ConsecutiveFrame: {
      if (state_ != State::Receiving) {
        result_ = TransferResult::UnexpectedPdu;
        state_ = State::Finished;
        return;
      }
      const auto sn = static_cast<std::uint8_t>(body[0] & 0x0Fu);
      if (sn != sequence_) {
        // N_WRONG_SN. The standard requires an abort, not an attempt to
        // resynchronise, because a gap means data was lost.
        result_ = TransferResult::WrongSequence;
        state_ = State::Finished;
        return;
      }
      sequence_ = static_cast<std::uint8_t>((sequence_ + 1u) & 0x0Fu);

      const std::size_t remaining = expected_ - payload_.size();
      const std::size_t available = body_size - 1u;
      const std::size_t take = std::min(remaining, available);
      payload_.insert(payload_.end(), body + 1, body + 1 + take);
      ++frames_in_block_;

      if (payload_.size() >= expected_) {
        result_ = TransferResult::Ok;
        state_ = State::Finished;
        return;
      }
      deadline_ns_ = now_ns + config_.n_cr_ns;
      if (config_.block_size != 0 && frames_in_block_ >= config_.block_size) {
        pending_status_ = FlowStatus::ContinueToSend;
        state_ = State::SendFlowControl;
        fc_due_ns_ = now_ns + config_.n_br_ns;
      }
      return;
    }

    case Pci::FlowControl:
      // A Flow Control aimed at a receiver means the peer is confused.
      result_ = TransferResult::UnexpectedPdu;
      state_ = State::Finished;
      return;
  }

  result_ = TransferResult::InvalidPdu;
  state_ = State::Finished;
}

}  // namespace canforge::isotp
