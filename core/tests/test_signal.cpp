// SPDX-License-Identifier: MIT
#include "canforge/core/Signal.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <limits>

namespace canforge::core {
namespace {

SignalLayout make(std::uint16_t start, std::uint8_t len, ByteOrder order,
                  Signedness sign = Signedness::Unsigned, double factor = 1.0,
                  double offset = 0.0) {
  SignalLayout s;
  s.start_bit = start;
  s.bit_length = len;
  s.byte_order = order;
  s.signedness = sign;
  s.factor = factor;
  s.offset = offset;
  return s;
}

TEST(BitMask, CoversEveryWidth) {
  EXPECT_EQ(bit_mask(1), 0x1u);
  EXPECT_EQ(bit_mask(8), 0xFFu);
  EXPECT_EQ(bit_mask(32), 0xFFFFFFFFu);
  EXPECT_EQ(bit_mask(63), 0x7FFFFFFFFFFFFFFFULL);
  EXPECT_EQ(bit_mask(64), ~std::uint64_t{0});
}

TEST(SignExtend, EveryWidth) {
  for (std::uint8_t len = 1; len <= 64; ++len) {
    // All-ones at any width is -1.
    EXPECT_EQ(sign_extend(bit_mask(len), len), -1) << "len " << unsigned{len};
    EXPECT_EQ(sign_extend(0, len), 0);
    // The sign bit alone is the most negative value.
    const std::uint64_t sign_only =
        len >= 64u ? (std::uint64_t{1} << 63u) : (std::uint64_t{1} << (len - 1u));
    const std::int64_t expect_min = len >= 64u
                                        ? std::numeric_limits<std::int64_t>::min()
                                        : -(std::int64_t{1} << (len - 1u));
    EXPECT_EQ(sign_extend(sign_only, len), expect_min) << "len " << unsigned{len};
  }
}

TEST(CanonicalStart, IntelIsIdentityAndMotorolaMirrorsWithinTheByte) {
  for (std::uint16_t bit = 0; bit < 64u; ++bit) {
    EXPECT_EQ(make(bit, 1, ByteOrder::Intel).canonical_start(), bit);
    const std::size_t expected = (bit / 8u) * 8u + (7u - (bit % 8u));
    EXPECT_EQ(make(bit, 1, ByteOrder::Motorola).canonical_start(), expected)
        << "bit " << bit;
  }
  // The mirror is an involution: applying it twice returns the start bit.
  for (std::uint16_t bit = 0; bit < 64u; ++bit) {
    const auto once =
        static_cast<std::uint16_t>(make(bit, 1, ByteOrder::Motorola).canonical_start());
    EXPECT_EQ(make(once, 1, ByteOrder::Motorola).canonical_start(), bit);
  }
}

TEST(Validate, RejectsBadWidths) {
  EXPECT_EQ(make(0, 0, ByteOrder::Intel).validate(8).error().code(),
            ErrorCode::CodecBadBitLength);
  SignalLayout wide = make(0, 8, ByteOrder::Intel);
  wide.bit_length = 65;
  EXPECT_EQ(wide.validate(64).error().code(), ErrorCode::CodecBadBitLength);
}

TEST(Validate, RejectsBadFactors) {
  EXPECT_EQ(make(0, 8, ByteOrder::Intel, Signedness::Unsigned, 0.0)
                .validate(8)
                .error()
                .code(),
            ErrorCode::CodecBadFactor);
  auto nan_factor = make(0, 8, ByteOrder::Intel);
  nan_factor.factor = std::numeric_limits<double>::quiet_NaN();
  EXPECT_EQ(nan_factor.validate(8).error().code(), ErrorCode::CodecBadFactor);
  auto inf_offset = make(0, 8, ByteOrder::Intel);
  inf_offset.offset = std::numeric_limits<double>::infinity();
  EXPECT_EQ(inf_offset.validate(8).error().code(), ErrorCode::CodecBadFactor);
}

TEST(Validate, FloatSignalsHaveFixedWidths) {
  auto f = make(0, 16, ByteOrder::Intel);
  f.value_type = ValueType::Float32;
  EXPECT_EQ(f.validate(8).error().code(), ErrorCode::CodecBadBitLength);
  f.bit_length = 32;
  EXPECT_TRUE(f.validate(8).has_value());

  auto d = make(0, 32, ByteOrder::Intel);
  d.value_type = ValueType::Float64;
  EXPECT_FALSE(d.validate(8).has_value());
  d.bit_length = 64;
  EXPECT_TRUE(d.validate(8).has_value());
}

TEST(Validate, ErrorDetailCarriesTheSignalPosition) {
  const auto s = make(60, 16, ByteOrder::Intel);
  const auto err = s.validate(8).error();
  EXPECT_EQ(err.code(), ErrorCode::CodecSignalOutOfBounds);
  EXPECT_EQ(err.detail().a, 60u);
  EXPECT_EQ(err.detail().b, 16u);
}

TEST(Range, ZeroZeroMeansUnbounded) {
  // DBC writes [0|0] when no range was specified. Treating that literally
  // would make every such signal encode as zero, which a naive
  // implementation does and is always wrong.
  auto s = make(0, 8, ByteOrder::Intel);
  EXPECT_FALSE(s.has_range());
  std::array<std::uint8_t, 8> payload{};
  ASSERT_TRUE(s.encode(200.0, payload.data()).has_value());
  EXPECT_DOUBLE_EQ(s.decode(payload.data()), 200.0);

  s.minimum = 0.0;
  s.maximum = 100.0;
  EXPECT_TRUE(s.has_range());
  ASSERT_TRUE(s.encode(200.0, payload.data()).has_value());
  EXPECT_DOUBLE_EQ(s.decode(payload.data()), 100.0);
}

TEST(Range, PhysicalBoundsAccountForNegativeFactors) {
  const auto up = make(0, 8, ByteOrder::Intel, Signedness::Unsigned, 2.0, 10.0);
  EXPECT_DOUBLE_EQ(up.physical_floor(), 10.0);
  EXPECT_DOUBLE_EQ(up.physical_ceiling(), 255.0 * 2.0 + 10.0);

  const auto down = make(0, 8, ByteOrder::Intel, Signedness::Unsigned, -2.0, 10.0);
  EXPECT_DOUBLE_EQ(down.physical_floor(), 255.0 * -2.0 + 10.0);
  EXPECT_DOUBLE_EQ(down.physical_ceiling(), 10.0);
  EXPECT_DOUBLE_EQ(down.resolution(), 2.0);
}

TEST(Range, NegativeFactorRoundTrips) {
  const auto s = make(0, 8, ByteOrder::Intel, Signedness::Unsigned, -0.5, 100.0);
  std::array<std::uint8_t, 8> payload{};
  for (int raw = 0; raw <= 255; ++raw) {
    const double physical = static_cast<double>(raw) * -0.5 + 100.0;
    ASSERT_TRUE(s.encode(physical, payload.data()).has_value());
    EXPECT_DOUBLE_EQ(s.decode(payload.data()), physical) << "raw " << raw;
  }
}

TEST(Signal, DecodeAndEncodeThroughAFrame) {
  Signal s("EngineSpeed",
           make(24, 16, ByteOrder::Intel, Signedness::Unsigned, 0.125, 0.0));
  s.set_unit("rpm");

  // J1939 EEC1, PGN 61444, source address 0x00.
  constexpr CanId kEec1 = CanId::make_extended<0x0CF00400>();
  auto frame = Frame::make(kEec1, {0, 0, 0, 0, 0, 0, 0, 0}).value();
  ASSERT_TRUE(s.encode(1500.0, frame).has_value());
  EXPECT_DOUBLE_EQ(s.decode(frame), 1500.0);
  EXPECT_EQ(frame.data()[3], 0xE0);
  EXPECT_EQ(frame.data()[4], 0x2E);
  EXPECT_EQ(s.unit(), "rpm");
}

TEST(Signal, TryDecodeRejectsAShortFrame) {
  const Signal s("Wide", make(0, 32, ByteOrder::Intel));
  const auto short_frame = Frame::make(CanId{}, {1, 2}).value();
  const auto r = s.try_decode(short_frame);
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error().code(), ErrorCode::CodecSignalOutOfBounds);

