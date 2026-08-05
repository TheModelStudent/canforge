// SPDX-License-Identifier: MIT
#include "canforge/transport/IBus.hpp"

#include <gtest/gtest.h>

namespace canforge::transport {
namespace {

using core::CanId;
using core::FdFrame;
using core::FrameFlags;

FdFrame classic(bool extended, std::size_t bytes) {
  const CanId id =
      extended ? CanId::extended(0x12345678).value() : CanId::standard(0x123).value();
  return FdFrame::make(id, nullptr, bytes).value();
}

TEST(Timing, ClassicWorstCaseMatchesThePublishedFigures) {
  const BusTiming t{500000, 2000000};
  // The two numbers everyone quotes for a full 8-byte frame at worst-case
  // stuffing: 135 bits standard, 160 bits extended.
  EXPECT_EQ(frame_timing(classic(false, 8), t).total_bits(), 135u);
  EXPECT_EQ(frame_timing(classic(true, 8), t).total_bits(), 160u);
  // And the zero-length cases, which are what a heartbeat costs.
  //   standard: 47 + floor(33/4) = 47 + 8 = 55
  //   extended: 67 + floor(53/4) = 67 + 13 = 80
  EXPECT_EQ(frame_timing(classic(false, 0), t).total_bits(), 55u);
  EXPECT_EQ(frame_timing(classic(true, 0), t).total_bits(), 80u);
}

TEST(Timing, DurationFollowsTheBitrate) {
  const BusTiming slow{125000, 0};
  const BusTiming fast{1000000, 0};
  const auto s = frame_timing(classic(false, 8), slow);
  const auto f = frame_timing(classic(false, 8), fast);
  EXPECT_EQ(s.total_bits(), f.total_bits());
  // 135 bits at 125 kbit/s is 1.08 ms; at 1 Mbit/s it is 135 us.
  EXPECT_EQ(s.nanoseconds, 1080000u);
  EXPECT_EQ(f.nanoseconds, 135000u);
}

TEST(Timing, LongerPayloadsTakeLonger) {
  const BusTiming t{500000, 0};
  std::uint64_t previous = 0;
  for (std::size_t n = 0; n <= 8; ++n) {
    const auto timing = frame_timing(classic(false, n), t);
    EXPECT_GT(timing.nanoseconds, previous) << "payload " << n;
    previous = timing.nanoseconds;
  }
}

TEST(Timing, RemoteFramesCarryNoData) {
  const BusTiming t{500000, 0};
  const auto rtr = FdFrame::make_remote(CanId::standard(0x100).value(), 8).value();
  EXPECT_EQ(frame_timing(rtr, t).total_bits(),
            frame_timing(classic(false, 0), t).total_bits());
}

TEST(Timing, FdSplitsIntoArbitrationAndDataPhases) {
  const BusTiming t{500000, 2000000};
  const auto id = CanId::standard(0x123).value();
  const auto no_brs = FdFrame::make(id, nullptr, 64, FrameFlags::Fd).value();
  const auto with_brs =
      FdFrame::make(id, nullptr, 64, FrameFlags::Fd | FrameFlags::Brs).value();

  const auto a = frame_timing(no_brs, t);
  const auto b = frame_timing(with_brs, t);
  EXPECT_GT(a.data_bits, 0u);
  EXPECT_EQ(a.total_bits(), b.total_bits()) << "the same bits are on the wire";
  EXPECT_LT(b.nanoseconds, a.nanoseconds)
      << "the bit rate switch must make the frame shorter in time";
}

TEST(Timing, FdUsesTheWiderCrcAboveSixteenBytes) {
  const BusTiming t{500000, 2000000};
  const auto id = CanId::standard(1).value();
  const auto small = FdFrame::make(id, nullptr, 16, FrameFlags::Fd).value();
  const auto large = FdFrame::make(id, nullptr, 20, FrameFlags::Fd).value();
  // Four extra data bytes are 32 bits; the CRC also grows from 17 to 21, so
  // the difference must exceed 32.
  EXPECT_GT(large.size() * 8u, 0u);
  EXPECT_GT(frame_timing(large, t).data_bits - frame_timing(small, t).data_bits, 32u);
}

TEST(Filters, EmptyListAcceptsEverything) {
  const std::vector<Filter> none;
  EXPECT_TRUE(accepted_by(none, CanId::standard(0x123).value()));
  EXPECT_TRUE(accepted_by(none, CanId::extended(0x1234567).value()));
}

TEST(Filters, ExactMatch) {
  const std::vector<Filter> f = {Filter::exact(CanId::standard(0x123).value())};
  EXPECT_TRUE(accepted_by(f, CanId::standard(0x123).value()));
  EXPECT_FALSE(accepted_by(f, CanId::standard(0x124).value()));
  // The same number as an extended identifier is a different frame.
  EXPECT_FALSE(accepted_by(f, CanId::extended(0x123).value()));
}

TEST(Filters, MaskedRange) {
  // Accept 0x120..0x12F.
  const std::vector<Filter> f = {Filter::range(0x120, 0x7F0)};
  EXPECT_TRUE(accepted_by(f, CanId::standard(0x120).value()));
  EXPECT_TRUE(accepted_by(f, CanId::standard(0x12F).value()));
  EXPECT_FALSE(accepted_by(f, CanId::standard(0x130).value()));
  EXPECT_FALSE(accepted_by(f, CanId::standard(0x110).value()));
}

TEST(Filters, Inverted) {
  Filter f = Filter::exact(CanId::standard(0x123).value());
  f.invert = true;
  EXPECT_FALSE(accepted_by({f}, CanId::standard(0x123).value()));
  EXPECT_TRUE(accepted_by({f}, CanId::standard(0x124).value()));
}

TEST(Filters, SeveralFiltersAreOred) {
  const std::vector<Filter> f = {Filter::exact(CanId::standard(0x100).value()),
                                 Filter::exact(CanId::standard(0x200).value())};
  EXPECT_TRUE(accepted_by(f, CanId::standard(0x100).value()));
  EXPECT_TRUE(accepted_by(f, CanId::standard(0x200).value()));
  EXPECT_FALSE(accepted_by(f, CanId::standard(0x300).value()));
}

TEST(Statistics, BusLoad) {
  BusStatistics s;
  const BusTiming t{500000, 0};
  EXPECT_DOUBLE_EQ(s.bus_load(t), 0.0) << "no traffic means no load";

  // Ten full frames of 135 bits inside one second on a 500 kbit/s bus.
  s.frames_received = 10;
  s.wire_bits = 1350;
  s.first_timestamp_ns = 0;
  s.last_timestamp_ns = 1000000000ULL;
  EXPECT_NEAR(s.bus_load(t), 1350.0 / 500000.0, 1e-12);

  // A saturated bus is reported as 1.0, never above.
  s.wire_bits = 900000;
  EXPECT_DOUBLE_EQ(s.bus_load(t), 1.0);
}

}  // namespace
}  // namespace canforge::transport
