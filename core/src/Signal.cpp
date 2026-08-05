// SPDX-License-Identifier: MIT
#include "canforge/core/Signal.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace canforge::core {
namespace {

// Canonical bit space
//
// Both byte orders reduce to "a contiguous run of `len` bits starting at
// canonical position q0". The two orders differ only in how a canonical
// position maps back to a (byte, bit) pair:
//
//   Intel     q -> byte q/8, bit q%8          (bit 0 = LSB of the byte)
//   Motorola  q -> byte q/8, bit 7 - (q%8)
//
// which is to say Motorola reads each byte most-significant-bit first. That
// makes an Intel run a little-endian integer over its bytes and a Motorola
// run a big-endian one, so both can be done a word at a time.

/// Number of bytes an aligned run of `len` bits touches when it begins `off`
/// bits into the first byte. Between 1 and 9: a 64-bit signal that starts on a
/// non-zero bit offset straddles nine bytes, which is why the wide cases below
/// exist and why they are exercised by the FD golden vectors.
constexpr std::size_t spanned_bytes(std::size_t off, std::uint8_t len) noexcept {
  return (off + len + 7u) / 8u;
}

std::uint64_t load_le(const std::uint8_t* p, std::size_t n) noexcept {
  std::uint64_t acc = 0;
  for (std::size_t i = 0; i < n; ++i) {
    acc |= static_cast<std::uint64_t>(p[i]) << (8u * i);
  }
  return acc;
}

void store_le(std::uint8_t* p, std::size_t n, std::uint64_t v) noexcept {
  for (std::size_t i = 0; i < n; ++i) {
    p[i] = static_cast<std::uint8_t>((v >> (8u * i)) & 0xFFu);
  }
}

std::uint64_t load_be(const std::uint8_t* p, std::size_t n) noexcept {
  std::uint64_t acc = 0;
  for (std::size_t i = 0; i < n; ++i) {
    acc = (acc << 8u) | static_cast<std::uint64_t>(p[i]);
  }
  return acc;
}

void store_be(std::uint8_t* p, std::size_t n, std::uint64_t v) noexcept {
  for (std::size_t i = 0; i < n; ++i) {
    p[i] = static_cast<std::uint8_t>((v >> (8u * (n - 1u - i))) & 0xFFu);
  }
}

std::uint64_t extract_intel(const std::uint8_t* data, std::size_t q0,
                            std::uint8_t len) noexcept {
  const std::size_t first = q0 / 8u;
  const std::size_t off = q0 % 8u;
  const std::size_t nb = spanned_bytes(off, len);
  const std::size_t head = std::min<std::size_t>(nb, 8u);

  std::uint64_t raw = load_le(data + first, head) >> off;
  if (nb == 9u) {
    // off > 0 is guaranteed here: nine bytes are only touched when
    // off + len > 64 and len <= 64.
    raw |= static_cast<std::uint64_t>(data[first + 8u]) << (64u - off);
  }
  return raw & bit_mask(len);
}

void insert_intel(std::uint8_t* data, std::size_t q0, std::uint8_t len,
                  std::uint64_t value) noexcept {
  const std::size_t first = q0 / 8u;
  const std::size_t off = q0 % 8u;
  const std::size_t nb = spanned_bytes(off, len);
  const std::size_t head = std::min<std::size_t>(nb, 8u);
  const std::uint64_t v = value & bit_mask(len);

  // Bits of the window that live in the first eight bytes. When the signal
  // spills into a ninth byte the high bits simply fall off this mask, which is
  // exactly right -- they are handled below.
  const std::uint64_t window = bit_mask(len) << off;
  const std::uint64_t acc = load_le(data + first, head);
  store_le(data + first, head, (acc & ~window) | ((v << off) & window));

  if (nb == 9u) {
    const std::size_t tail = off + len - 64u;  // 1..7
    const auto tail_mask = static_cast<std::uint8_t>((1u << tail) - 1u);
    const auto tail_bits =
        static_cast<std::uint8_t>((v >> (len - tail)) & tail_mask);
    data[first + 8u] = static_cast<std::uint8_t>(
        (data[first + 8u] & static_cast<std::uint8_t>(~tail_mask)) | tail_bits);
  }
}

std::uint64_t extract_motorola(const std::uint8_t* data, std::size_t q0,
                               std::uint8_t len) noexcept {
  const std::size_t first = q0 / 8u;
  const std::size_t off = q0 % 8u;
  const std::size_t nb = spanned_bytes(off, len);

  if (nb <= 8u) {
    const std::uint64_t acc = load_be(data + first, nb);
    const std::size_t shift = nb * 8u - off - len;
    return (acc >> shift) & bit_mask(len);
  }
  // Nine bytes. The first eight hold `off` skipped bits followed by the top
  // (len - tail) bits of the signal; the ninth holds the remaining `tail`
  // bits in its most significant positions. Note tail <= off, so shifting acc
  // left by tail only discards bits that were being skipped anyway.
  const std::size_t tail = off + len - 64u;  // 1..7
  const std::uint64_t acc = load_be(data + first, 8u);
  const std::uint64_t last = data[first + 8u];
  return ((acc << tail) | (last >> (8u - tail))) & bit_mask(len);
}

void insert_motorola(std::uint8_t* data, std::size_t q0, std::uint8_t len,
                     std::uint64_t value) noexcept {
  const std::size_t first = q0 / 8u;
  const std::size_t off = q0 % 8u;
  const std::size_t nb = spanned_bytes(off, len);
  const std::uint64_t v = value & bit_mask(len);

  if (nb <= 8u) {
    const std::size_t shift = nb * 8u - off - len;
    const std::uint64_t window = bit_mask(len) << shift;
    const std::uint64_t acc = load_be(data + first, nb);
    store_be(data + first, nb, (acc & ~window) | ((v << shift) & window));
    return;
  }
  const std::size_t tail = off + len - 64u;  // 1..7
  const auto head_bits = static_cast<std::uint8_t>(len - tail);  // 1..63
  const std::uint64_t window = bit_mask(head_bits);
  const std::uint64_t acc = load_be(data + first, 8u);
  store_be(data + first, 8u, (acc & ~window) | ((v >> tail) & window));

  const auto tail_mask = static_cast<std::uint8_t>(0xFFu << (8u - tail));
  const auto tail_bits =
      static_cast<std::uint8_t>((v << (8u - tail)) & tail_mask);
  data[first + 8u] = static_cast<std::uint8_t>(
      (data[first + 8u] & static_cast<std::uint8_t>(~tail_mask)) | tail_bits);
}

struct BitAddress {
  std::size_t byte;
  std::uint8_t bit;
};

constexpr BitAddress address_of(std::size_t q, ByteOrder order) noexcept {
  const std::size_t byte = q / 8u;
  const auto within = static_cast<std::uint8_t>(q % 8u);
  return order == ByteOrder::Intel
             ? BitAddress{byte, within}
             : BitAddress{byte, static_cast<std::uint8_t>(7u - within)};
}

}  // namespace