  const auto full = Frame::make(CanId{}, {1, 2, 3, 4}).value();
  EXPECT_TRUE(s.try_decode(full).has_value());
}

TEST(Signal, EncodeRejectsAShortFrame) {
  const Signal s("Wide", make(0, 32, ByteOrder::Intel));
  auto short_frame = Frame::make(CanId{}, {1, 2}).value();
  EXPECT_FALSE(s.encode(1.0, short_frame).has_value());
}

// Multiplexing

TEST(Signal, PlainSignalsAreAlwaysPresent) {
  const Signal s("Always", make(0, 8, ByteOrder::Intel));
  EXPECT_TRUE(s.is_present_for(0));
  EXPECT_TRUE(s.is_present_for(7));
  EXPECT_TRUE(s.is_present_for(0xFFFFFFFFu));
}

TEST(Signal, MultiplexorIsAlwaysPresent) {
  Signal s("Mux", make(0, 4, ByteOrder::Intel));
  s.set_multiplex_role(MultiplexRole::Multiplexor);
  EXPECT_TRUE(s.is_present_for(0));
  EXPECT_TRUE(s.is_present_for(3));
}

TEST(Signal, SimpleMultiplexingMatchesOneValue) {
  Signal s("OnlyForTwo", make(8, 8, ByteOrder::Intel));
  s.set_multiplex_role(MultiplexRole::Multiplexed);
  s.set_multiplex_value(2);
  EXPECT_FALSE(s.is_present_for(0));
  EXPECT_FALSE(s.is_present_for(1));
  EXPECT_TRUE(s.is_present_for(2));
  EXPECT_FALSE(s.is_present_for(3));
}

TEST(Signal, ExtendedMultiplexingUsesRanges) {
  Signal s("Ranged", make(8, 8, ByteOrder::Intel));
  s.set_multiplex_role(MultiplexRole::Multiplexed);
  s.set_multiplex_value(2);  // must be ignored once ranges are present
  s.set_multiplex_ranges({{0, 2}, {5, 5}, {10, 12}});
  EXPECT_TRUE(s.is_present_for(0));
  EXPECT_TRUE(s.is_present_for(2));
  EXPECT_FALSE(s.is_present_for(3));
  EXPECT_FALSE(s.is_present_for(4));
  EXPECT_TRUE(s.is_present_for(5));
  EXPECT_FALSE(s.is_present_for(9));
  EXPECT_TRUE(s.is_present_for(11));
  EXPECT_FALSE(s.is_present_for(13));
}

// Value tables

TEST(Signal, ValueDescriptions) {
  Signal s("GearLever", make(0, 4, ByteOrder::Intel));
  s.set_value_descriptions({{0, "Park"}, {1, "Reverse"}, {2, "Neutral"}, {3, "Drive"}});
  EXPECT_EQ(s.describe(0.0), "Park");
  EXPECT_EQ(s.describe(3.0), "Drive");
  EXPECT_TRUE(s.describe(9.0).empty());
  EXPECT_EQ(s.describe_raw(2), "Neutral");
}

TEST(Signal, ValueDescriptionsKeyOnTheRawValueNotTheScaledOne) {
  // A scaled signal with a table: the table entry 2 means raw 2, which is a
  // physical value of 20 once the factor is applied.
  Signal s("Scaled", make(0, 8, ByteOrder::Intel, Signedness::Unsigned, 10.0, 0.0));
  s.set_value_descriptions({{2, "Two"}});
  EXPECT_EQ(s.describe(20.0), "Two");
  EXPECT_TRUE(s.describe(2.0).empty());
}

TEST(Signal, SignedValueDescriptions) {
  Signal s("Trim", make(0, 8, ByteOrder::Intel, Signedness::Signed));
  s.set_value_descriptions({{-1, "Error"}, {0, "Off"}});
  EXPECT_EQ(s.describe(-1.0), "Error");
  EXPECT_EQ(s.describe(0.0), "Off");
}

// Documented worked example, kept as a regression guard

TEST(Codec, MotorolaWorkedExample) {
  // The canonical illustration: a 16-bit Motorola signal at start bit 7 is
  // simply the first two bytes read most significant first. A very common bug
  // is to byte-swap it, producing 0x3412 instead of 0x1234.
  const std::array<std::uint8_t, 8> payload = {0x12, 0x34, 0, 0, 0, 0, 0, 0};
  EXPECT_EQ(make(7, 16, ByteOrder::Motorola).decode_raw(payload.data()), 0x1234u);
  // The Intel signal at start bit 0 over the same bytes is the swap.
  EXPECT_EQ(make(0, 16, ByteOrder::Intel).decode_raw(payload.data()), 0x3412u);
}

TEST(Codec, IntelAndMotorolaCoincideOnlyForSingleBytes) {
  const std::array<std::uint8_t, 8> payload = {0xA5, 0x5A, 0, 0, 0, 0, 0, 0};
  // Byte-aligned, single byte: Intel start 0 and Motorola start 7 both read
  // byte 0 in full.
  EXPECT_EQ(make(0, 8, ByteOrder::Intel).decode_raw(payload.data()),
            make(7, 8, ByteOrder::Motorola).decode_raw(payload.data()));
  // Two bytes: they disagree.
  EXPECT_NE(make(0, 16, ByteOrder::Intel).decode_raw(payload.data()),
            make(7, 16, ByteOrder::Motorola).decode_raw(payload.data()));
}

}  // namespace
}  // namespace canforge::core
