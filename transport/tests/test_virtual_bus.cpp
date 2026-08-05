// SPDX-License-Identifier: MIT
#include "canforge/transport/VirtualBus.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <vector>

namespace canforge::transport {
namespace {

using core::CanId;
using core::FdFrame;
using core::Frame;
using core::FrameFlags;

using ms = std::chrono::milliseconds;
using us = std::chrono::microseconds;

FdFrame data_frame(std::uint32_t id, std::size_t len = 8) {
  return FdFrame::make(CanId::standard(id).value(), nullptr, len).value();
}

struct Fixture {
  std::shared_ptr<VirtualMedium> medium;
  std::unique_ptr<VirtualBus> a;
  std::unique_ptr<VirtualBus> b;
  std::unique_ptr<VirtualBus> c;

  explicit Fixture(BusTiming t = {500000, 2000000})
      : medium(VirtualMedium::create(t)) {
    a = medium->attach("ECM");
    b = medium->attach("TCM");
    c = medium->attach("DASH");
    EXPECT_TRUE(a->open().has_value());
    EXPECT_TRUE(b->open().has_value());
    EXPECT_TRUE(c->open().has_value());
  }
};

TEST(VirtualBus, DeliversToEveryParticipant) {
  Fixture f;
  ASSERT_TRUE(f.a->send(data_frame(0x123, 4)).has_value());
  f.medium->drain();

  for (VirtualBus* p : {f.b.get(), f.c.get()}) {
    const auto got = p->receive(ms(0));
    ASSERT_TRUE(got.has_value()) << p->name();
    EXPECT_EQ(got.value().id().value(), 0x123u);
    EXPECT_EQ(got.value().size(), 4u);
  }
  // SocketCAN loops a transmitted frame back to the sender by default.
  EXPECT_TRUE(f.a->receive(ms(0)).has_value());
}

TEST(VirtualBus, LoopbackCanBeTurnedOff) {
  Fixture f;
  f.medium->set_loopback(false);
  ASSERT_TRUE(f.a->send(data_frame(0x123)).has_value());
  f.medium->drain();
  EXPECT_FALSE(f.a->receive(ms(0)).has_value());
  EXPECT_TRUE(f.b->receive(ms(0)).has_value());
}

TEST(VirtualBus, FramesTakeARealisticAmountOfTime) {
  Fixture f({500000, 0});
  const std::uint64_t start = f.medium->now_ns();
  ASSERT_TRUE(f.a->send(data_frame(0x123, 8)).has_value());
  const auto took = f.medium->drain();
  // 135 bits at 500 kbit/s is 270 microseconds.
  EXPECT_EQ(took.count(), 270000);
  EXPECT_EQ(f.medium->now_ns() - start, 270000u);
}

TEST(VirtualBus, TheMediumCarriesOneFrameAtATime) {
  Fixture f({500000, 0});
  // Three frames from one node: they queue and go out back to back.
  for (std::uint32_t i = 0; i < 3; ++i) {
    ASSERT_TRUE(f.a->send(data_frame(0x100 + i, 8)).has_value());
  }
  const auto took = f.medium->drain();
  EXPECT_EQ(took.count(), 3 * 270000);
  EXPECT_EQ(f.b->available(), 3u);
}

TEST(Arbitration, TheLowerIdentifierWins) {
  Fixture f({500000, 0});
  ASSERT_TRUE(f.a->send(data_frame(0x300)).has_value());
  ASSERT_TRUE(f.b->send(data_frame(0x100)).has_value());
  ASSERT_TRUE(f.c->send(data_frame(0x200)).has_value());

  f.medium->drain();
  // Everything eventually gets through, but the order is by priority, not by
  // the order in which the nodes queued their frames.
  std::vector<std::uint32_t> order;
  for (;;) {
    const auto got = f.a->receive(ms(0));
    if (!got) {
      break;
    }
    order.push_back(got.value().id().value());
  }
  EXPECT_EQ(order, (std::vector<std::uint32_t>{0x100, 0x200, 0x300}));
}

TEST(Arbitration, TheLoserRetransmitsRatherThanLosingTheFrame) {
  Fixture f({500000, 0});
  ASSERT_TRUE(f.a->send(data_frame(0x700, 8)).has_value());
  ASSERT_TRUE(f.b->send(data_frame(0x001, 8)).has_value());

  // Sample in the middle of the first frame: one transmission is under way and
  // the loser is still holding its own.
  f.medium->advance(us(200));
  EXPECT_EQ(f.a->pending(), 1u) << "the loser must still hold its frame";
  EXPECT_EQ(f.b->pending(), 0u) << "the winner's frame is on the wire";
  EXPECT_TRUE(f.medium->busy());

  // By the middle of the second frame the loser has won the next arbitration.
  f.medium->advance(us(270));
  EXPECT_EQ(f.a->pending(), 0u) << "and must win the next arbitration";

  // Both frames eventually land, in priority order rather than in the order
  // they were queued.
  f.medium->drain();
  const auto first = f.c->receive(ms(0));
  const auto second = f.c->receive(ms(0));
  ASSERT_TRUE(first.has_value());
  ASSERT_TRUE(second.has_value());
  EXPECT_EQ(first.value().id().value(), 0x001u);
  EXPECT_EQ(second.value().id().value(), 0x700u);
}

TEST(Arbitration, LossesAreCounted) {
  Fixture f({500000, 0});
  ASSERT_TRUE(f.a->send(data_frame(0x700)).has_value());
  ASSERT_TRUE(f.b->send(data_frame(0x001)).has_value());
  f.medium->drain();

  EXPECT_EQ(f.a->statistics().arbitration_losses, 1u);
  EXPECT_EQ(f.b->statistics().arbitration_losses, 0u);

  ASSERT_EQ(f.medium->arbitration_log().size(), 1u);
  const ArbitrationEvent& event = f.medium->arbitration_log().front();
  EXPECT_EQ(event.winner.value(), 0x001u);
  EXPECT_EQ(event.winner_name, "TCM");
  ASSERT_EQ(event.losers.size(), 1u);
  EXPECT_EQ(event.losers.front().value(), 0x700u);
}

TEST(Arbitration, StandardBeatsExtendedWithTheSameBaseIdentifier) {
  Fixture f({500000, 0});
  // The IDE bit is dominant for a standard frame, so with the same 11-bit
  // base the standard frame wins even though the extended one is numerically
  // larger only in its extension bits.
  const auto std_frame =
      FdFrame::make(CanId::standard(0x100).value(), nullptr, 1).value();
  const auto ext_frame =
      FdFrame::make(CanId::extended(0x100u << 18).value(), nullptr, 1).value();
  ASSERT_TRUE(f.a->send(ext_frame).has_value());
  ASSERT_TRUE(f.b->send(std_frame).has_value());
  f.medium->drain();

  const auto first = f.c->receive(ms(0));
  ASSERT_TRUE(first.has_value());
  EXPECT_FALSE(first.value().id().is_extended())
      << "the standard frame should have won arbitration";
}

TEST(Arbitration, AHighPriorityTalkerCanStarveALowPriorityOne) {
  // The classic CAN pathology, reproduced deliberately: a node that always has
  // something queued at a low identifier never lets a higher identifier
  // through. Being able to show this is the point of modelling arbitration at
  // all rather than using a plain queue.
  Fixture f({500000, 0});
  // The low priority frame is queued first and stays queued.
  ASSERT_TRUE(f.b->send(data_frame(0x7FF, 8)).has_value());
  // The high priority talker always has something ready, so it wins every
  // arbitration for as long as its backlog lasts. The backlog is deeper than
  // the number of rounds below, so it never runs dry mid-test.
  for (int round = 0; round < 25; ++round) {
    ASSERT_TRUE(f.a->send(data_frame(0x001, 8)).has_value());
  }

  // Step through twenty frame times one at a time.
  for (int round = 0; round < 20; ++round) {
    f.medium->advance(us(270));
    EXPECT_EQ(f.b->pending(), 1u) << "round " << round
                                  << ": the low priority frame is still stuck";
  }
  EXPECT_GE(f.b->statistics().arbitration_losses, 20u);

  // Once the busy talker runs dry, the starved frame goes out immediately.
  f.medium->drain();
  EXPECT_EQ(f.b->pending(), 0u);
  EXPECT_EQ(f.a->statistics().frames_sent, 25u);
  EXPECT_EQ(f.b->statistics().frames_sent, 1u);
}

TEST(Arbitration, NoArbitrationEventWhenOnlyOneNodeIsTalking) {
  Fixture f;
  ASSERT_TRUE(f.a->send(data_frame(0x123)).has_value());
  f.medium->drain();
  EXPECT_TRUE(f.medium->arbitration_log().empty());
}

TEST(VirtualBus, FiltersApplyOnReceive) {
  Fixture f;
  ASSERT_TRUE(f.b->set_filters({Filter::exact(CanId::standard(0x200).value())})
                  .has_value());
  ASSERT_TRUE(f.a->send(data_frame(0x100)).has_value());
  ASSERT_TRUE(f.a->send(data_frame(0x200)).has_value());
  f.medium->drain();

  const auto got = f.b->receive(ms(0));
  ASSERT_TRUE(got.has_value());
  EXPECT_EQ(got.value().id().value(), 0x200u);
  EXPECT_FALSE(f.b->receive(ms(0)).has_value());
  // The unfiltered participant saw both.
  EXPECT_EQ(f.c->available(), 2u);
}

TEST(VirtualBus, ReceiveAdvancesTheClockUpToItsTimeout) {
  Fixture f({500000, 0});
  ASSERT_TRUE(f.a->send(data_frame(0x123, 8)).has_value());
  // Nothing has been transmitted yet, but a receive with a generous timeout
  // lets the medium run, which is what makes this a drop-in for SocketCAN.
  const auto got = f.b->receive(ms(1));
  EXPECT_TRUE(got.has_value());
}

TEST(VirtualBus, ReceiveTimesOutWhenTheBusIsQuiet) {
  Fixture f;
  const auto got = f.b->receive(ms(5));
  ASSERT_FALSE(got.has_value());
  EXPECT_EQ(got.error().code(), core::ErrorCode::TransportTimeout);
}

TEST(VirtualBus, ReceiveQueueOverflowIsCounted) {
  Fixture f({1000000, 0});
  f.medium->set_receive_queue_limit(4);
  f.medium->set_transmit_queue_limit(64);
  for (int i = 0; i < 10; ++i) {
    ASSERT_TRUE(f.a->send(data_frame(0x100)).has_value());
  }
  f.medium->drain();
  EXPECT_EQ(f.b->available(), 4u);
  EXPECT_GE(f.b->statistics().dropped, 6u);
}

TEST(VirtualBus, StatisticsAndBusLoad) {
  Fixture f({500000, 0});
  for (int i = 0; i < 100; ++i) {
    ASSERT_TRUE(f.a->send(data_frame(0x123, 8)).has_value());
  }
  f.medium->drain();
  EXPECT_EQ(f.a->statistics().frames_sent, 100u);
  EXPECT_EQ(f.a->statistics().bytes_sent, 800u);
  EXPECT_EQ(f.a->statistics().wire_bits, 100u * 135u);
  // Back-to-back frames saturate the bus.
  EXPECT_NEAR(f.a->bus_load(), 1.0, 0.02);
}

TEST(VirtualBus, ClosedParticipantsNeitherSendNorReceive) {
  Fixture f;
  f.c->close();
  ASSERT_TRUE(f.a->send(data_frame(0x123)).has_value());
  f.medium->drain();
  EXPECT_EQ(f.c->available(), 0u);
  EXPECT_FALSE(f.c->send(data_frame(0x1)).has_value());
  EXPECT_FALSE(f.c->receive(ms(0)).has_value());
}

TEST(VirtualBus, DetachingIsSafe) {
  auto medium = VirtualMedium::create();
  auto a = medium->attach("A");
  ASSERT_TRUE(a->open().has_value());
  {
    auto temporary = medium->attach("B");
    ASSERT_TRUE(temporary->open().has_value());
    EXPECT_EQ(medium->participant_count(), 2u);
  }
  EXPECT_EQ(medium->participant_count(), 1u);
  ASSERT_TRUE(a->send(data_frame(0x100)).has_value());
  medium->drain();  // must not touch the destroyed participant
  EXPECT_EQ(a->available(), 1u);
}

TEST(VirtualBus, CanFdFramesAreCarried) {
  Fixture f({500000, 2000000});
  const auto fd = FdFrame::make(CanId::standard(0x456).value(), nullptr, 64,
                                FrameFlags::Fd | FrameFlags::Brs)
                      .value();
  ASSERT_TRUE(f.a->send(fd).has_value());
  f.medium->drain();
  const auto got = f.b->receive(ms(0));
  ASSERT_TRUE(got.has_value());
  EXPECT_TRUE(got.value().is_fd());
  EXPECT_TRUE(got.value().is_brs());
  EXPECT_EQ(got.value().size(), 64u);
}

TEST(VirtualBus, ClassicFramesGoThroughTheConvenienceOverload) {
  Fixture f;
  const Frame classic = Frame::make(CanId::standard(0x321).value(), {1, 2, 3}).value();
  ASSERT_TRUE(f.a->send(classic).has_value());
  f.medium->drain();
  const auto got = f.b->receive(ms(0));
  ASSERT_TRUE(got.has_value());
  EXPECT_FALSE(got.value().is_fd());
  EXPECT_EQ(got.value().size(), 3u);
}

TEST(VirtualBus, ReceiveTimestampIsTheEndOfFrame) {
  Fixture f({500000, 0});
  ASSERT_TRUE(f.a->send(data_frame(0x123, 8)).has_value());
  f.medium->drain();
  const auto got = f.b->receive(ms(0));
  ASSERT_TRUE(got.has_value());
  EXPECT_EQ(got.value().timestamp_ns(), 270000u);
}

}  // namespace
}  // namespace canforge::transport