std::uint64_t SignalLayout::decode_raw(const std::uint8_t* data) const noexcept {
  const std::size_t q0 = canonical_start();
  return byte_order == ByteOrder::Intel ? extract_intel(data, q0, bit_length)
                                        : extract_motorola(data, q0, bit_length);
}

void SignalLayout::encode_raw(std::uint64_t raw, std::uint8_t* data) const noexcept {
  const std::size_t q0 = canonical_start();
  if (byte_order == ByteOrder::Intel) {
    insert_intel(data, q0, bit_length, raw);
  } else {
    insert_motorola(data, q0, bit_length, raw);
  }
}

std::uint64_t SignalLayout::decode_raw_reference(
    const std::uint8_t* data) const noexcept {
  const std::size_t q0 = canonical_start();
  std::uint64_t raw = 0;
  for (std::uint8_t k = 0; k < bit_length; ++k) {
    const BitAddress a = address_of(q0 + k, byte_order);
    const std::uint64_t bit = (data[a.byte] >> a.bit) & 1u;
    if (byte_order == ByteOrder::Intel) {
      // Intel ascends from the least significant bit of the signal.
      raw |= bit << k;
    } else {
      // Motorola ascends from the most significant bit of the signal.
      raw = (raw << 1u) | bit;
    }
  }
  return raw;
}

void SignalLayout::encode_raw_reference(std::uint64_t raw,
                                        std::uint8_t* data) const noexcept {
  const std::size_t q0 = canonical_start();
  const std::uint64_t v = raw & bit_mask(bit_length);
  for (std::uint8_t k = 0; k < bit_length; ++k) {
    const BitAddress a = address_of(q0 + k, byte_order);
    const std::uint8_t shift =
        byte_order == ByteOrder::Intel
            ? k
            : static_cast<std::uint8_t>(bit_length - 1u - k);
    const std::uint64_t bit = (v >> shift) & 1u;
    const auto mask = static_cast<std::uint8_t>(1u << a.bit);
    data[a.byte] = static_cast<std::uint8_t>(
        (data[a.byte] & static_cast<std::uint8_t>(~mask)) |
        (bit != 0u ? mask : std::uint8_t{0}));
  }
}

