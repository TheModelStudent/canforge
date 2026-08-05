// SPDX-License-Identifier: MIT
//
// Property-based tests for the signal codec.
//
// The generator produces random but *valid* signal layouts -- any start bit,
// any width from 1 to 64, either byte order, either signedness, on payloads of
// 8 and of 64 bytes -- and asserts the properties that must hold for all of
// them. The seed is fixed so a failure is reproducible, and printed so it can
// be varied deliberately.

#include "canforge/core/Signal.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <random>
#include <utility>
#include <vector>

namespace canforge::core {
namespace {

constexpr std::uint64_t kSeed = 0x5EEDC0FFEE1234ULL;
constexpr int kCasesPerProperty = 20000;

class Generator {
 public:
  explicit Generator(std::uint64_t seed) : rng_(seed) {}

  /// A layout guaranteed to fit inside `payload_bytes`.
  SignalLayout layout(std::size_t payload_bytes) {
    const std::size_t total_bits = payload_bytes * 8u;
    SignalLayout s;
    for (;;) {
      s.bit_length = static_cast<std::uint8_t>(
          std::uniform_int_distribution<int>(1, 64)(rng_));
      s.byte_order = std::bernoulli_distribution(0.5)(rng_) ? ByteOrder::Intel
                                                            : ByteOrder::Motorola;
      s.signedness = std::bernoulli_distribution(0.5)(rng_) ? Signedness::Signed
                                                            : Signedness::Unsigned;
      s.start_bit = static_cast<std::uint16_t>(
          std::uniform_int_distribution<int>(0, static_cast<int>(total_bits) - 1)(rng_));
      s.factor = 1.0;
      s.offset = 0.0;
      if (s.fits(payload_bytes)) {
        return s;
      }
    }
  }

  void scale(SignalLayout& s) {
    // Factors that actually occur in databases: powers of two, tenths,
    // 1/256, and the occasional awkward one. Includes negatives.
    static constexpr double kFactors[] = {1.0,   0.5,    0.25,   0.125,
                                          0.1,   0.01,   0.001,  1.0 / 256.0,
                                          2.0,   10.0,   100.0,  0.0625,
                                          -1.0,  -0.1,   -0.5,   3.0};
    s.factor = kFactors[std::uniform_int_distribution<std::size_t>(
        0, std::size(kFactors) - 1)(rng_)];
    static constexpr double kOffsets[] = {0.0, -40.0, 273.15, 1.0, -1000.0, 0.5};
    s.offset = kOffsets[std::uniform_int_distribution<std::size_t>(
        0, std::size(kOffsets) - 1)(rng_)];
  }

  std::uint64_t raw(std::uint8_t len) {
    return std::uniform_int_distribution<std::uint64_t>(0, ~std::uint64_t{0})(rng_) &
           bit_mask(len);
  }

  void fill(std::uint8_t* p, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i) {
      p[i] = static_cast<std::uint8_t>(
          std::uniform_int_distribution<int>(0, 255)(rng_));
    }
  }

  double unit() { return std::uniform_real_distribution<double>(0.0, 1.0)(rng_); }

  std::mt19937_64& engine() { return rng_; }

