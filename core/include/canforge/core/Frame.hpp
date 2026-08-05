// SPDX-License-Identifier: MIT
#ifndef CANFORGE_CORE_FRAME_HPP
#define CANFORGE_CORE_FRAME_HPP

/// BasicFrame is a template over payload capacity so classic CAN stays 24
/// bytes -- small enough that the virtual bus queue is cheap -- while FD gets
/// its full 64. Both are trivially copyable, so a transport can hand them
/// straight to write(2) with no marshalling step.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <type_traits>

#include "canforge/core/Result.hpp"

namespace canforge::core {

inline constexpr std::uint32_t kStandardIdMax = 0x7FFu;       // 11 bits
inline constexpr std::uint32_t kExtendedIdMax = 0x1FFFFFFFu;  // 29 bits

/// Bit 31 marks an extended identifier, matching SocketCAN's CAN_EFF_FLAG so
/// SocketCanBus moves the value across without translating it.
inline constexpr std::uint32_t kEffFlag = 0x80000000u;

/// A CAN identifier that cannot be constructed out of range. Runtime values
/// go through standard()/extended(), which return a Result; compile-time
/// constants go through make_standard<Id>(), which static_asserts instead.
class CanId {
 public:
  constexpr CanId() noexcept = default;

  static Result<CanId> standard(std::uint32_t id) noexcept;
  static Result<CanId> extended(std::uint32_t id) noexcept;

  template <std::uint32_t Id>
  static constexpr CanId make_standard() noexcept {
    static_assert(Id <= kStandardIdMax,
                  "standard CAN identifier does not fit in 11 bits");
    return CanId(Id, false);
  }

  template <std::uint32_t Id>
  static constexpr CanId make_extended() noexcept {
    static_assert(Id <= kExtendedIdMax,
                  "extended CAN identifier does not fit in 29 bits");
    return CanId(Id, true);
  }

  /// Rebuild from the packed SocketCAN-style word. Transport code only.
  static Result<CanId> from_packed(std::uint32_t packed) noexcept;

  constexpr std::uint32_t value() const noexcept {
    return packed_ & kExtendedIdMax;
  }
  constexpr std::uint32_t packed() const noexcept { return packed_; }
  constexpr bool is_extended() const noexcept {
    return (packed_ & kEffFlag) != 0u;
  }
  constexpr std::uint8_t bit_count() const noexcept {
    return is_extended() ? std::uint8_t{29} : std::uint8_t{11};
  }

  /// ISO 11898-1 forbids a base identifier whose top seven bits are all
  /// recessive (0x7F0..0x7FF), but no controller enforces it and shipped
  /// databases contain them. standard() accepts them; this only reports them.
  constexpr bool violates_base_id_rule() const noexcept {
    return !is_extended() && value() >= 0x7F0u;
  }

  /// Sort key for bus arbitration: lower wins. The 11-bit base compares first,
  /// then on a tie the standard frame wins, because the IDE bit that follows
  /// is dominant for standard and recessive for extended (ISO 11898-1). Only
  /// then do the 18 extension bits decide.
  constexpr std::uint64_t arbitration_key() const noexcept {
    const std::uint32_t base = is_extended() ? (value() >> 18) : value();
    const std::uint32_t ide = is_extended() ? 1u : 0u;
    const std::uint32_t ext = is_extended() ? (value() & 0x3FFFFu) : 0u;
    return (static_cast<std::uint64_t>(base) << 19) |
           (static_cast<std::uint64_t>(ide) << 18) |
           static_cast<std::uint64_t>(ext);
  }

  friend constexpr bool operator==(CanId a, CanId b) noexcept {
    return a.packed_ == b.packed_;
  }
  friend constexpr bool operator!=(CanId a, CanId b) noexcept {
    return !(a == b);
  }
  /// Arbitration order, not numeric order.
  friend constexpr bool operator<(CanId a, CanId b) noexcept {
    return a.arbitration_key() < b.arbitration_key();
  }

 private:
  constexpr CanId(std::uint32_t id, bool extended) noexcept
      : packed_(extended ? (id | kEffFlag) : id) {}

