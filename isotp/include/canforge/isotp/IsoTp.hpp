// SPDX-License-Identifier: MIT
#ifndef CANFORGE_ISOTP_ISOTP_HPP
#define CANFORGE_ISOTP_ISOTP_HPP

/// ISO 15765-2 transport protocol.
///
/// Nothing here sleeps, owns a thread, or touches a bus. You feed a session
/// received frames with `on_frame()` and push it along with `poll(now_ns)`,
/// which hands back whatever should go out. Every timeout is then just a
/// comparison against a timestamp the caller supplies, so N_Bs, N_Cr and STmin
/// can be tested to the microsecond against a fake clock instead of a slow and
/// flaky wall-clock test.
///
/// The timing is the part of the standard that's easy to misread. ISO 15765-2
/// defines six parameters, but they're not all the same kind of thing:
///
///   N_As  timeout   sender:   CAN frame transmission (link layer confirm)
///   N_Ar  timeout   receiver: CAN frame transmission
///   N_Bs  timeout   sender:   waiting for a Flow Control after FF or a block
///   N_Cr  timeout   receiver: waiting for the next Consecutive Frame
///   N_Br  *performance requirement* receiver: FF/block received -> FC sent
///   N_Cs  *performance requirement* sender:   FC received -> next CF sent
///
/// N_Br and N_Cs aren't timeouts at all. Nothing times out on them. They're
/// budgets the implementation has to stay inside, bounded by
///
///   N_Br + N_Ar < 0.9 * N_Bs        and        N_Cs + N_As < 0.9 * N_Cr
///
/// Modelling them as timeouts, which plenty of implementations do, is just
/// wrong. Here they're configurable delays and the two inequalities get checked
/// when a configuration is validated.
///
/// The default for the four real timeouts is 1000 ms.
///
/// STmin encoding:
///   0x00..0x7F   0 to 127 milliseconds
///   0xF1..0xF9   100 to 900 microseconds
///   everything else is reserved, and ISO 15765-2 says to treat a reserved
///   value as 0x7F, the slowest rate, not to reject it. Real ECUs do send
///   reserved values, so that's implemented rather than called an error.

#include <cstdint>
#include <string_view>
#include <vector>

#include "canforge/core/Frame.hpp"
#include "canforge/core/Result.hpp"

namespace canforge::isotp {

using core::CanId;
using core::FdFrame;
using core::Result;
using core::Status;

enum class Pci : std::uint8_t {
  SingleFrame = 0x0,
  FirstFrame = 0x1,
  ConsecutiveFrame = 0x2,
  FlowControl = 0x3,
};

enum class FlowStatus : std::uint8_t {
  ContinueToSend = 0,
  Wait = 1,
  Overflow = 2,
};

/// The largest payload a 12-bit FF_DL can describe. Above this the escape
/// sequence puts FF_DL to zero and follows it with a 32-bit length.
inline constexpr std::uint32_t kClassicMaxLength = 4095;

/// Result of a transfer, mapped from the standard's N_Result values.
enum class TransferResult : std::uint8_t {
  InProgress,
  Ok,
  TimeoutA,        ///< N_As / N_Ar
  TimeoutBs,       ///< N_Bs: no Flow Control arrived
  TimeoutCr,       ///< N_Cr: no Consecutive Frame arrived
  WrongSequence,   ///< N_WRONG_SN
  UnexpectedPdu,   ///< N_UNEXP_PDU
  BufferOverflow,  ///< N_BUFFER_OVFLW: the receiver said it cannot hold this
  WaitOverrun,     ///< N_WFT_OVRN: too many consecutive FC.WAIT frames
  InvalidPdu,
  Aborted,
};

const char* to_string(TransferResult r) noexcept;
const char* to_string(FlowStatus s) noexcept;

enum class AddressingMode : std::uint8_t {
  Normal,    ///< The CAN identifier alone selects the peer.
  Extended,  ///< The first payload byte carries an address extension.
};

struct Address {
  AddressingMode mode = AddressingMode::Normal;
  CanId tx_id;                    ///< Identifier this side transmits on.
  CanId rx_id;                    ///< Identifier this side listens on.
  std::uint8_t tx_extension = 0;  ///< Extended addressing only.
  std::uint8_t rx_extension = 0;

  /// Bytes of the payload consumed before the PCI. One for extended
  /// addressing, zero otherwise.
  std::uint8_t header_size() const noexcept {
    return mode == AddressingMode::Extended ? 1u : 0u;
  }

  static Address normal(CanId tx, CanId rx) {
    Address a;
    a.tx_id = tx;
    a.rx_id = rx;
    return a;
  }
  static Address extended(CanId tx, CanId rx, std::uint8_t tx_ext,
                          std::uint8_t rx_ext) {
    Address a;
    a.mode = AddressingMode::Extended;
    a.tx_id = tx;
    a.rx_id = rx;
    a.tx_extension = tx_ext;
    a.rx_extension = rx_ext;
    return a;
  }
};

struct Config {
  Address address;

  // Real timeouts, nanoseconds. ISO default 1000 ms each.
  std::uint64_t n_as_ns = 1000000000ULL;
  std::uint64_t n_ar_ns = 1000000000ULL;
  std::uint64_t n_bs_ns = 1000000000ULL;
  std::uint64_t n_cr_ns = 1000000000ULL;

