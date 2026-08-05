// SPDX-License-Identifier: MIT
#ifndef CANFORGE_CORE_SIGNAL_HPP
#define CANFORGE_CORE_SIGNAL_HPP

/// Signal definition and the bit-exact codec. Everything in SignalLayout is
/// allocation-free and exception-free; test_no_alloc.cpp proves it. The
/// canonical bit space both byte orders reduce to is described in Signal.cpp.

#include <cstddef>
#include <cstdint>
#include <string>
#include <type_traits>
#include <vector>

#include "canforge/core/Attribute.hpp"
#include "canforge/core/Frame.hpp"
#include "canforge/core/Result.hpp"

namespace canforge::core {

/// Low `len` bits set. `len` must be in 1..64.
constexpr std::uint64_t bit_mask(std::uint8_t len) noexcept {
  // Shifting a 64-bit value by 64 is undefined, so the full-width case is
  // spelled out instead.
  return len >= 64u ? ~std::uint64_t{0}
                    : ((std::uint64_t{1} << len) - std::uint64_t{1});
}

constexpr std::int64_t sign_extend(std::uint64_t raw, std::uint8_t len) noexcept {
  if (len >= 64u) {
    return static_cast<std::int64_t>(raw);
  }
  const std::uint64_t sign_bit = std::uint64_t{1} << (len - 1u);
  if ((raw & sign_bit) != 0u) {
    raw |= ~bit_mask(len);
  }
  // Conversion of an out-of-range unsigned to a signed type is well defined
  // as the two's-complement bit pattern from C++20 on, and is the documented
  // behaviour of every compiler canforge targets before that.
  return static_cast<std::int64_t>(raw);
}

/// Matches the DBC spelling: `@0` is Motorola, `@1` is Intel.
enum class ByteOrder : std::uint8_t { Motorola = 0, Intel = 1 };

/// Matches the DBC spelling: `-` is signed, `+` is unsigned.
enum class Signedness : std::uint8_t { Unsigned = 0, Signed = 1 };

/// Set by `SIG_VALTYPE_`. Integer is the default.
enum class ValueType : std::uint8_t { Integer = 0, Float32 = 1, Float64 = 2 };

enum class MultiplexRole : std::uint8_t {
  None = 0,        ///< Always present.
  Multiplexor,     ///< `M` -- selects which multiplexed signals are present.
  Multiplexed,     ///< `m<n>` -- present only for specific multiplexor values.
  /// m<n>M -- switched in by an outer multiplexor and itself switching an inner
  /// group. Only SG_MUL_VAL_ can describe this fully.
  Both,
};

/// A closed range of multiplexor values, from `SG_MUL_VAL_`.
struct MuxRange {
  std::uint32_t low = 0;
  std::uint32_t high = 0;
  constexpr bool contains(std::uint32_t v) const noexcept {
    return v >= low && v <= high;
  }
  friend constexpr bool operator==(MuxRange a, MuxRange b) noexcept {
    return a.low == b.low && a.high == b.high;
  }
};

struct ValueDescription {
  std::int64_t value = 0;
  std::string text;
  friend bool operator==(const ValueDescription& a, const ValueDescription& b) {
    return a.value == b.value && a.text == b.text;
  }
};

/// Trivially copyable, so it can be captured by value in a hot loop.
struct SignalLayout {
  std::uint16_t start_bit = 0;
  std::uint8_t bit_length = 1;
  ByteOrder byte_order = ByteOrder::Intel;
  Signedness signedness = Signedness::Unsigned;
  ValueType value_type = ValueType::Integer;
  double factor = 1.0;
  double offset = 0.0;
  double minimum = 0.0;
  double maximum = 0.0;

  /// DBC writes [0|0] when no range is given, so an all-zero range means
  /// unbounded, not "the value must be exactly zero".
  constexpr bool has_range() const noexcept {
    return minimum != 0.0 || maximum != 0.0;
  }

  /// First bit of the signal in canonical space. Intel starts at the LSB and
  /// Motorola at the MSB, but both runs ascend -- that is the whole point.
  constexpr std::size_t canonical_start() const noexcept {
    if (byte_order == ByteOrder::Intel) {
      return start_bit;
    }
    const std::size_t byte = start_bit / 8u;
    const std::size_t bit = start_bit % 8u;
    return byte * 8u + (7u - bit);
  }

  /// One past the last canonical bit position the signal touches.
  constexpr std::size_t canonical_end() const noexcept {
    return canonical_start() + bit_length;
  }

  constexpr bool fits(std::size_t payload_bytes) const noexcept {
    return bit_length >= 1u && bit_length <= 64u &&
           canonical_end() <= payload_bytes * 8u;
  }

  /// Full check, done once when a database is built, never on the hot path.
  Status validate(std::size_t payload_bytes) const noexcept;

  // Precondition for all four: fits(payload_bytes) for the buffer passed.
  // Checked by validate() at database build time, deliberately not re-checked
  // here so the hot path stays branch-free.

  std::uint64_t decode_raw(const std::uint8_t* data) const noexcept;

  void encode_raw(std::uint64_t raw, std::uint8_t* data) const noexcept;

  /// One bit at a time, straight from the definition. Kept as an executable
  /// spec: the property test uses it as the oracle for the fast path above.
  std::uint64_t decode_raw_reference(const std::uint8_t* data) const noexcept;
  void encode_raw_reference(std::uint64_t raw, std::uint8_t* data) const noexcept;