  std::uint32_t packed_ = 0;
};

static_assert(std::is_trivially_copyable_v<CanId>);
static_assert(sizeof(CanId) == 4);

enum class FrameFlags : std::uint8_t {
  None = 0,
  Rtr = 1u << 0,    ///< Remote transmission request (classic CAN only).
  Error = 1u << 1,  ///< Error frame delivered by the driver, not a data frame.
  Fd = 1u << 2,     ///< CAN FD format (FDF/EDL bit set).
  Brs = 1u << 3,    ///< Bit rate switch; FD only.
  Esi = 1u << 4,    ///< Error state indicator; FD only.
};

constexpr FrameFlags operator|(FrameFlags a, FrameFlags b) noexcept {
  return static_cast<FrameFlags>(static_cast<std::uint8_t>(a) |
                                 static_cast<std::uint8_t>(b));
}
constexpr FrameFlags operator&(FrameFlags a, FrameFlags b) noexcept {
  return static_cast<FrameFlags>(static_cast<std::uint8_t>(a) &
                                 static_cast<std::uint8_t>(b));
}
constexpr FrameFlags operator~(FrameFlags a) noexcept {
  return static_cast<FrameFlags>(static_cast<std::uint8_t>(
      ~static_cast<unsigned>(static_cast<std::uint8_t>(a)) & 0xFFu));
}
constexpr FrameFlags& operator|=(FrameFlags& a, FrameFlags b) noexcept {
  a = a | b;
  return a;
}
constexpr bool has_flag(FrameFlags set, FrameFlags bit) noexcept {
  return (set & bit) != FrameFlags::None;
}

/// CAN FD data length code table (ISO 11898-1:2015 table 5).
inline constexpr std::array<std::uint8_t, 16> kFdDlcToLength = {
    0, 1, 2, 3, 4, 5, 6, 7, 8, 12, 16, 20, 24, 32, 48, 64};

/// ISO 11898-1 marks classic DLC 9..15 reserved, and a classic controller
/// transmits exactly 8 bytes for any of them -- but real logs contain them.
/// BasicFrame stores the raw DLC so a log round trip is byte-identical, while
/// this reports the true length of 8. SocketCAN clamps and loses the original.
constexpr std::uint8_t dlc_to_length(std::uint8_t dlc, bool fd) noexcept {
  if (dlc > 15u) {
    return 0u;
  }
  if (fd) {
    return kFdDlcToLength[dlc];
  }
  // The ternary promotes to int, so cast back explicitly: -Wconversion is on.
  return dlc > 8u ? std::uint8_t{8} : static_cast<std::uint8_t>(dlc);
}

struct DlcFit {
  std::uint8_t dlc = 0;
  std::uint8_t length = 0;   ///< The length the DLC actually encodes.
  std::uint8_t padding = 0;  ///< length - requested; nonzero only for FD.
};

/// Smallest DLC that can carry `len` bytes. CAN FD has no code for e.g. 9..11,
/// so the length rounds up and the pad count is reported for the encoder.
Result<DlcFit> length_to_dlc(std::size_t len, bool fd) noexcept;

template <std::size_t Capacity>
class BasicFrame {
  static_assert(Capacity == 8 || Capacity == 64,
                "only the classic (8) and CAN FD (64) capacities are meaningful");

 public:
  static constexpr std::size_t capacity = Capacity;
  static constexpr bool supports_fd = (Capacity >= 64);

  constexpr BasicFrame() noexcept = default;