  // Performance requirements, not timeouts. See the file comment.
  std::uint64_t n_br_ns = 0;
  std::uint64_t n_cs_ns = 0;

  /// Flow control this side offers when receiving. Block size 0 means "send
  /// everything without further flow control".
  std::uint8_t block_size = 8;
  std::uint8_t st_min_raw = 0;

  /// Maximum consecutive FC.WAIT frames tolerated before giving up.
  std::uint8_t wft_max = 4;

  /// Largest message this side is willing to receive; a First Frame above it
  /// is answered with FC.OVERFLOW.
  std::uint32_t max_receive_length = 4095;

  /// ISO mandates no padding value. 0x00, 0xAA and 0xCC all occur in the
  /// wild; CC is the most common in diagnostics.
  bool pad_frames = true;
  std::uint8_t padding_byte = 0xCC;

  /// Transmit CAN FD frames, which allows a Single Frame up to 62 bytes and
  /// avoids splitting most diagnostic responses at all.
  bool use_can_fd = false;
  bool fd_bit_rate_switch = true;

  /// Checks that the N_Br and N_Cs delays leave the link layer room under
  /// the ISO inequalities. See Config::validate for why the inequalities
  /// cannot be checked against the timeout maxima directly.
  Status validate() const noexcept;
};

/// STmin encoding <-> nanoseconds.
std::uint64_t st_min_to_ns(std::uint8_t raw) noexcept;
std::uint8_t ns_to_st_min(std::uint64_t ns) noexcept;

/// Segments an outgoing message. One instance handles one transfer.
class Sender {
 public:
  explicit Sender(Config config) : config_(config) {}

  /// Begin a transfer. Fails if one is already running or the payload is
  /// larger than the protocol can describe.
  Status begin(const std::uint8_t* data, std::size_t size, std::uint64_t now_ns);
  Status begin(const std::vector<std::uint8_t>& data, std::uint64_t now_ns) {
    return begin(data.data(), data.size(), now_ns);
  }

  void on_frame(const FdFrame& frame, std::uint64_t now_ns);
  std::vector<FdFrame> poll(std::uint64_t now_ns);

  TransferResult result() const noexcept { return result_; }
  bool done() const noexcept { return result_ != TransferResult::InProgress; }
  /// True while the sender is stalled waiting for a Flow Control, which is
  /// exactly the window in which N_Bs is running.
  bool waiting_for_flow_control() const noexcept {
    return state_ == State::WaitingFlowControl;
  }
  std::size_t sent_bytes() const noexcept { return offset_; }
  std::size_t total_bytes() const noexcept { return payload_.size(); }
  const Config& config() const noexcept { return config_; }
  void abort() noexcept { result_ = TransferResult::Aborted; }

 private:
  enum class State : std::uint8_t {
    Idle,
    SendingSingle,
    SendingFirst,
    WaitingFlowControl,
    SendingConsecutive,
    Finished,
  };

  FdFrame make_frame(const std::uint8_t* body, std::size_t body_size) const;

  Config config_;
  std::vector<std::uint8_t> payload_;
  State state_ = State::Idle;
  TransferResult result_ = TransferResult::InProgress;
  std::size_t offset_ = 0;
  std::uint8_t sequence_ = 0;
  std::uint8_t block_size_ = 0;  ///< From the peer's FC.
  std::uint64_t st_min_ns_ = 0;  ///< From the peer's FC.
  std::uint8_t frames_in_block_ = 0;
  std::uint8_t wait_count_ = 0;
  std::uint64_t deadline_ns_ = 0;   ///< N_Bs while waiting for FC.
  std::uint64_t next_send_ns_ = 0;  ///< STmin / N_Cs pacing.
};

class Receiver {
 public:
  explicit Receiver(Config config) : config_(config) {}

  void on_frame(const FdFrame& frame, std::uint64_t now_ns);
  std::vector<FdFrame> poll(std::uint64_t now_ns);

  TransferResult result() const noexcept { return result_; }
  bool done() const noexcept { return result_ != TransferResult::InProgress; }
  bool has_message() const noexcept { return result_ == TransferResult::Ok; }
  const std::vector<std::uint8_t>& message() const noexcept { return payload_; }
  std::size_t expected_length() const noexcept { return expected_; }
  const Config& config() const noexcept { return config_; }

  void reset() noexcept;

 private:
  enum class State : std::uint8_t {
    Idle,
    SendFlowControl,
    Receiving,
    Finished,
  };

  FdFrame make_flow_control(FlowStatus status) const;

  Config config_;
  std::vector<std::uint8_t> payload_;
  State state_ = State::Idle;
  TransferResult result_ = TransferResult::InProgress;
  std::size_t expected_ = 0;
  std::uint8_t sequence_ = 0;
  std::uint8_t frames_in_block_ = 0;
  std::uint64_t deadline_ns_ = 0;  ///< N_Cr while waiting for a CF.
  std::uint64_t fc_due_ns_ = 0;    ///< N_Br pacing for the Flow Control.
  FlowStatus pending_status_ = FlowStatus::ContinueToSend;
};

}  // namespace canforge::isotp

#endif  // CANFORGE_ISOTP_ISOTP_HPP
