// SPDX-License-Identifier: MIT
#include "canforge/core/Frame.hpp"

namespace canforge::core {

Result<CanId> CanId::standard(std::uint32_t id) noexcept {
  if (id > kStandardIdMax) {
    return Error(ErrorCode::FrameBadIdentifier,
                 "standard CAN identifier does not fit in 11 bits",
                 {id, kStandardIdMax});
  }
  // 0x7F0..0x7FF violate the ISO 11898-1 rule that the top seven base-identifier
  // bits must not all be recessive. Real buses carry them; see violates_base_id_rule().
  return CanId(id, false);
}

Result<CanId> CanId::extended(std::uint32_t id) noexcept {
  if (id > kExtendedIdMax) {
    return Error(ErrorCode::FrameBadIdentifier,
                 "extended CAN identifier does not fit in 29 bits",
                 {id, kExtendedIdMax});
  }
  return CanId(id, true);
}

Result<CanId> CanId::from_packed(std::uint32_t packed) noexcept {
  const bool eff = (packed & kEffFlag) != 0u;
  const std::uint32_t id = packed & kExtendedIdMax;
  return eff ? extended(id) : standard(id);
}

Result<DlcFit> length_to_dlc(std::size_t len, bool fd) noexcept {
  const std::uint8_t limit = fd ? std::uint8_t{64} : std::uint8_t{8};
  if (len > limit) {
    return Error(ErrorCode::FramePayloadTooLarge,
                 "no data length code can carry this many bytes",
                 {static_cast<std::uint32_t>(len), limit});
  }
  if (!fd) {
    const auto n = static_cast<std::uint8_t>(len);
    return DlcFit{n, n, 0};
  }
  for (std::uint8_t dlc = 0; dlc < 16u; ++dlc) {
    const std::uint8_t encoded = kFdDlcToLength[dlc];
    if (encoded >= len) {
      return DlcFit{
          dlc, encoded,
          static_cast<std::uint8_t>(encoded - static_cast<std::uint8_t>(len))};
    }
  }
  return Error(ErrorCode::FrameBadDlc, "unreachable: no data length code found");
}

}  // namespace canforge::core
