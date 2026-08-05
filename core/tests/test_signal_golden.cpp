// SPDX-License-Identifier: MIT
//
// Golden vectors for the signal codec.
//
// Every expected value in this file was worked out by hand from the bit
// numbering rules, not produced by running the code under test. The comment
// on each case shows the derivation, so a reviewer can check the arithmetic
// without trusting the implementation. Several cases are taken from published
// J1939 parameter group definitions, where the physical result is documented
// independently of any particular decoder.
//
// The reference payload is
//
//   byte     0     1     2     3     4     5     6     7
//   hex     01    23    45    67    89    AB    CD    EF
//   bin  00000001 00100011 01000101 01100111 10001001 10101011 11001101 11101111
//
// chosen because every nibble is distinct, so a byte-order or nibble-order
// mistake produces a visibly wrong answer rather than a plausible one.

#include "canforge/core/Signal.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <string>

namespace canforge::core {
namespace {

using Payload = std::array<std::uint8_t, 8>;

constexpr Payload kSeq = {0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF};
constexpr Payload kAllOnes = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
constexpr Payload kTopBit = {0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
constexpr Payload kLastByteTopBit = {0x00, 0x00, 0x00, 0x00,
                                     0x00, 0x00, 0x00, 0x80};
constexpr Payload kMaxPositive = {0x7F, 0xFF, 0xFF, 0xFF,
                                  0xFF, 0xFF, 0xFF, 0xFF};

struct RawCase {
  const char* name;
  const Payload* payload;
  std::uint16_t start;
  std::uint8_t length;
  ByteOrder order;
  std::uint64_t expected;
};

constexpr ByteOrder kI = ByteOrder::Intel;
constexpr ByteOrder kM = ByteOrder::Motorola;

// Intel / little-endian. The start bit is the signal's LSB and the signal
// grows toward higher bit numbers.
constexpr RawCase kIntelCases[] = {
    // byte0 bit0 = 1
    {"I01 single bit at origin", &kSeq, 0, 1, kI, 0x1},
    // byte0 entire
    {"I02 aligned byte", &kSeq, 0, 8, kI, 0x01},
    // byte1 entire
    {"I03 aligned second byte", &kSeq, 8, 8, kI, 0x23},
    // b1<<8 | b0
    {"I04 aligned 16 bit", &kSeq, 0, 16, kI, 0x2301},
    // b3 b2 b1 b0
    {"I05 aligned 32 bit", &kSeq, 0, 32, kI, 0x67452301},
    // whole payload reversed
    {"I06 full 64 bit", &kSeq, 0, 64, kI, 0xEFCDAB8967452301ULL},
    // bits 4..11: (b0>>4)=0x0 in bits 0..3, (b1&0x0F)=0x3 in bits 4..7
    {"I07 nibble aligned, crosses byte 0/1", &kSeq, 4, 8, kI, 0x30},
    // bits 4..15: (b0>>4)=0x0, then b1=0x23 shifted up 4
    {"I08 12 bit from bit 4", &kSeq, 4, 12, kI, 0x230},
    // bits 12..23: (b1>>4)=0x2, then b2=0x45 shifted up 4
    {"I09 12 bit from bit 12", &kSeq, 12, 12, kI, 0x452},
    // byte0 bits 1,2,3 are all zero
    {"I10 three bits inside byte 0", &kSeq, 1, 3, kI, 0x0},
    // byte0 bit7 = 0
    {"I11 top bit of byte 0", &kSeq, 7, 1, kI, 0x0},
    // byte1 bit0 = 1
    {"I12 low bit of byte 1", &kSeq, 8, 1, kI, 0x1},
    // bits 28..35 straddle the 32 bit boundary:
    //   bits 28..31 -> b3 bits 4..7 = 0x67>>4 = 0x6  (raw bits 0..3)
    //   bits 32..35 -> b4 bits 0..3 = 0x89&0xF = 0x9 (raw bits 4..7)
    {"I13 crosses the 32 bit boundary", &kSeq, 28, 8, kI, 0x96},
    // bits 30..35: (b3>>6)&3 = 1, then (b4&0xF)=9 shifted up 2 -> 1 + 36 = 37
    {"I14 six bits across the 32 bit boundary", &kSeq, 30, 6, kI, 0x25},
    // b4<<8 | b3
    {"I15 16 bit across the 32 bit boundary", &kSeq, 24, 16, kI, 0x8967},
    // bits 20..43: (b2>>4)=0x4 | b3<<4 | b4<<12 | (b5&0xF)<<20
    //            = 0x4 + 0x670 + 0x89000 + 0xB00000
    {"I16 24 bit spanning four bytes", &kSeq, 20, 24, kI, 0xB89674},
    {"I17 last byte", &kSeq, 56, 8, kI, 0xEF},
    // byte7 bit7 = 1
    {"I18 very last bit", &kSeq, 63, 1, kI, 0x1},
    // b7 b6 b5 b4
    {"I19 upper 32 bits", &kSeq, 32, 32, kI, 0xEFCDAB89ULL},
    // bits 17..23 = b2>>1 over seven bits = 0x45>>1 = 0x22
    {"I20 seven bits inside byte 2", &kSeq, 17, 7, kI, 0x22},
    // bits 9..23: (b1>>1)=0x11 | b2<<7 = 0x2280 -> 0x2291
    {"I21 fifteen bits from an odd start", &kSeq, 9, 15, kI, 0x2291},
};

// Motorola / big-endian. The start bit is the signal's MSB; the signal grows
// toward lower bit numbers within the byte, then continues at bit 7 of the
// next byte. Each case notes the canonical position q0 = byte*8 + (7 - bit),
// after which the run is contiguous and ascending.
constexpr RawCase kMotorolaCases[] = {
    // s=7 -> q0=0. byte0 bit7 = 0
    {"M01 single bit, MSB of byte 0", &kSeq, 7, 1, kM, 0x0},
    // s=0 -> q0=7. byte0 bit0 = 1
    {"M02 single bit, LSB of byte 0", &kSeq, 0, 1, kM, 0x1},
    // s=7 -> q0=0, q=0..7 = byte0 MSB first
    {"M03 aligned byte", &kSeq, 7, 8, kM, 0x01},
    // q=0..15 = b0 then b1
    {"M04 aligned 16 bit", &kSeq, 7, 16, kM, 0x0123},
    {"M05 aligned 32 bit", &kSeq, 7, 32, kM, 0x01234567},
    {"M06 full 64 bit", &kSeq, 7, 64, kM, 0x0123456789ABCDEFULL},
    // s=15 -> byte1 bit7 -> q0=8, q=8..23 = b1 then b2
    {"M07 aligned 16 bit at byte 1", &kSeq, 15, 16, kM, 0x2345},
    // s=3 -> q0 = 0*8 + (7-3) = 4. q=4..7 = byte0 bits 3..0 = 0x1
    {"M08 nibble, low half of byte 0", &kSeq, 3, 4, kM, 0x1},
    // s=3 -> q0=4, q=4..15: byte0 bits 3..0 = 0x1, then byte1 = 0x23
    {"M09 12 bit crossing byte 0/1", &kSeq, 3, 12, kM, 0x123},
    // s=1 -> q0 = 7-1 = 6. q=6,7 = byte0 bits 1,0 = 0,1 -> 0b01
    {"M10 two bits at the bottom of byte 0", &kSeq, 1, 2, kM, 0x1},
    // s=11 -> q0 = 8 + (7-3) = 12. byte1 bits 3..0 = 0x3, then byte2 = 0x45
    {"M11 12 bit crossing byte 1/2", &kSeq, 11, 12, kM, 0x345},
    // s=13 -> q0 = 8 + (7-5) = 10. q=10..19:
    //   byte1 bits 5..0 = 0x23 & 0x3F = 0b100011 (six bits)
    //   byte2 bits 7..4 = 0x45 >> 4 = 0b0100     (four bits)
    //   -> 0b1000110100 = 0x234
    {"M12 ten bits, six then four", &kSeq, 13, 10, kM, 0x234},
    // s=39 -> byte4 bit7 -> q0=32. b4 then b5
    {"M13 16 bit at byte 4", &kSeq, 39, 16, kM, 0x89AB},
    // s=31 -> byte3 bit7 -> q0=24. b3 then b4, straddling the 32 bit boundary
    {"M14 16 bit across the 32 bit boundary", &kSeq, 31, 16, kM, 0x6789},
    // s=27 -> byte3 bit3 -> q0 = 24 + (7-3) = 28. q=28..51:
    //   byte3 bits 3..0 = 0x7, byte4 = 0x89, byte5 = 0xAB, byte6 bits 7..4 = 0xC
    {"M15 24 bit across the 32 bit boundary", &kSeq, 27, 24, kM, 0x789ABC},
    // s=35 -> byte4 bit3 -> q0 = 32 + 4 = 36. q=36..59:
    //   byte4 bits 3..0 = 0x9, byte5 = 0xAB, byte6 = 0xCD, byte7 bits 7..4 = 0xE
    {"M16 24 bit, nibble aligned, four bytes", &kSeq, 35, 24, kM, 0x9ABCDE},
    // byte0 bits 7,6,5 = 000
    {"M17 three bits at the top of byte 0", &kSeq, 7, 3, kM, 0x0},
    // s=4 -> q0 = 7-4 = 3. q=3..7 = byte0 bits 4,3,2,1,0 = 00001
    {"M18 five bits inside byte 0", &kSeq, 4, 5, kM, 0x1},
    // s=55 -> byte6 bit7 -> q0=48. b6 then b7
    {"M19 last 16 bits", &kSeq, 55, 16, kM, 0xCDEF},
    // s=63 -> byte7 bit7 -> q0=56
    {"M20 last byte", &kSeq, 63, 8, kM, 0xEF},
    // s=2 -> q0 = 7-2 = 5. q=5..31:
    //   byte0 bits 2,1,0 = 001 then bytes 1,2,3 = 0x234567 -> 27 bits
    {"M21 27 bits from a ragged start", &kSeq, 2, 27, kM, 0x1234567},
    // s=23 -> byte2 bit7 -> q0=16. b2 b3 b4 b5
    {"M22 32 bit at byte 2", &kSeq, 23, 32, kM, 0x456789AB},
    // s=5 -> q0 = 7-5 = 2. q=2..14:
    //   byte0 bits 5..0 = 000001 (six bits)
    //   byte1 bits 7..1 = 0010001 (seven bits)
    //   -> (1 << 7) | 17 = 145
    {"M23 13 bits, six then seven", &kSeq, 5, 13, kM, 0x91},
    // s=7 -> q0=0. q=0..12: byte0 = 0x01 then byte1 bits 7..3 = 00100 = 4
    //   -> (0x01 << 5) | 4 = 36
    {"M24 13 bits, eight then five", &kSeq, 7, 13, kM, 0x24},
    // s=47 -> byte5 bit7 -> q0=40. b5 b6 b7
    {"M25 last 24 bits", &kSeq, 47, 24, kM, 0xABCDEF},
    // s=6 -> q0 = 7-6 = 1. q=1..10:
    //   byte0 bits 6..0 = 0000001 (seven bits)
    //   byte1 bits 7..5 = 001     (three bits)
    //   -> (1 << 3) | 1 = 9
    {"M26 ten bits, seven then three", &kSeq, 6, 10, kM, 0x9},
};

TEST(GoldenRaw, IntelExtraction) {
  for (const RawCase& c : kIntelCases) {
    SignalLayout s;
    s.start_bit = c.start;
    s.bit_length = c.length;
    s.byte_order = c.order;
    ASSERT_TRUE(s.fits(8)) << c.name;
    EXPECT_EQ(s.decode_raw(c.payload->data()), c.expected) << c.name;
    EXPECT_EQ(s.decode_raw_reference(c.payload->data()), c.expected)
        << c.name << " (reference implementation)";
  }
}

TEST(GoldenRaw, MotorolaExtraction) {
  for (const RawCase& c : kMotorolaCases) {
    SignalLayout s;
    s.start_bit = c.start;
    s.bit_length = c.length;
    s.byte_order = c.order;
    ASSERT_TRUE(s.fits(8)) << c.name;
    EXPECT_EQ(s.decode_raw(c.payload->data()), c.expected) << c.name;
    EXPECT_EQ(s.decode_raw_reference(c.payload->data()), c.expected)
        << c.name << " (reference implementation)";
  }
}

// Writing a golden value into an empty payload and reading it back must give
// the same value, and must not disturb any bit outside the signal.
TEST(GoldenRaw, InsertionIsTheInverseAndTouchesNothingElse) {
  for (const auto* table : {&kIntelCases[0], &kMotorolaCases[0]}) {
    const std::size_t count =
        table == &kIntelCases[0] ? std::size(kIntelCases) : std::size(kMotorolaCases);
    for (std::size_t i = 0; i < count; ++i) {
      const RawCase& c = table[i];
      SignalLayout s;
      s.start_bit = c.start;
      s.bit_length = c.length;
      s.byte_order = c.order;

      Payload zeroed{};
      s.encode_raw(c.expected, zeroed.data());
      EXPECT_EQ(s.decode_raw(zeroed.data()), c.expected) << c.name;

      // Setting every bit of the signal in an all-zero payload, then clearing
      // it again, must return the payload to all zeros.
      Payload scratch{};
      s.encode_raw(~std::uint64_t{0}, scratch.data());
      s.encode_raw(0, scratch.data());
      EXPECT_EQ(scratch, Payload{}) << c.name << ": bled outside the signal";

      // And the same starting from all ones.
      Payload ones = kAllOnes;
      s.encode_raw(0, ones.data());
      s.encode_raw(~std::uint64_t{0}, ones.data());
      EXPECT_EQ(ones, kAllOnes) << c.name << ": bled outside the signal";
    }
  }
}

struct SignedCase {
  const char* name;
  const Payload* payload;
  std::uint16_t start;
  std::uint8_t length;
  ByteOrder order;
  double expected;
};

const Payload kFiveBitAllOnes = {0x1F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
const Payload kFiveBitMin = {0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
const Payload kFiveBitMax = {0x0F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

const SignedCase kSignedCases[] = {
    // All ones at any width is -1 in two's complement.
    {"S01 1 bit of ones", &kAllOnes, 0, 1, kI, -1.0},
    {"S02 4 bits of ones", &kAllOnes, 0, 4, kI, -1.0},
    {"S03 64 bits of ones", &kAllOnes, 0, 64, kI, -1.0},
    {"S04 12 bits of ones, Motorola", &kAllOnes, 7, 12, kM, -1.0},
    // 0x80 as an 8 bit signed value is the most negative.
    {"S05 0x80 as int8, Intel", &kTopBit, 0, 8, kI, -128.0},
    {"S06 0x80 as int8, Motorola", &kTopBit, 7, 8, kM, -128.0},
    // A one bit signed signal holds only 0 and -1.
    {"S07 1 bit signed is 0 or -1", &kTopBit, 7, 1, kI, -1.0},
    // 0x8000 as int16 is the most negative 16 bit value.
    {"S08 0x8000 as int16, Motorola", &kTopBit, 7, 16, kM, -32768.0},
    {"S09 top bit of the last byte, Intel", &kLastByteTopBit, 63, 1, kI, -1.0},
    {"S10 0x80 in the last byte, Motorola", &kLastByteTopBit, 63, 8, kM, -128.0},
    // 0x7F is the largest positive int8.
    {"S11 0x7F as int8", &kMaxPositive, 0, 8, kI, 127.0},
    {"S12 0x7FFF as int16, Motorola", &kMaxPositive, 7, 16, kM, 32767.0},
    // Five bit signed: 0x1F -> -1, 0x10 -> -16, 0x0F -> +15.
    {"S13 five bit -1", &kFiveBitAllOnes, 0, 5, kI, -1.0},
    {"S14 five bit minimum", &kFiveBitMin, 0, 5, kI, -16.0},
    {"S15 five bit maximum", &kFiveBitMax, 0, 5, kI, 15.0},
};

TEST(GoldenSigned, TwosComplementAtArbitraryWidths) {
  for (const SignedCase& c : kSignedCases) {
    SignalLayout s;
    s.start_bit = c.start;
    s.bit_length = c.length;
    s.byte_order = c.order;
    s.signedness = Signedness::Signed;
    ASSERT_TRUE(s.fits(8)) << c.name;
    EXPECT_DOUBLE_EQ(s.decode(c.payload->data()), c.expected) << c.name;
  }
}

// Scaled signals taken from published J1939 parameter definitions. The
// physical results below are what the standard says the parameter means, so
// they are independent of this implementation.

struct ScaledCase {
  const char* name;
  Payload payload;
  std::uint16_t start;
  std::uint8_t length;
  ByteOrder order;
  Signedness sign;
  double factor;
  double offset;
  double expected;
};

const ScaledCase kScaledCases[] = {
    // SPN 190, Engine Speed, PGN 61444 (EEC1): bytes 4-5, 0.125 rpm/bit.
    // raw 0x2000 = 8192 -> 8192 * 0.125 = 1024 rpm
    {"R1 J1939 SPN190 engine speed 1024 rpm",
     {0xFF, 0xFF, 0xFF, 0x00, 0x20, 0xFF, 0xFF, 0xFF},
     24, 16, kI, Signedness::Unsigned, 0.125, 0.0, 1024.0},
    // raw 0x2EE0 = 12000 -> 1500 rpm
    {"R2 J1939 SPN190 engine speed 1500 rpm",
     {0xFF, 0xFF, 0xFF, 0xE0, 0x2E, 0xFF, 0xFF, 0xFF},
     24, 16, kI, Signedness::Unsigned, 0.125, 0.0, 1500.0},
    // SPN 84, Wheel-Based Vehicle Speed, PGN 65265 (CCVS1): bytes 2-3,
    // 1/256 km/h per bit. raw 0x1000 = 4096 -> 16 km/h
    {"R3 J1939 SPN84 wheel speed 16 km/h",
     {0xFF, 0x00, 0x10, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF},
     8, 16, kI, Signedness::Unsigned, 1.0 / 256.0, 0.0, 16.0},
    // SPN 110, Engine Coolant Temperature, PGN 65262 (ET1): byte 1,
    // 1 degC/bit with a -40 degC offset. raw 90 -> 50 degC
    {"R4 J1939 SPN110 coolant temperature 50 C",
     {0x5A, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF},
     0, 8, kI, Signedness::Unsigned, 1.0, -40.0, 50.0},
    // SPN 91, Accelerator Pedal Position 1, PGN 61443 (EEC2): byte 2,
    // 0.4 %/bit. raw 100 -> 40 %
    {"R5 J1939 SPN91 accelerator pedal 40 %",
     {0xFF, 0x64, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF},
     8, 8, kI, Signedness::Unsigned, 0.4, 0.0, 40.0},
    // A Motorola sensor reporting decikelvin with a Celsius offset.
    // raw 0x0BB8 = 3000 -> 3000 * 0.1 - 273.15 = 26.85
    {"R6 Motorola 16 bit with a fractional factor and negative offset",
     {0x0B, 0xB8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
     7, 16, kM, Signedness::Unsigned, 0.1, -273.15, 26.85},
    // Signed Motorola: 0xFF38 as int16 = -200 -> -2.0
    {"R7 signed Motorola 16 bit, -2.0",
     {0xFF, 0x38, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
     7, 16, kM, Signedness::Signed, 0.01, 0.0, -2.0},
    // Signed Intel steering angle: 0xFC18 as int16 = -1000 -> -100.0 deg
    {"R8 signed Intel 16 bit, -100.0 degrees",
     {0x18, 0xFC, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
     0, 16, kI, Signedness::Signed, 0.1, 0.0, -100.0},
};

TEST(GoldenScaled, PublishedParameterDefinitions) {
  for (const ScaledCase& c : kScaledCases) {
    SignalLayout s;
    s.start_bit = c.start;
    s.bit_length = c.length;
    s.byte_order = c.order;
    s.signedness = c.sign;
    s.factor = c.factor;
    s.offset = c.offset;
    ASSERT_TRUE(s.fits(8)) << c.name;
    EXPECT_NEAR(s.decode(c.payload.data()), c.expected, 1e-9) << c.name;
  }
}

struct FloatCase {
  const char* name;
  Payload payload;
  std::uint16_t start;
  std::uint8_t length;
  ByteOrder order;
  ValueType type;
  double factor;
  double offset;
  double expected;
};

const FloatCase kFloatCases[] = {
    // 1.0f is 0x3F800000; little endian on the wire is 00 00 80 3F
    {"F1 float32 Intel 1.0",
     {0x00, 0x00, 0x80, 0x3F, 0, 0, 0, 0},
     0, 32, kI, ValueType::Float32, 1.0, 0.0, 1.0},
    // -0.5f is 0xBF000000
    {"F2 float32 Intel -0.5",
     {0x00, 0x00, 0x00, 0xBF, 0, 0, 0, 0},
     0, 32, kI, ValueType::Float32, 1.0, 0.0, -0.5},
    // the same 1.0f big endian: 3F 80 00 00
    {"F3 float32 Motorola 1.0",
     {0x3F, 0x80, 0x00, 0x00, 0, 0, 0, 0},
     7, 32, kM, ValueType::Float32, 1.0, 0.0, 1.0},
    // 1.0 double is 0x3FF0000000000000
    {"F4 float64 Intel 1.0",
     {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xF0, 0x3F},
     0, 64, kI, ValueType::Float64, 1.0, 0.0, 1.0},
    {"F5 float64 Motorola 1.0",
     {0x3F, 0xF0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
     7, 64, kM, ValueType::Float64, 1.0, 0.0, 1.0},
    // 3.0f is 0x40400000; factor and offset still apply on top: 3*2+1 = 7
    {"F6 float32 with factor and offset",
     {0x00, 0x00, 0x40, 0x40, 0, 0, 0, 0},
     0, 32, kI, ValueType::Float32, 2.0, 1.0, 7.0},
};

TEST(GoldenFloat, IeeeSignals) {
  for (const FloatCase& c : kFloatCases) {
    SignalLayout s;
    s.start_bit = c.start;
    s.bit_length = c.length;
    s.byte_order = c.order;
    s.value_type = c.type;
    s.factor = c.factor;
    s.offset = c.offset;
    ASSERT_TRUE(s.validate(8).has_value()) << c.name;
    EXPECT_DOUBLE_EQ(s.decode(c.payload.data()), c.expected) << c.name;

    Payload out{};
    ASSERT_TRUE(s.encode(c.expected, out.data()).has_value()) << c.name;
    EXPECT_DOUBLE_EQ(s.decode(out.data()), c.expected) << c.name << " round trip";
  }
}

// Nine-byte spans. A 64 bit signal that does not start on a byte boundary
// touches nine bytes, which only a CAN FD payload can hold. These two cases
// exercise the widest path in the codec.

TEST(GoldenWide, SixtyFourBitSignalSpanningNineBytes) {
  std::array<std::uint8_t, 12> fd{};
  const std::array<std::uint8_t, 9> head = {0x01, 0x23, 0x45, 0x67, 0x89,
                                            0xAB, 0xCD, 0xEF, 0x5A};
  for (std::size_t i = 0; i < head.size(); ++i) {
    fd[i] = head[i];
  }

  // Intel, start bit 4, 64 bits: bits 4..67.
  //   bits 4..63  = (bytes 0..7 read little endian) >> 4 = 0x0EFCDAB896745230
  //   bits 64..67 = byte8 & 0x0F = 0xA, landing in raw bits 60..63
  //   -> 0xAEFCDAB896745230
  {
    SignalLayout s;
    s.start_bit = 4;
    s.bit_length = 64;
    s.byte_order = kI;
    ASSERT_TRUE(s.fits(12));
    EXPECT_EQ(s.decode_raw(fd.data()), 0xAEFCDAB896745230ULL);
    EXPECT_EQ(s.decode_raw_reference(fd.data()), 0xAEFCDAB896745230ULL);
  }

  // Motorola, start bit 3 -> q0 = 4, 64 bits: q = 4..67, most significant
  // first.
  //   byte0 bits 3..0 = 0x1
  //   bytes 1..7      = 0x23456789ABCDEF
  //   byte8 bits 7..4 = 0x5
  //   -> 0x123456789ABCDEF5
  {
    SignalLayout s;
    s.start_bit = 3;
    s.bit_length = 64;
    s.byte_order = kM;
    ASSERT_TRUE(s.fits(12));
    EXPECT_EQ(s.decode_raw(fd.data()), 0x123456789ABCDEF5ULL);
    EXPECT_EQ(s.decode_raw_reference(fd.data()), 0x123456789ABCDEF5ULL);
  }
}

TEST(GoldenWide, NineByteSpanWritesBackCleanly) {
  for (const ByteOrder order : {kI, kM}) {
    for (std::uint16_t start : {std::uint16_t{1}, std::uint16_t{3},
                                std::uint16_t{4}, std::uint16_t{6}}) {
      SignalLayout s;
      s.start_bit = start;
      s.bit_length = 64;
      s.byte_order = order;
      if (!s.fits(12)) {
        continue;
      }
      std::array<std::uint8_t, 12> buf{};
      buf.fill(0xA5);
      const std::uint64_t v = 0xDEADBEEFCAFEF00DULL;
      s.encode_raw(v, buf.data());
      EXPECT_EQ(s.decode_raw(buf.data()), v)
          << "order=" << static_cast<int>(order) << " start=" << start;
      // Bytes 9..11 are outside every one of these signals.
      EXPECT_EQ(buf[9], 0xA5);
      EXPECT_EQ(buf[10], 0xA5);
      EXPECT_EQ(buf[11], 0xA5);
    }
  }
}

TEST(GoldenBounds, SignalsThatDoNotFitAreRejected) {
  {  // Intel: 1 + 64 > 64
    SignalLayout s;
    s.start_bit = 1;
    s.bit_length = 64;
    s.byte_order = kI;
    EXPECT_FALSE(s.fits(8));
    EXPECT_EQ(s.validate(8).error().code(), ErrorCode::CodecSignalOutOfBounds);
  }
  {  // Motorola: start bit 27 -> q0 = 28, 28 + 40 = 68 > 64
    SignalLayout s;
    s.start_bit = 27;
    s.bit_length = 40;
    s.byte_order = kM;
    EXPECT_FALSE(s.fits(8));
  }
  {  // Motorola: start bit 7 -> q0 = 0, exactly 64 bits fits
    SignalLayout s;
    s.start_bit = 7;
    s.bit_length = 64;
    s.byte_order = kM;
    EXPECT_TRUE(s.fits(8));
  }
}

}  // namespace
}  // namespace canforge::core
