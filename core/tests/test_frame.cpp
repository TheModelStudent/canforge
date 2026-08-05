// SPDX-License-Identifier: MIT
#include "canforge/core/Frame.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>

namespace canforge::core {
namespace {

TEST(CanId, StandardBoundaries) {
  EXPECT_TRUE(CanId::standard(0u).has_value());
  EXPECT_TRUE(CanId::standard(kStandardIdMax).has_value());
  auto over = CanId::standard(0x800u);
  ASSERT_FALSE(over.has_value());
  EXPECT_EQ(over.error().code(), ErrorCode::FrameBadIdentifier);
  EXPECT_EQ(over.error().detail().a, 0x800u);
  EXPECT_EQ(over.error().detail().b, kStandardIdMax);
}

TEST(CanId, ExtendedBoundaries) {
  EXPECT_TRUE(CanId::extended(0u).has_value());
  EXPECT_TRUE(CanId::extended(kExtendedIdMax).has_value());
  EXPECT_FALSE(CanId::extended(0x20000000u).has_value());
}

TEST(CanId, CompileTimeConstruction) {
  // The template form is checked by static_assert, so these cost nothing and
  // an out-of-range literal would not compile at all.
  constexpr CanId a = CanId::make_standard<0x123>();
  constexpr CanId b = CanId::make_extended<0x18DAF110>();
  static_assert(a.value() == 0x123u);
  static_assert(!a.is_extended());
  static_assert(b.value() == 0x18DAF110u);
  static_assert(b.is_extended());
  static_assert(a.bit_count() == 11);
  static_assert(b.bit_count() == 29);
  EXPECT_NE(a, b);
}

TEST(CanId, StandardAndExtendedWithTheSameNumberDiffer) {
  const auto s = CanId::standard(0x123u).value();
  const auto e = CanId::extended(0x123u).value();
  EXPECT_NE(s, e);
  EXPECT_EQ(s.value(), e.value());
  EXPECT_FALSE(s.is_extended());
  EXPECT_TRUE(e.is_extended());
}

TEST(CanId, PackedRoundTripsThroughSocketCanLayout) {
  const auto e = CanId::extended(0x1ABCDEFu).value();
  EXPECT_EQ(e.packed(), 0x1ABCDEFu | kEffFlag);
  const auto back = CanId::from_packed(e.packed()).value();
  EXPECT_EQ(back, e);

  const auto s = CanId::standard(0x7FFu).value();
  EXPECT_EQ(s.packed(), 0x7FFu);
  EXPECT_EQ(CanId::from_packed(s.packed()).value(), s);
}

TEST(CanId, AcceptsButFlagsReservedBaseIdentifiers) {
  // ISO 11898-1 forbids 0x7F0..0x7FF; real databases contain them.
  const auto id = CanId::standard(0x7F5u);
  ASSERT_TRUE(id.has_value());
  EXPECT_TRUE(id.value().violates_base_id_rule());
  EXPECT_FALSE(CanId::standard(0x7EFu).value().violates_base_id_rule());
  EXPECT_FALSE(CanId::extended(0x7FFu).value().violates_base_id_rule());
}

TEST(CanId, ArbitrationOrder) {
  const auto low = CanId::standard(0x100u).value();
  const auto high = CanId::standard(0x200u).value();
  EXPECT_LT(low, high);

  // Same 11-bit base: the standard frame wins because IDE is dominant.
  const auto std_id = CanId::standard(0x100u).value();
  const auto ext_id = CanId::extended(0x100u << 18).value();
  EXPECT_LT(std_id, ext_id);

  // A lower base beats any extension of a higher base.
  const auto ext_low_base = CanId::extended((0x0FFu << 18) | 0x3FFFFu).value();
  EXPECT_LT(ext_low_base, std_id);
}

TEST(Dlc, ClassicMapping) {
  for (std::uint8_t dlc = 0; dlc <= 8u; ++dlc) {
    EXPECT_EQ(dlc_to_length(dlc, false), dlc);
  }
  // Reserved codes: a classic controller puts 8 bytes on the wire.
  for (std::uint8_t dlc = 9u; dlc <= 15u; ++dlc) {
    EXPECT_EQ(dlc_to_length(dlc, false), 8u) << "dlc " << int{dlc};
  }
}

TEST(Dlc, FdMapping) {
  const std::array<std::uint8_t, 16> expected = {0, 1,  2,  3,  4,  5,  6,  7,
                                                 8, 12, 16, 20, 24, 32, 48, 64};
  for (std::uint8_t dlc = 0; dlc < 16u; ++dlc) {
    EXPECT_EQ(dlc_to_length(dlc, true), expected[dlc]) << "dlc " << int{dlc};
  }
}

TEST(Dlc, LengthToDlcRoundTripsForEveryEncodableLength) {
  for (std::uint8_t dlc = 0; dlc <= 8u; ++dlc) {
    const auto fit = length_to_dlc(dlc, false);
    ASSERT_TRUE(fit.has_value());
    EXPECT_EQ(fit.value().dlc, dlc);
    EXPECT_EQ(fit.value().padding, 0u);
  }
  for (std::uint8_t dlc = 0; dlc < 16u; ++dlc) {
    const std::uint8_t len = kFdDlcToLength[dlc];
    const auto fit = length_to_dlc(len, true);
    ASSERT_TRUE(fit.has_value());
    EXPECT_EQ(fit.value().length, len);
    EXPECT_EQ(fit.value().padding, 0u);
  }
}

TEST(Dlc, FdRoundsUpAndReportsPadding) {
  // 9..11 bytes are not encodable; the next code up carries 12.
  for (std::size_t len = 9; len <= 11; ++len) {
    const auto fit = length_to_dlc(len, true);
    ASSERT_TRUE(fit.has_value());
    EXPECT_EQ(fit.value().dlc, 9u);
    EXPECT_EQ(fit.value().length, 12u);
    EXPECT_EQ(fit.value().padding, 12u - len);
  }
  const auto fit = length_to_dlc(33, true);
  ASSERT_TRUE(fit.has_value());
  EXPECT_EQ(fit.value().length, 48u);
  EXPECT_EQ(fit.value().padding, 15u);
}

TEST(Dlc, RejectsOversizedLengths) {
  EXPECT_FALSE(length_to_dlc(9, false).has_value());
  EXPECT_FALSE(length_to_dlc(65, true).has_value());
}

TEST(Frame, LayoutContract) {
  EXPECT_TRUE(std::is_trivially_copyable_v<Frame>);
  EXPECT_TRUE(std::is_standard_layout_v<Frame>);
  EXPECT_EQ(sizeof(Frame), 24u);
  EXPECT_LE(sizeof(Frame), 64u);
  EXPECT_EQ(alignof(Frame), 8u);
  EXPECT_TRUE(std::is_trivially_copyable_v<FdFrame>);
  EXPECT_EQ(sizeof(FdFrame), 80u);
}

TEST(Frame, DefaultIsAZeroDataFrame) {
  Frame f;
  EXPECT_EQ(f.id(), CanId{});
  EXPECT_EQ(f.dlc(), 0u);
  EXPECT_EQ(f.size(), 0u);
  EXPECT_FALSE(f.is_fd());
  EXPECT_FALSE(f.is_remote());
}

TEST(Frame, MakeCopiesPayload) {
  const auto id = CanId::standard(0x201u).value();
  const auto f = Frame::make(id, {0xDE, 0xAD, 0xBE, 0xEF});
  ASSERT_TRUE(f.has_value());
  EXPECT_EQ(f.value().id(), id);
  EXPECT_EQ(f.value().dlc(), 4u);
  EXPECT_EQ(f.value().size(), 4u);
  EXPECT_EQ(f.value().data()[0], 0xDE);
  EXPECT_EQ(f.value().data()[3], 0xEF);
}

TEST(Frame, RejectsOversizedClassicPayload) {
  const auto id = CanId::standard(1u).value();
  const std::array<std::uint8_t, 9> nine{};
  const auto f = Frame::make(id, nine.data(), nine.size());
  ASSERT_FALSE(f.has_value());
  EXPECT_EQ(f.error().code(), ErrorCode::FramePayloadTooLarge);
}

TEST(Frame, ClassicFrameCannotBeFd) {
  const auto id = CanId::standard(1u).value();
  const auto f = Frame::make(id, {1, 2, 3}, FrameFlags::Fd);
  ASSERT_FALSE(f.has_value());
  EXPECT_EQ(f.error().code(), ErrorCode::FrameBadFlags);
}

TEST(Frame, FdFrameAcceptsSixtyFourBytes) {
  const auto id = CanId::extended(0x1000u).value();
  std::array<std::uint8_t, 64> payload{};
  for (std::size_t i = 0; i < payload.size(); ++i) {
    payload[i] = static_cast<std::uint8_t>(i);
  }
  const auto f = FdFrame::make(id, payload.data(), payload.size(),
                               FrameFlags::Fd | FrameFlags::Brs);
  ASSERT_TRUE(f.has_value());
  EXPECT_EQ(f.value().dlc(), 15u);
  EXPECT_EQ(f.value().size(), 64u);
  EXPECT_TRUE(f.value().is_fd());
  EXPECT_TRUE(f.value().is_brs());
  EXPECT_EQ(f.value().data()[63], 63u);
}

TEST(Frame, FdRejectsNonEncodableLength) {
  const auto id = CanId::standard(1u).value();
  std::array<std::uint8_t, 10> payload{};
  const auto f = FdFrame::make(id, payload.data(), payload.size(), FrameFlags::Fd);
  ASSERT_FALSE(f.has_value());
  EXPECT_EQ(f.error().code(), ErrorCode::FrameBadDlc);
  EXPECT_EQ(f.error().detail().b, 12u) << "should suggest the next encodable size";
}

TEST(Frame, FdHasNoRemoteFrames) {
  const auto id = CanId::standard(1u).value();
  const auto f = FdFrame::make(id, nullptr, 0, FrameFlags::Fd | FrameFlags::Rtr);
  ASSERT_FALSE(f.has_value());
  EXPECT_EQ(f.error().code(), ErrorCode::FrameBadFlags);
}

TEST(Frame, BrsAndEsiRequireFd) {
  const auto id = CanId::standard(1u).value();
  EXPECT_FALSE(FdFrame::make(id, {1}, FrameFlags::Brs).has_value());
  EXPECT_FALSE(FdFrame::make(id, {1}, FrameFlags::Esi).has_value());
}

TEST(Frame, RemoteFrameCarriesNoData) {
  const auto id = CanId::standard(0x123u).value();
  const auto rtr = Frame::make_remote(id, 8);
  ASSERT_TRUE(rtr.has_value());
  EXPECT_TRUE(rtr.value().is_remote());
  EXPECT_EQ(rtr.value().dlc(), 8u);
  EXPECT_EQ(rtr.value().size(), 0u) << "a remote frame transfers no bytes";

  EXPECT_FALSE(Frame::make(id, {1, 2}, FrameFlags::Rtr).has_value());
  EXPECT_FALSE(Frame::make_remote(id, 9).has_value());
}

TEST(Frame, FromWirePreservesReservedClassicDlc) {
  // A classic frame logged with DLC 12 must keep the 12 so that a log
  // read/write round trip is byte-identical, while reporting 8 usable bytes.
  const auto id = CanId::standard(0x100u).value();
  const std::array<std::uint8_t, 8> payload{1, 2, 3, 4, 5, 6, 7, 8};
  const auto f = Frame::from_wire(id, 12u, payload.data());
  ASSERT_TRUE(f.has_value());
  EXPECT_EQ(f.value().dlc(), 12u) << "raw DLC must survive";
  EXPECT_EQ(f.value().size(), 8u) << "but only 8 bytes are real";
  EXPECT_EQ(f.value().data()[7], 8u);
}

TEST(Frame, FromWireRejectsFiveBitDlc) {
  const auto id = CanId::standard(1u).value();
  EXPECT_FALSE(Frame::from_wire(id, 16u, nullptr).has_value());
}

TEST(Frame, EqualityIgnoresTimestampAndPadding) {
  const auto id = CanId::standard(0x55u).value();
  auto a = Frame::make(id, {1, 2, 3}, FrameFlags::None, 1000).value();
  auto b = Frame::make(id, {1, 2, 3}, FrameFlags::None, 2000).value();
  EXPECT_EQ(a, b) << "timestamp is metadata, not frame content";
  b.data()[5] = 0xFF;  // beyond dlc
  EXPECT_EQ(a, b);
  b.data()[1] = 0xFF;  // inside dlc
  EXPECT_NE(a, b);
}

TEST(Frame, WidenPreservesEverything) {
  const auto id = CanId::extended(0x1234u).value();
  const auto classic = Frame::make(id, {9, 8, 7}, FrameFlags::None, 42).value();
  const auto wide = classic.widen<64>();
  EXPECT_EQ(wide.id(), id);
  EXPECT_EQ(wide.dlc(), 3u);
  EXPECT_EQ(wide.size(), 3u);
  EXPECT_EQ(wide.timestamp_ns(), 42u);
  EXPECT_EQ(wide.data()[2], 7u);
}

TEST(Frame, Flags) {
  const FrameFlags f = FrameFlags::Fd | FrameFlags::Brs;
  EXPECT_TRUE(has_flag(f, FrameFlags::Fd));
  EXPECT_TRUE(has_flag(f, FrameFlags::Brs));
  EXPECT_FALSE(has_flag(f, FrameFlags::Esi));
  EXPECT_FALSE(has_flag(FrameFlags::None, FrameFlags::Fd));
}

}  // namespace
}  // namespace canforge::core
