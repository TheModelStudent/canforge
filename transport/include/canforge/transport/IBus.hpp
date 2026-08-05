// SPDX-License-Identifier: MIT
#ifndef CANFORGE_TRANSPORT_IBUS_HPP
#define CANFORGE_TRANSPORT_IBUS_HPP

/// The bus abstraction every backend implements, plus the frame timing model
/// the virtual bus and the bus-load estimate share.

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "canforge/core/Frame.hpp"
#include "canforge/core/Result.hpp"

namespace canforge::transport {

using core::CanId;
using core::FdFrame;
using core::Frame;
using core::Result;
using core::Status;

struct BusTiming {
  std::uint32_t nominal_bitrate = 500000;   ///< Arbitration phase, bits/s.
  std::uint32_t data_bitrate = 2000000;     ///< CAN FD data phase, bits/s.
};

struct FrameTiming {
  std::uint32_t arbitration_bits = 0;  ///< Transmitted at the nominal rate.
  std::uint32_t data_bits = 0;         ///< At the data rate when BRS is set.
  std::uint32_t stuff_bits = 0;        ///< Included in the two counts above.
  std::uint64_t nanoseconds = 0;

  std::uint32_t total_bits() const noexcept {
    return arbitration_bits + data_bits;
  }
};

/// Worst-case time on the wire for a frame.
///
/// Classic CAN, from ISO 11898-1's frame layout:
///   standard  SOF 1 + ID 11 + RTR 1 + IDE 1 + r0 1 + DLC 4 + data 8N
///             + CRC 15 + CRC delimiter 1 + ACK 1 + ACK delimiter 1
///             + EOF 7 + IFS 3                                = 47 + 8N
///   extended  additionally SRR 1 + IDE 1 + 18 ID bits + r1 1 = 67 + 8N
///
/// Stuffing applies from the SOF through the end of the CRC sequence -- not to
/// the delimiters, ACK, EOF or IFS -- worst case one stuff bit per four:
///   stuff_max = floor((stuffable_bits - 1) / 4)
///
/// which gives 135 bits for a standard 8-byte frame and 160 for an extended
/// one, the figures quoted in the CAN response-time literature. Worst case
/// rather than average, because a simulator that is optimistic about bus load
/// is worse than useless.
///
/// CAN FD is an estimate: arbitration runs at the nominal rate up to and
/// including BRS, the data phase at the data rate, the CRC is 17 bits up to 16
/// data bytes and 21 above, and the ISO variant adds a 3-bit stuff counter with
/// parity plus a fixed stuff bit every four bits of the CRC field.
FrameTiming frame_timing(const FdFrame& frame, const BusTiming& timing) noexcept;

/// A SocketCAN-style acceptance filter: a frame passes when
/// `(id & mask) == (filter & mask)`, optionally inverted.
struct Filter {
  std::uint32_t id = 0;    ///< Packed form, so bit 31 selects extended frames.
  std::uint32_t mask = 0;  ///< Zero mask accepts everything.
  bool invert = false;     ///< SocketCAN's CAN_INV_FILTER.
  bool match_format = true;  ///< Compare the extended flag as well as the bits.

  static Filter exact(CanId wanted) noexcept {
    Filter f;
    f.id = wanted.packed();
    f.mask = core::kExtendedIdMax | core::kEffFlag;
    return f;
  }
  static Filter range(std::uint32_t base, std::uint32_t bits) noexcept {
    Filter f;
    f.id = base;
    f.mask = bits;
    f.match_format = false;
    return f;
  }

  bool accepts(CanId candidate) const noexcept {
    const std::uint32_t effective =
        match_format ? mask : (mask & core::kExtendedIdMax);
    return ((candidate.packed() & effective) == (id & effective)) != invert;
  }
};

/// An empty filter list accepts everything, matching SocketCAN's default.
bool accepted_by(const std::vector<Filter>& filters, CanId id) noexcept;

struct BusStatistics {
  std::uint64_t frames_sent = 0;
  std::uint64_t frames_received = 0;
  std::uint64_t bytes_sent = 0;
  std::uint64_t bytes_received = 0;
  std::uint64_t send_errors = 0;
  std::uint64_t receive_errors = 0;
  std::uint64_t error_frames = 0;
  std::uint64_t arbitration_losses = 0;
  std::uint64_t dropped = 0;  ///< Overruns and filtered-away frames.

  /// Bits actually put on the wire, worst case, used for the load estimate.
  std::uint64_t wire_bits = 0;
  std::uint64_t first_timestamp_ns = 0;
  std::uint64_t last_timestamp_ns = 0;

  /// Fraction of the bus's capacity consumed, in 0..1. Returns 0 when less
  /// than two frames have been seen, since a single frame gives no interval to
  /// measure against.
  double bus_load(const BusTiming& timing) const noexcept;

  std::chrono::nanoseconds span() const noexcept {
    return std::chrono::nanoseconds(last_timestamp_ns >= first_timestamp_ns
                                        ? last_timestamp_ns - first_timestamp_ns
                                        : 0);
  }
};

/// Every backend speaks in `FdFrame`, which is a superset of the classic
/// frame; a classic-only backend rejects frames whose FD flag is set. Using
/// one currency keeps the interface from splitting in two, at the cost of 80
/// bytes per frame at the interface boundary rather than 24.
class IBus {
 public:
  IBus() = default;
  virtual ~IBus() = default;
  IBus(const IBus&) = delete;
  IBus& operator=(const IBus&) = delete;
  IBus(IBus&&) = delete;
  IBus& operator=(IBus&&) = delete;

  virtual Status open() = 0;
  virtual void close() noexcept = 0;
  virtual bool is_open() const noexcept = 0;

  virtual Status send(const FdFrame& frame) = 0;

  /// Blocks for at most `timeout`. Returns `TransportTimeout` when nothing
  /// arrived, which is an expected outcome and not an error condition.
  virtual Result<FdFrame> receive(std::chrono::nanoseconds timeout) = 0;

  /// Replaces the whole filter set. An empty list accepts everything.
  virtual Status set_filters(const std::vector<Filter>& filters) = 0;
  virtual const std::vector<Filter>& filters() const noexcept = 0;

  virtual const BusStatistics& statistics() const noexcept = 0;
  virtual void reset_statistics() noexcept = 0;

  virtual std::string_view name() const noexcept = 0;
  virtual BusTiming timing() const noexcept = 0;

  Status send(const Frame& frame) { return send(frame.widen<64>()); }

  double bus_load() const noexcept { return statistics().bus_load(timing()); }
};

}  // namespace canforge::transport

#endif  // CANFORGE_TRANSPORT_IBUS_HPP