  static Result<BasicFrame> make(CanId id, const std::uint8_t* bytes,
                                 std::size_t len,
                                 FrameFlags flags = FrameFlags::None,
                                 std::uint64_t timestamp_ns = 0) noexcept {
    CANFORGE_CHECK(validate_flags(flags));
    if (has_flag(flags, FrameFlags::Rtr) && len != 0u) {
      return Error(ErrorCode::FrameBadFlags,
                   "a remote frame carries no data bytes");
    }
    if (len > Capacity) {
      return Error(ErrorCode::FramePayloadTooLarge,
                   "payload exceeds the frame capacity",
                   {static_cast<std::uint32_t>(len),
                    static_cast<std::uint32_t>(Capacity)});
    }
    const bool fd = has_flag(flags, FrameFlags::Fd);
    CANFORGE_TRY(const auto fit, length_to_dlc(len, fd));
    if (fit.padding != 0u) {
      return Error(ErrorCode::FrameBadDlc,
                   "CAN FD has no data length code for this size; pad the "
                   "payload to the next encodable length",
                   {static_cast<std::uint32_t>(len),
                    static_cast<std::uint32_t>(fit.length)});
    }
    BasicFrame f;
    f.id_ = id;
    f.dlc_ = fit.dlc;
    f.flags_ = flags;
    f.timestamp_ns_ = timestamp_ns;
    if (len != 0u && bytes != nullptr) {
      std::memcpy(f.data_.data(), bytes, len);
    }
    return f;
  }

  static Result<BasicFrame> make(CanId id, std::initializer_list<std::uint8_t> bytes,
                                 FrameFlags flags = FrameFlags::None,
                                 std::uint64_t timestamp_ns = 0) noexcept {
    return make(id, bytes.begin(), bytes.size(), flags, timestamp_ns);
  }

  static Result<BasicFrame> make_empty(CanId id, std::size_t len,
                                       FrameFlags flags = FrameFlags::None,
                                       std::uint64_t timestamp_ns = 0) noexcept {
    return make(id, nullptr, len, flags, timestamp_ns);
  }

  /// Remote transmission request. The DLC is the *requested* length and no data
  /// bytes are transferred. CAN FD has no remote frames.
  static Result<BasicFrame> make_remote(CanId id, std::uint8_t requested_len,
                                        std::uint64_t timestamp_ns = 0) noexcept {
    if (requested_len > 8u) {
      return Error(ErrorCode::FrameBadDlc,
                   "a remote frame may request at most 8 bytes",
                   {requested_len, 8u});
    }
    BasicFrame f;
    f.id_ = id;
    f.dlc_ = requested_len;
    f.flags_ = FrameFlags::Rtr;
    f.timestamp_ns_ = timestamp_ns;
    return f;
  }

  /// Build from a raw DLC exactly as seen on the wire or in a log. The only
  /// entry point taking a classic DLC of 9..15, preserved verbatim.
  static Result<BasicFrame> from_wire(CanId id, std::uint8_t dlc,
                                      const std::uint8_t* bytes,
                                      FrameFlags flags = FrameFlags::None,
                                      std::uint64_t timestamp_ns = 0) noexcept {
    CANFORGE_CHECK(validate_flags(flags));
    if (dlc > 15u) {
      return Error(ErrorCode::FrameBadDlc, "a data length code has four bits",
                   {dlc, 15u});
    }
    const bool fd = has_flag(flags, FrameFlags::Fd);
    const std::uint8_t len = dlc_to_length(dlc, fd);
    if (len > Capacity) {
      return Error(ErrorCode::FramePayloadTooLarge,
                   "payload exceeds the frame capacity",
                   {len, static_cast<std::uint32_t>(Capacity)});
    }
    BasicFrame f;
    f.id_ = id;
    f.dlc_ = dlc;
    f.flags_ = flags;
    f.timestamp_ns_ = timestamp_ns;
    if (bytes != nullptr && len != 0u) {
      std::memcpy(f.data_.data(), bytes, len);
    }
    return f;
  }

  constexpr CanId id() const noexcept { return id_; }
  constexpr std::uint8_t dlc() const noexcept { return dlc_; }
  constexpr FrameFlags flags() const noexcept { return flags_; }
  constexpr std::uint64_t timestamp_ns() const noexcept { return timestamp_ns_; }

  constexpr bool is_fd() const noexcept { return has_flag(flags_, FrameFlags::Fd); }
  constexpr bool is_remote() const noexcept {
    return has_flag(flags_, FrameFlags::Rtr);
  }
  constexpr bool is_error_frame() const noexcept {
    return has_flag(flags_, FrameFlags::Error);
  }
  constexpr bool is_brs() const noexcept { return has_flag(flags_, FrameFlags::Brs); }
  constexpr bool is_esi() const noexcept { return has_flag(flags_, FrameFlags::Esi); }