double SignalLayout::raw_to_physical(std::uint64_t raw) const noexcept {
  double value = 0.0;
  switch (value_type) {
    case ValueType::Float32: {
      const auto bits = static_cast<std::uint32_t>(raw & 0xFFFFFFFFu);
      float f = 0.0F;
      std::memcpy(&f, &bits, sizeof(f));
      value = static_cast<double>(f);
      break;
    }
    case ValueType::Float64: {
      double d = 0.0;
      std::memcpy(&d, &raw, sizeof(d));
      value = d;
      break;
    }
    case ValueType::Integer:
    default:
      value = signedness == Signedness::Signed
                  ? static_cast<double>(sign_extend(raw, bit_length))
                  : static_cast<double>(raw);
      break;
  }
  return value * factor + offset;
}

std::uint64_t SignalLayout::physical_to_raw(double physical) const noexcept {
  double v = physical;
  // A declared [min|max] saturates rather than wraps. NaN has no order, so it
  // is excluded from the clamp; the integer path below maps it to zero and the
  // float path passes it through, since a float signal can carry a NaN.
  if (has_range() && !std::isnan(v)) {
    const double lo = std::min(minimum, maximum);
    const double hi = std::max(minimum, maximum);
    v = std::min(std::max(v, lo), hi);
  }
  const double scaled = (v - offset) / factor;

  switch (value_type) {
    case ValueType::Float32: {
      const auto f = static_cast<float>(scaled);
      std::uint32_t bits = 0;
      std::memcpy(&bits, &f, sizeof(bits));
      return bits;
    }
    case ValueType::Float64: {
      std::uint64_t bits = 0;
      std::memcpy(&bits, &scaled, sizeof(bits));
      return bits;
    }
    case ValueType::Integer:
    default:
      break;
  }

  // Round half away from zero. That is what CANoe and CANalyzer do; std::round
  // has exactly those semantics, unlike std::nearbyint, which follows the
  // current rounding mode and therefore rounds halves to even by default.
  double rounded = std::round(scaled);
  if (std::isnan(rounded)) {
    rounded = 0.0;
  }

  // Saturate to what the bit width can hold. The bounds are computed as bit
  // patterns rather than by casting a double, so that a 64-bit signal -- whose
  // extremes are not exactly representable as a double -- still cannot produce
  // an out-of-range floating point to integer conversion, which is UB.
  std::uint64_t out = 0;
  const std::uint64_t mask = bit_mask(bit_length);
  if (signedness == Signedness::Signed) {
    const double top = std::ldexp(1.0, bit_length - 1);  // 2^(len-1)
    if (rounded >= top) {
      out = mask >> 1u;  // 2^(len-1) - 1
    } else if (rounded < -top) {
      out = bit_length >= 64u ? (std::uint64_t{1} << 63u)
                              : (std::uint64_t{1} << (bit_length - 1u));
    } else {
      out = static_cast<std::uint64_t>(static_cast<std::int64_t>(rounded));
    }
  } else {
    const double top = std::ldexp(1.0, bit_length);  // 2^len
    if (rounded <= 0.0) {
      out = 0;
    } else if (rounded >= top) {
      out = mask;
    } else {
      out = static_cast<std::uint64_t>(rounded);
    }
  }
  return out & mask;
}

double SignalLayout::decode(const std::uint8_t* data) const noexcept {
  return raw_to_physical(decode_raw(data));
}

Status SignalLayout::encode(double physical, std::uint8_t* data) const noexcept {
  if (!std::isfinite(physical) && value_type == ValueType::Integer) {
    return Error(ErrorCode::CodecValueNotFinite,
                 "cannot encode a non-finite value into an integer signal",
                 {start_bit, bit_length});
  }
  encode_raw(physical_to_raw(physical), data);
  return ok();
}

double SignalLayout::raw_minimum() const noexcept {
  switch (value_type) {
    case ValueType::Float32:
      return static_cast<double>(-std::numeric_limits<float>::max());
    case ValueType::Float64:
      return -std::numeric_limits<double>::max();
    case ValueType::Integer:
    default:
      return signedness == Signedness::Signed
                 ? -std::ldexp(1.0, bit_length - 1)
                 : 0.0;
  }
}

double SignalLayout::raw_maximum() const noexcept {
  switch (value_type) {
    case ValueType::Float32:
      return static_cast<double>(std::numeric_limits<float>::max());
    case ValueType::Float64:
      return std::numeric_limits<double>::max();
    case ValueType::Integer:
    default:
      return signedness == Signedness::Signed
                 ? std::ldexp(1.0, bit_length - 1) - 1.0
                 : std::ldexp(1.0, bit_length) - 1.0;
  }
}