  /// raw -> physical: sign extension or float reinterpretation, then
  /// physical = raw * factor + offset.
  double raw_to_physical(std::uint64_t raw) const noexcept;

  /// physical -> raw: the inverse, with saturation to [min, max] when a range is
  /// declared, round-half-away-from-zero, and a clamp to what the width holds.
  std::uint64_t physical_to_raw(double physical) const noexcept;

  double decode(const std::uint8_t* data) const noexcept;

  /// Fails only on a non-finite input, which cannot be represented at all.
  Status encode(double physical, std::uint8_t* data) const noexcept;

  /// Raw range as doubles. Exact to 2^53; the extremes of a 64-bit signal round,
  /// which is why physical_to_raw re-checks the clamp in the integer domain.
  double raw_minimum() const noexcept;
  double raw_maximum() const noexcept;

  /// Physical range the encoding can reach, ignoring any declared [min|max].
  /// Accounts for a negative factor, which swaps the two ends.
  double physical_floor() const noexcept;
  double physical_ceiling() const noexcept;

  /// Quantisation step; equals |factor| for an integer signal.
  double resolution() const noexcept;

  friend bool operator==(const SignalLayout& a, const SignalLayout& b) noexcept;
  friend bool operator!=(const SignalLayout& a, const SignalLayout& b) noexcept {
    return !(a == b);
  }
};

static_assert(std::is_trivially_copyable_v<SignalLayout>,
              "the codec input must be copyable into a real-time context");

class Signal {
 public:
  Signal() = default;
  Signal(std::string name, SignalLayout layout)
      : name_(std::move(name)), layout_(layout) {}

  const std::string& name() const noexcept { return name_; }
  const std::string& unit() const noexcept { return unit_; }
  const std::string& comment() const noexcept { return comment_; }
  const std::vector<std::string>& receivers() const noexcept { return receivers_; }
  const SignalLayout& layout() const noexcept { return layout_; }
  SignalLayout& layout() noexcept { return layout_; }

  MultiplexRole multiplex_role() const noexcept { return mux_role_; }
  std::uint32_t multiplex_value() const noexcept { return mux_value_; }
  /// Non-empty only for `SG_MUL_VAL_` extended multiplexing.
  const std::vector<MuxRange>& multiplex_ranges() const noexcept { return mux_ranges_; }
  /// Multiplexor this signal is switched by; empty for simple multiplexing,
  /// where the message's single multiplexor is implied.
  const std::string& multiplexor_name() const noexcept { return multiplexor_name_; }

  const std::vector<ValueDescription>& value_descriptions() const noexcept {
    return value_descriptions_;
  }
  const AttributeMap& attributes() const noexcept { return attributes_; }
  AttributeMap& attributes() noexcept { return attributes_; }

  void set_name(std::string v) { name_ = std::move(v); }
  void set_unit(std::string v) { unit_ = std::move(v); }
  void set_comment(std::string v) { comment_ = std::move(v); }
  void set_receivers(std::vector<std::string> v) { receivers_ = std::move(v); }
  void set_layout(SignalLayout v) noexcept { layout_ = v; }
  void set_multiplex_role(MultiplexRole r) noexcept { mux_role_ = r; }
  void set_multiplex_value(std::uint32_t v) noexcept { mux_value_ = v; }
  void set_multiplex_ranges(std::vector<MuxRange> v) { mux_ranges_ = std::move(v); }
  void set_multiplexor_name(std::string v) { multiplexor_name_ = std::move(v); }
  void set_value_descriptions(std::vector<ValueDescription> v) {
    value_descriptions_ = std::move(v);
  }

  bool is_present_for(std::uint32_t mux) const noexcept;

  /// Precondition: layout().fits(frame.size()). Use try_decode() when the frame
  /// comes from an untrusted source.
  template <std::size_t Cap>
  double decode(const BasicFrame<Cap>& frame) const noexcept {
    return layout_.decode(frame.data());
  }

  template <std::size_t Cap>
  Result<double> try_decode(const BasicFrame<Cap>& frame) const noexcept {
    if (!layout_.fits(frame.size())) {
      return Error(ErrorCode::CodecSignalOutOfBounds,
                   "signal does not fit in the received payload",
                   {layout_.start_bit, layout_.bit_length});
    }
    return layout_.decode(frame.data());
  }

  template <std::size_t Cap>
  Status encode(double physical, BasicFrame<Cap>& frame) const noexcept {
    if (!layout_.fits(frame.size())) {
      return Error(ErrorCode::CodecSignalOutOfBounds,
                   "signal does not fit in the frame payload",
                   {layout_.start_bit, layout_.bit_length});
    }
    return layout_.encode(physical, frame.data());
  }

  /// VAL_ text for a raw value, or an empty view when there is no description.
  std::string_view describe_raw(std::int64_t raw) const noexcept;

  std::string_view describe(double physical) const noexcept;

  friend bool operator==(const Signal& a, const Signal& b);
  friend bool operator!=(const Signal& a, const Signal& b) { return !(a == b); }

 private:
  std::string name_;
  std::string unit_;
  std::string comment_;
  std::string multiplexor_name_;
  std::vector<std::string> receivers_;
  std::vector<MuxRange> mux_ranges_;
  std::vector<ValueDescription> value_descriptions_;
  AttributeMap attributes_;
  SignalLayout layout_{};
  MultiplexRole mux_role_ = MultiplexRole::None;
  std::uint32_t mux_value_ = 0;
};

}  // namespace canforge::core

#endif  // CANFORGE_CORE_SIGNAL_HPP