  /// Valid payload bytes. Zero for a remote frame.
  constexpr std::size_t size() const noexcept {
    return is_remote() ? std::size_t{0} : dlc_to_length(dlc_, is_fd());
  }

  const std::uint8_t* data() const noexcept { return data_.data(); }
  std::uint8_t* data() noexcept { return data_.data(); }
  constexpr const std::array<std::uint8_t, Capacity>& bytes() const noexcept {
    return data_;
  }
  std::array<std::uint8_t, Capacity>& bytes() noexcept { return data_; }

  void set_timestamp_ns(std::uint64_t ts) noexcept { timestamp_ns_ = ts; }

  friend bool operator==(const BasicFrame& a, const BasicFrame& b) noexcept {
    if (a.id_ != b.id_ || a.dlc_ != b.dlc_ || a.flags_ != b.flags_) {
      return false;
    }
    return std::memcmp(a.data_.data(), b.data_.data(), a.size()) == 0;
  }
  friend bool operator!=(const BasicFrame& a, const BasicFrame& b) noexcept {
    return !(a == b);
  }

  template <std::size_t Other>
  BasicFrame<Other> widen() const noexcept {
    static_assert(Other >= Capacity, "widen() must not lose payload bytes");
    BasicFrame<Other> out;
    out.assign_unchecked(id_, dlc_, flags_, timestamp_ns_, data_.data(), size());
    return out;
  }

  /// Precondition: the wire representation has already been validated.
  void assign_unchecked(CanId id, std::uint8_t dlc, FrameFlags flags,
                        std::uint64_t ts, const std::uint8_t* src,
                        std::size_t len) noexcept {
    id_ = id;
    dlc_ = dlc;
    flags_ = flags;
    timestamp_ns_ = ts;
    if (src != nullptr && len != 0u) {
      std::memcpy(data_.data(), src, len > Capacity ? Capacity : len);
    }
  }

 private:
  static Status validate_flags(FrameFlags flags) noexcept {
    const bool fd = has_flag(flags, FrameFlags::Fd);
    if (fd && !supports_fd) {
      return Error(ErrorCode::FrameBadFlags,
                   "this frame type cannot hold a CAN FD payload");
    }
    // ISO 11898-1:2015 removed the remote frame from CAN FD: the RTR bit
    // position is reused by RRS, which is always dominant.
    if (fd && has_flag(flags, FrameFlags::Rtr)) {
      return Error(ErrorCode::FrameBadFlags,
                   "CAN FD has no remote frames");
    }
    if (!fd && (has_flag(flags, FrameFlags::Brs) ||
                has_flag(flags, FrameFlags::Esi))) {
      return Error(ErrorCode::FrameBadFlags,
                   "BRS and ESI are only defined for CAN FD frames");
    }
    return ok();
  }

  // Ordered so the classic frame is 24 bytes with no interior padding.
  std::uint64_t timestamp_ns_ = 0;
  CanId id_{};
  std::uint8_t dlc_ = 0;
  FrameFlags flags_ = FrameFlags::None;
  std::array<std::uint8_t, Capacity> data_{};
};

using Frame = BasicFrame<8>;      ///< Classic CAN.
using FdFrame = BasicFrame<64>;   ///< CAN FD.

static_assert(std::is_trivially_copyable_v<Frame>,
              "Frame must be trivially copyable so bus queues can memcpy it");
static_assert(std::is_standard_layout_v<Frame>);
static_assert(sizeof(Frame) <= 64,
              "a classic frame must fit inside one cache line");
// At 24 bytes two frames plus a queue header still share a cache line, which is
// what makes the virtual bus cheap.
static_assert(sizeof(Frame) == 24, "unexpected classic frame layout");
static_assert(alignof(Frame) == 8);

static_assert(std::is_trivially_copyable_v<FdFrame>);
static_assert(sizeof(FdFrame) == 80, "unexpected FD frame layout");

}  // namespace canforge::core

#endif  // CANFORGE_CORE_FRAME_HPP