double SignalLayout::physical_floor() const noexcept {
  const double a = raw_minimum() * factor + offset;
  const double b = raw_maximum() * factor + offset;
  return std::min(a, b);  // a negative factor swaps the ends
}

double SignalLayout::physical_ceiling() const noexcept {
  const double a = raw_minimum() * factor + offset;
  const double b = raw_maximum() * factor + offset;
  return std::max(a, b);
}

double SignalLayout::resolution() const noexcept { return std::fabs(factor); }

Status SignalLayout::validate(std::size_t payload_bytes) const noexcept {
  if (bit_length < 1u || bit_length > 64u) {
    return Error(ErrorCode::CodecBadBitLength,
                 "a signal must be between 1 and 64 bits wide",
                 {start_bit, bit_length});
  }
  if (factor == 0.0 || !std::isfinite(factor)) {
    return Error(ErrorCode::CodecBadFactor,
                 "a signal scale factor must be finite and non-zero",
                 {start_bit, bit_length});
  }
  if (!std::isfinite(offset)) {
    return Error(ErrorCode::CodecBadFactor, "a signal offset must be finite",
                 {start_bit, bit_length});
  }
  if (value_type == ValueType::Float32 && bit_length != 32u) {
    return Error(ErrorCode::CodecBadBitLength,
                 "an IEEE single precision signal must be 32 bits wide",
                 {start_bit, bit_length});
  }
  if (value_type == ValueType::Float64 && bit_length != 64u) {
    return Error(ErrorCode::CodecBadBitLength,
                 "an IEEE double precision signal must be 64 bits wide",
                 {start_bit, bit_length});
  }
  if (canonical_end() > payload_bytes * 8u) {
    return Error(ErrorCode::CodecSignalOutOfBounds,
                 "signal extends past the end of the payload",
                 {start_bit, bit_length});
  }
  return ok();
}

bool operator==(const SignalLayout& a, const SignalLayout& b) noexcept {
  return a.start_bit == b.start_bit && a.bit_length == b.bit_length &&
         a.byte_order == b.byte_order && a.signedness == b.signedness &&
         a.value_type == b.value_type && a.factor == b.factor &&
         a.offset == b.offset && a.minimum == b.minimum && a.maximum == b.maximum;
}

bool Signal::is_present_for(std::uint32_t mux) const noexcept {
  if (mux_role_ != MultiplexRole::Multiplexed &&
      mux_role_ != MultiplexRole::Both) {
    return true;
  }
  if (!mux_ranges_.empty()) {
    for (const MuxRange& r : mux_ranges_) {
      if (r.contains(mux)) {
        return true;
      }
    }
    return false;
  }
  return mux_value_ == mux;
}

std::string_view Signal::describe_raw(std::int64_t raw) const noexcept {
  for (const ValueDescription& v : value_descriptions_) {
    if (v.value == raw) {
      return v.text;
    }
  }
  return {};
}

std::string_view Signal::describe(double physical) const noexcept {
  if (value_descriptions_.empty() || !std::isfinite(physical)) {
    return {};
  }
  // A VAL_ table keys on the *raw* signal value, not the scaled one. For the
  // overwhelmingly common enum case (factor 1, offset 0) the two coincide, but
  // a table on a scaled signal only resolves correctly if the physical value is
  // converted back first.
  //
  // Deliberately not physical_to_raw(): that saturates to the widest value the
  // bit field can hold, which would make a lookup for an out-of-range value
  // silently resolve to the topmost table entry instead of reporting no match.
  const double rounded = std::round((physical - layout_.offset) / layout_.factor);
  if (!std::isfinite(rounded) || rounded < layout_.raw_minimum() ||
      rounded > layout_.raw_maximum() ||
      std::fabs(rounded) >= 9.223372036854775808e18) {
    return {};
  }
  return describe_raw(static_cast<std::int64_t>(rounded));
}

bool operator==(const Signal& a, const Signal& b) {
  return a.name_ == b.name_ && a.unit_ == b.unit_ && a.comment_ == b.comment_ &&
         a.multiplexor_name_ == b.multiplexor_name_ &&
         a.receivers_ == b.receivers_ && a.mux_ranges_ == b.mux_ranges_ &&
         a.value_descriptions_ == b.value_descriptions_ &&
         a.attributes_ == b.attributes_ && a.layout_ == b.layout_ && a.mux_role_ == b.mux_role_ &&
         a.mux_value_ == b.mux_value_;
}

}  // namespace canforge::core
