// SPDX-License-Identifier: MIT
#include "canforge/transport/IBus.hpp"

namespace canforge::transport {
namespace {

/// Worst-case stuff bits for a stuffable region of `bits` bits: the encoder
/// inserts one complementary bit after every five identical bits, so the
/// densest possible pattern is one inserted bit per four original ones.
constexpr std::uint32_t worst_case_stuff_bits(std::uint32_t bits) noexcept {
  return bits == 0 ? 0u : (bits - 1u) / 4u;
}

FrameTiming classic_timing(const FdFrame& frame, const BusTiming& timing) noexcept {
  const auto payload_bits = static_cast<std::uint32_t>(
      (frame.is_remote() ? std::size_t{0} : frame.size()) * 8u);

  // Region subject to bit stuffing: SOF through the end of the CRC sequence.
  const std::uint32_t stuffable = (frame.id().is_extended() ? 54u : 34u) + payload_bits;
  // Fixed-form region: CRC delimiter, ACK slot, ACK delimiter, EOF, IFS.
  constexpr std::uint32_t kTrailer = 1u + 1u + 1u + 7u + 3u;

  FrameTiming out;
  out.stuff_bits = worst_case_stuff_bits(stuffable);
  out.arbitration_bits = stuffable + kTrailer + out.stuff_bits;
  out.data_bits = 0;
  out.nanoseconds =
      timing.nominal_bitrate == 0
          ? 0
          : (static_cast<std::uint64_t>(out.arbitration_bits) * 1000000000ULL) /
                timing.nominal_bitrate;
  return out;
}

FrameTiming fd_timing(const FdFrame& frame, const BusTiming& timing) noexcept {
  const auto length = static_cast<std::uint32_t>(frame.size());
  const std::uint32_t payload_bits = length * 8u;

  // Arbitration phase: everything up to and including the BRS bit is sent at
  // the nominal rate.
  //   standard  SOF 1 + ID 11 + RRS 1 + IDE 1 + FDF 1 + res 1 + BRS 1  = 17
  //   extended  + SRR 1 + 18 ID bits + r1 1                            = 37
  const std::uint32_t arbitration_core = frame.id().is_extended() ? 37u : 17u;

  // Data phase: ESI + DLC + data + CRC. ISO CAN FD prefixes the CRC with a
  // 3-bit stuff counter and a parity bit, and inserts a fixed stuff bit every
  // four bits of the CRC field regardless of the data.
  const std::uint32_t crc_bits = length > 16u ? 21u : 17u;
  const std::uint32_t crc_field = crc_bits + 4u;  // stuff counter + parity
  const std::uint32_t fixed_stuff = crc_field / 4u;
  const std::uint32_t dynamic_stuff =
      worst_case_stuff_bits(arbitration_core + 1u + 4u + payload_bits);

  FrameTiming out;
  out.stuff_bits = fixed_stuff + dynamic_stuff;
  // The trailing CRC delimiter, ACK and EOF return to the nominal rate.
  constexpr std::uint32_t kTrailer = 1u + 1u + 1u + 7u + 3u;
  out.arbitration_bits = arbitration_core + kTrailer;
  out.data_bits = 1u + 4u + payload_bits + crc_field + out.stuff_bits;

  const std::uint32_t data_rate = frame.is_brs() && timing.data_bitrate != 0
                                      ? timing.data_bitrate
                                      : timing.nominal_bitrate;
  const std::uint64_t nominal_ns =
      timing.nominal_bitrate == 0
          ? 0
          : (static_cast<std::uint64_t>(out.arbitration_bits) * 1000000000ULL) /
                timing.nominal_bitrate;
  const std::uint64_t data_ns =
      data_rate == 0
          ? 0
          : (static_cast<std::uint64_t>(out.data_bits) * 1000000000ULL) / data_rate;
  out.nanoseconds = nominal_ns + data_ns;
  return out;
}

}  // namespace

FrameTiming frame_timing(const FdFrame& frame, const BusTiming& timing) noexcept {
  return frame.is_fd() ? fd_timing(frame, timing) : classic_timing(frame, timing);
}

bool accepted_by(const std::vector<Filter>& filters, CanId id) noexcept {
  if (filters.empty()) {
    return true;
  }
  for (const Filter& f : filters) {
    if (f.accepts(id)) {
      return true;
    }
  }
  return false;
}

double BusStatistics::bus_load(const BusTiming& timing) const noexcept {
  const std::uint64_t elapsed = last_timestamp_ns >= first_timestamp_ns
                                    ? last_timestamp_ns - first_timestamp_ns
                                    : 0;
  if (elapsed == 0 || timing.nominal_bitrate == 0 ||
      (frames_sent + frames_received) < 2) {
    return 0.0;
  }
  const double seconds = static_cast<double>(elapsed) / 1e9;
  const double capacity = static_cast<double>(timing.nominal_bitrate) * seconds;
  if (capacity <= 0.0) {
    return 0.0;
  }
  const double used = static_cast<double>(wire_bits) / capacity;
  return used > 1.0 ? 1.0 : used;
}

}  // namespace canforge::transport