 private:
  std::mt19937_64 rng_;
};

/// The word-at-a-time codec must agree with the one-bit-at-a-time reference
/// implementation on every layout and every payload. This is the property that
/// makes the fast paths trustworthy: the reference is transcribed directly
/// from the bit numbering rules and is obviously correct by inspection.
TEST(CodecProperty, FastPathAgreesWithReferenceOnExtraction) {
  Generator gen(kSeed);
  for (int i = 0; i < kCasesPerProperty; ++i) {
    const std::size_t bytes = (i % 2 == 0) ? 8u : 64u;
    const SignalLayout s = gen.layout(bytes);
    std::array<std::uint8_t, 64> payload{};
    gen.fill(payload.data(), bytes);

    const std::uint64_t fast = s.decode_raw(payload.data());
    const std::uint64_t slow = s.decode_raw_reference(payload.data());
    ASSERT_EQ(fast, slow) << "seed=" << kSeed << " case=" << i
                          << " start=" << s.start_bit
                          << " len=" << unsigned{s.bit_length}
                          << " order=" << static_cast<int>(s.byte_order)
                          << " bytes=" << bytes;
  }
}

TEST(CodecProperty, FastPathAgreesWithReferenceOnInsertion) {
  Generator gen(kSeed + 1);
  for (int i = 0; i < kCasesPerProperty; ++i) {
    const std::size_t bytes = (i % 2 == 0) ? 8u : 64u;
    const SignalLayout s = gen.layout(bytes);
    std::array<std::uint8_t, 64> base{};
    gen.fill(base.data(), bytes);
    const std::uint64_t value = gen.raw(s.bit_length);

    std::array<std::uint8_t, 64> fast = base;
    std::array<std::uint8_t, 64> slow = base;
    s.encode_raw(value, fast.data());
    s.encode_raw_reference(value, slow.data());

    ASSERT_EQ(fast, slow) << "seed=" << kSeed << " case=" << i
                          << " start=" << s.start_bit
                          << " len=" << unsigned{s.bit_length}
                          << " order=" << static_cast<int>(s.byte_order);
  }
}

/// Writing a raw value and reading it back must be the identity.
TEST(CodecProperty, RawRoundTrip) {
  Generator gen(kSeed + 2);
  for (int i = 0; i < kCasesPerProperty; ++i) {
    const std::size_t bytes = (i % 2 == 0) ? 8u : 64u;
    const SignalLayout s = gen.layout(bytes);
    std::array<std::uint8_t, 64> payload{};
    gen.fill(payload.data(), bytes);
    const std::uint64_t value = gen.raw(s.bit_length);

    s.encode_raw(value, payload.data());
    ASSERT_EQ(s.decode_raw(payload.data()), value)
        << "start=" << s.start_bit << " len=" << unsigned{s.bit_length}
        << " order=" << static_cast<int>(s.byte_order);
  }
}

/// Encoding must not disturb a single bit outside the signal. Verified by
/// masking: write the value, then write it into a second buffer through the
/// reference bit-by-bit path, and separately check that every bit the signal
/// does not own is unchanged from the original payload.
TEST(CodecProperty, EncodingTouchesOnlyItsOwnBits) {
  Generator gen(kSeed + 3);
  for (int i = 0; i < kCasesPerProperty; ++i) {
    const std::size_t bytes = (i % 2 == 0) ? 8u : 64u;
    const SignalLayout s = gen.layout(bytes);
    std::array<std::uint8_t, 64> before{};
    gen.fill(before.data(), bytes);
    std::array<std::uint8_t, 64> after = before;
    s.encode_raw(gen.raw(s.bit_length), after.data());

    // Build a mask of the bits the signal owns, in canonical space.
    std::array<std::uint8_t, 64> owned{};
    for (std::uint8_t k = 0; k < s.bit_length; ++k) {
      const std::size_t q = s.canonical_start() + k;
      const std::size_t byte = q / 8u;
      const std::uint8_t bit =
          s.byte_order == ByteOrder::Intel
              ? static_cast<std::uint8_t>(q % 8u)
              : static_cast<std::uint8_t>(7u - (q % 8u));
      owned[byte] = static_cast<std::uint8_t>(owned[byte] | (1u << bit));
    }
    for (std::size_t b = 0; b < bytes; ++b) {
      const auto outside = static_cast<std::uint8_t>(~owned[b]);
      ASSERT_EQ(before[b] & outside, after[b] & outside)
          << "byte " << b << " start=" << s.start_bit
          << " len=" << unsigned{s.bit_length}
          << " order=" << static_cast<int>(s.byte_order);
    }
  }
}

/// The headline property: a physical value survives an encode/decode round
/// trip to within one quantisation step.
TEST(CodecProperty, PhysicalRoundTripWithinQuantisationStep) {
  Generator gen(kSeed + 4);
  int checked = 0;
  for (int i = 0; i < kCasesPerProperty; ++i) {
    const std::size_t bytes = (i % 2 == 0) ? 8u : 64u;
    SignalLayout s = gen.layout(bytes);
    gen.scale(s);

    // Draw a target uniformly from the representable physical range. Very wide
    // signals with a large factor produce ranges that exceed what a double can
    // resolve to within half a step, so those are skipped rather than tested
    // with a meaningless tolerance.
    const double lo = s.physical_floor();
    const double hi = s.physical_ceiling();
    if (!std::isfinite(lo) || !std::isfinite(hi)) {
      continue;
    }
    const double target = lo + gen.unit() * (hi - lo);
    if (std::fabs(target) / std::fabs(s.factor) > 4.5e15) {
      continue;  // beyond double's integer resolution; nothing to assert
    }

    std::array<std::uint8_t, 64> payload{};
    gen.fill(payload.data(), bytes);
    ASSERT_TRUE(s.encode(target, payload.data()).has_value());
    const double back = s.decode(payload.data());

    // Half a step for the rounding, plus room for the floating point error in
    // (x - offset) / factor and its inverse.
    const double tolerance =
        0.5 * std::fabs(s.factor) +
        1e-9 * std::max({1.0, std::fabs(target), std::fabs(back)});
    ASSERT_LE(std::fabs(back - target), tolerance)
        << "start=" << s.start_bit << " len=" << unsigned{s.bit_length}
        << " order=" << static_cast<int>(s.byte_order)
        << " signed=" << static_cast<int>(s.signedness)
        << " factor=" << s.factor << " offset=" << s.offset
        << " target=" << target << " back=" << back;
    ++checked;
  }
  EXPECT_GT(checked, kCasesPerProperty / 2) << "too many cases were skipped";
}

/// Decoding a raw value and re-encoding the result must reproduce the same raw
/// value exactly -- no drift through the scaling.
TEST(CodecProperty, RawToPhysicalToRawIsExact) {
  Generator gen(kSeed + 5);
  int checked = 0;
  for (int i = 0; i < kCasesPerProperty; ++i) {
    SignalLayout s = gen.layout(8);
    gen.scale(s);
    if (s.bit_length > 48u) {
      // Above ~2^48 the double mantissa can no longer hold every raw value
      // once a non-trivial factor is applied, so exactness is not claimed.
      continue;
    }
    const std::uint64_t raw = gen.raw(s.bit_length);
    const double physical = s.raw_to_physical(raw);
    ASSERT_EQ(s.physical_to_raw(physical), raw)
        << "start=" << s.start_bit << " len=" << unsigned{s.bit_length}
        << " factor=" << s.factor << " offset=" << s.offset << " raw=" << raw;
    ++checked;
  }
  EXPECT_GT(checked, 1000);
}

/// A declared [min|max] saturates; it never wraps.
TEST(CodecProperty, DeclaredRangeSaturates) {
  Generator gen(kSeed + 6);
  for (int i = 0; i < 5000; ++i) {
    SignalLayout s = gen.layout(8);
    s.factor = 1.0;
    s.offset = 0.0;
    if (s.bit_length > 32u) {
      continue;
    }
    s.minimum = 10.0;
    s.maximum = 20.0;
    if (s.physical_ceiling() < 20.0) {
      continue;  // the signal cannot reach the declared maximum
    }
    std::array<std::uint8_t, 8> payload{};
    ASSERT_TRUE(s.encode(1e9, payload.data()).has_value());
    EXPECT_DOUBLE_EQ(s.decode(payload.data()), 20.0);
    ASSERT_TRUE(s.encode(-1e9, payload.data()).has_value());
    EXPECT_DOUBLE_EQ(s.decode(payload.data()), 10.0);
  }
}

/// Without a declared range, encoding still saturates at the widest value the
/// bit field can hold rather than wrapping around.
TEST(CodecProperty, BitWidthSaturates) {
  for (std::uint8_t len = 1; len <= 64; ++len) {
    for (const Signedness sign : {Signedness::Unsigned, Signedness::Signed}) {
      SignalLayout s;
      s.start_bit = 0;
      s.bit_length = len;
      s.byte_order = ByteOrder::Intel;
      s.signedness = sign;

      std::array<std::uint8_t, 8> payload{};
      ASSERT_TRUE(s.encode(1e30, payload.data()).has_value());
      EXPECT_DOUBLE_EQ(s.decode(payload.data()), s.raw_maximum())
          << "len=" << unsigned{len} << " signed=" << static_cast<int>(sign);

      ASSERT_TRUE(s.encode(-1e30, payload.data()).has_value());
      EXPECT_DOUBLE_EQ(s.decode(payload.data()), s.raw_minimum())
          << "len=" << unsigned{len} << " signed=" << static_cast<int>(sign);
    }
  }
}

/// Rounding is half away from zero, matching CANoe.
TEST(CodecProperty, RoundsHalfAwayFromZero) {
  SignalLayout s;
  s.start_bit = 0;
  s.bit_length = 8;
  s.byte_order = ByteOrder::Intel;
  s.signedness = Signedness::Signed;
  std::array<std::uint8_t, 8> payload{};

  const std::array<std::pair<double, double>, 8> cases = {{
      {0.5, 1.0}, {1.5, 2.0}, {2.5, 3.0}, {-0.5, -1.0},
      {-1.5, -2.0}, {-2.5, -3.0}, {0.4, 0.0}, {-0.4, 0.0},
  }};
  for (const auto& [in, want] : cases) {
    ASSERT_TRUE(s.encode(in, payload.data()).has_value());
    EXPECT_DOUBLE_EQ(s.decode(payload.data()), want) << "input " << in;
  }
}

TEST(CodecProperty, NonFiniteIntegerValuesAreRejected) {
  SignalLayout s;
  s.bit_length = 16;
  std::array<std::uint8_t, 8> payload{};
  const double inf = std::numeric_limits<double>::infinity();
  EXPECT_EQ(s.encode(inf, payload.data()).error().code(),
            ErrorCode::CodecValueNotFinite);
  EXPECT_EQ(s.encode(std::numeric_limits<double>::quiet_NaN(), payload.data())
                .error()
                .code(),
            ErrorCode::CodecValueNotFinite);

  // A float signal, by contrast, can carry a NaN, so it must be accepted.
  SignalLayout f;
  f.bit_length = 32;
  f.value_type = ValueType::Float32;
  EXPECT_TRUE(f.encode(std::numeric_limits<double>::quiet_NaN(), payload.data())
                  .has_value());
  EXPECT_TRUE(std::isnan(f.decode(payload.data())));
}

}  // namespace
}  // namespace canforge::core
