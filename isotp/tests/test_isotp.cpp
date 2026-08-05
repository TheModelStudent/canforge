// SPDX-License-Identifier: MIT
//
// ISO 15765-2 tests.
//
// Because the layer is a pure state machine driven by an explicit timestamp,
// every timeout is exercised by moving a variable instead of by sleeping:
// the whole file runs in milliseconds and is completely deterministic.

#include "canforge/isotp/IsoTp.hpp"

#include <gtest/gtest.h>

#include <numeric>
#include <vector>

#include "canforge/transport/VirtualBus.hpp"

namespace canforge::isotp {
namespace {

constexpr std::uint64_t kUs = 1000ULL;
constexpr std::uint64_t kMs = 1000000ULL;

CanId tx_id() { return CanId::standard(0x7E0).value(); }
CanId rx_id() { return CanId::standard(0x7E8).value(); }

Config sender_config() {
  Config c;
  c.address = Address::normal(tx_id(), rx_id());
  return c;
}
Config receiver_config() {
  Config c;
  c.address = Address::normal(rx_id(), tx_id());
  return c;
}

std::vector<std::uint8_t> pattern(std::size_t n) {
  std::vector<std::uint8_t> out(n);
  for (std::size_t i = 0; i < n; ++i) {
    out[i] = static_cast<std::uint8_t>((i * 31u + 7u) & 0xFFu);
  }
  return out;
}

/// Run a sender and a receiver against each other on a virtual wire, stepping
/// a virtual clock. Returns the number of frames that crossed.
struct Link {
  Sender sender;
  Receiver receiver;
  std::uint64_t now = 0;
  std::size_t frames = 0;

  Link(Config s, Config r) : sender(s), receiver(r) {}

  void run(std::uint64_t step_ns = 100 * kUs, std::uint64_t limit_ns = 60000 * kMs) {
    while (!sender.done() && !receiver.done() && now < limit_ns) {
      for (const FdFrame& f : sender.poll(now)) {
        ++frames;
        receiver.on_frame(f, now);
      }
      for (const FdFrame& f : receiver.poll(now)) {
        ++frames;
        sender.on_frame(f, now);
      }
      now += step_ns;
    }
    // Let whichever side is still open finish.
    for (int i = 0; i < 4; ++i) {
      for (const FdFrame& f : sender.poll(now)) {
        ++frames;
        receiver.on_frame(f, now);
      }
      for (const FdFrame& f : receiver.poll(now)) {
        ++frames;
        sender.on_frame(f, now);
      }
      now += step_ns;
    }
  }
};

TEST(StMin, MillisecondRange) {
  EXPECT_EQ(st_min_to_ns(0x00), 0u);
  EXPECT_EQ(st_min_to_ns(0x01), 1 * kMs);
  EXPECT_EQ(st_min_to_ns(0x0A), 10 * kMs);
  EXPECT_EQ(st_min_to_ns(0x7F), 127 * kMs);
}

TEST(StMin, SubMillisecondRange) {
  // 0xF1..0xF9 are 100..900 microseconds. Implementations that ignore this
  // range are 10x slower than they need to be on a fast bus.
  EXPECT_EQ(st_min_to_ns(0xF1), 100 * kUs);
  EXPECT_EQ(st_min_to_ns(0xF5), 500 * kUs);
  EXPECT_EQ(st_min_to_ns(0xF9), 900 * kUs);
}

TEST(StMin, ReservedValuesBecomeTheSlowestRate) {
  // ISO says a reserved value shall be handled as 0x7F, not rejected.
  for (const int raw : {0x80, 0x90, 0xF0, 0xFA, 0xFF}) {
    EXPECT_EQ(st_min_to_ns(static_cast<std::uint8_t>(raw)), 127 * kMs)
        << "raw 0x" << std::hex << raw;
  }
}

TEST(StMin, RoundTrip) {
  for (std::uint8_t raw = 0; raw <= 0x7F; ++raw) {
    EXPECT_EQ(ns_to_st_min(st_min_to_ns(raw)), raw) << int{raw};
  }
  for (std::uint8_t raw = 0xF1; raw <= 0xF9; ++raw) {
    EXPECT_EQ(ns_to_st_min(st_min_to_ns(raw)), raw) << int{raw};
  }
}

TEST(Config, EnforcesTheIsoInequalities) {
  Config c = sender_config();
  EXPECT_TRUE(c.validate().has_value());

  // The ISO defaults must themselves validate: a check that rejects them is
  // the classic sign of substituting the timeout maxima into an inequality
  // that constrains measured durations.
  c.n_br_ns = 0;
  c.n_cs_ns = 0;
  EXPECT_TRUE(c.validate().has_value());

  // An N_Br that eats the whole N_Bs budget leaves the link layer nothing.
  c.n_br_ns = 900 * kMs;
  auto bad = c.validate();
  ASSERT_FALSE(bad.has_value());
  EXPECT_NE(std::string(bad.error().message()).find("N_Br"), std::string::npos);

  c = sender_config();
  c.n_cs_ns = 900 * kMs;
  bad = c.validate();
  ASSERT_FALSE(bad.has_value());
  EXPECT_NE(std::string(bad.error().message()).find("N_Cs"), std::string::npos);

  // A comfortable N_Br is fine.
  c = sender_config();
  c.n_br_ns = 20 * kMs;
  c.n_cs_ns = 5 * kMs;
  EXPECT_TRUE(c.validate().has_value());
}

TEST(IsoTp, SingleFrame) {
  Link link(sender_config(), receiver_config());
  const auto payload = pattern(6);
  ASSERT_TRUE(link.sender.begin(payload, 0).has_value());
  link.run();

  EXPECT_EQ(link.sender.result(), TransferResult::Ok);
  EXPECT_EQ(link.receiver.result(), TransferResult::Ok);
  EXPECT_EQ(link.receiver.message(), payload);
  EXPECT_EQ(link.frames, 1u) << "no flow control for a single frame";
}

TEST(IsoTp, SingleFrameAtTheBoundary) {
  for (std::size_t n = 1; n <= 7; ++n) {
    Link link(sender_config(), receiver_config());
    const auto payload = pattern(n);
    ASSERT_TRUE(link.sender.begin(payload, 0).has_value());
    link.run();
    EXPECT_EQ(link.receiver.message(), payload) << "length " << n;
    EXPECT_EQ(link.frames, 1u) << "length " << n << " should still be one frame";
  }
  // Eight bytes no longer fits and must be segmented.
  Link link(sender_config(), receiver_config());
  ASSERT_TRUE(link.sender.begin(pattern(8), 0).has_value());
  link.run();
  EXPECT_EQ(link.receiver.message(), pattern(8));
  EXPECT_GT(link.frames, 1u);
}

TEST(IsoTp, FourThousandBytesArriveByteExact) {
  // The headline case: a 4000-byte transfer, well past a single block, with
  // sequence numbers wrapping 16 times over.
  Link link(sender_config(), receiver_config());
  const auto payload = pattern(4000);
  ASSERT_TRUE(link.sender.begin(payload, 0).has_value());
  link.run();

  ASSERT_EQ(link.sender.result(), TransferResult::Ok)
      << to_string(link.sender.result());
  ASSERT_EQ(link.receiver.result(), TransferResult::Ok)
      << to_string(link.receiver.result());
  EXPECT_EQ(link.receiver.expected_length(), 4000u);
  ASSERT_EQ(link.receiver.message().size(), 4000u);
  EXPECT_EQ(link.receiver.message(), payload) << "byte-exact reassembly";
  // 1 FF + 572 CF, plus one FC per block of 8.
  EXPECT_GT(link.frames, 600u);
}

TEST(IsoTp, EveryLengthUpToOneBlockRoundTrips) {
  for (const std::size_t n : {std::size_t{8}, std::size_t{9}, std::size_t{13},
                              std::size_t{14}, std::size_t{20}, std::size_t{62},
                              std::size_t{63}, std::size_t{64}, std::size_t{100},
                              std::size_t{255}, std::size_t{256}}) {
    Link link(sender_config(), receiver_config());
    const auto payload = pattern(n);
    ASSERT_TRUE(link.sender.begin(payload, 0).has_value());
    link.run();
    ASSERT_EQ(link.receiver.result(), TransferResult::Ok) << "length " << n;
    EXPECT_EQ(link.receiver.message(), payload) << "length " << n;
  }
}

TEST(IsoTp, MaximumClassicLength) {
  Link link(sender_config(), receiver_config());
  const auto payload = pattern(kClassicMaxLength);
  ASSERT_TRUE(link.sender.begin(payload, 0).has_value());
  link.run();
  ASSERT_EQ(link.receiver.result(), TransferResult::Ok);
  EXPECT_EQ(link.receiver.message().size(), kClassicMaxLength);
  EXPECT_EQ(link.receiver.message(), payload);
}

TEST(IsoTp, EscapeSequenceAboveFourThousandNinetyFive) {
  // FF_DL of zero followed by a 32-bit length. Without this a transfer larger
  // than 4095 bytes is simply impossible, and firmware images are larger.
  Config r = receiver_config();
  r.max_receive_length = 100000;
  Link link(sender_config(), r);
  const auto payload = pattern(10000);
  ASSERT_TRUE(link.sender.begin(payload, 0).has_value());
  link.run();

  ASSERT_EQ(link.receiver.result(), TransferResult::Ok)
      << to_string(link.receiver.result());
  EXPECT_EQ(link.receiver.expected_length(), 10000u);
  EXPECT_EQ(link.receiver.message(), payload);
}

TEST(IsoTp, BlockSizeZeroMeansSendEverything) {
  Config r = receiver_config();
  r.block_size = 0;
  Link link(sender_config(), r);
  const auto payload = pattern(500);
  ASSERT_TRUE(link.sender.begin(payload, 0).has_value());
  link.run();
  ASSERT_EQ(link.receiver.result(), TransferResult::Ok);
  EXPECT_EQ(link.receiver.message(), payload);
  // One First Frame, one Flow Control, then nothing but Consecutive Frames.
  EXPECT_EQ(link.frames, 1u + 1u + 71u);
}

TEST(IsoTp, SequenceNumbersWrapCorrectly) {
  // 16 consecutive frames take the sequence number through its full range and
  // back to zero, which is the classic off-by-one.
  Config r = receiver_config();
  r.block_size = 0;
  Link link(sender_config(), r);
  const auto payload = pattern(6 + 17 * 7);
  ASSERT_TRUE(link.sender.begin(payload, 0).has_value());
  link.run();
  ASSERT_EQ(link.receiver.result(), TransferResult::Ok);
  EXPECT_EQ(link.receiver.message(), payload);
}

TEST(IsoTp, ExtendedAddressing) {
  Config s = sender_config();
  s.address = Address::extended(tx_id(), rx_id(), 0xF1, 0x10);
  Config r = receiver_config();
  r.address = Address::extended(rx_id(), tx_id(), 0x10, 0xF1);

  Link link(s, r);
  const auto payload = pattern(300);
  ASSERT_TRUE(link.sender.begin(payload, 0).has_value());
  link.run();
  ASSERT_EQ(link.receiver.result(), TransferResult::Ok);
  EXPECT_EQ(link.receiver.message(), payload);
}

TEST(IsoTp, ExtendedAddressingSingleFrameHoldsOneByteLess) {
  Config s = sender_config();
  s.address = Address::extended(tx_id(), rx_id(), 0xF1, 0x10);
  Config r = receiver_config();
  r.address = Address::extended(rx_id(), tx_id(), 0x10, 0xF1);

  Link six(s, r);
  ASSERT_TRUE(six.sender.begin(pattern(6), 0).has_value());
  six.run();
  EXPECT_EQ(six.frames, 1u) << "six bytes still fit in a single frame";

  Link seven(s, r);
  ASSERT_TRUE(seven.sender.begin(pattern(7), 0).has_value());
  seven.run();
  EXPECT_GT(seven.frames, 1u) << "the address extension costs one byte";
  EXPECT_EQ(seven.receiver.message(), pattern(7));
}

TEST(IsoTp, ExtendedAddressingIgnoresAnotherPeer) {
  Config r = receiver_config();
  r.address = Address::extended(rx_id(), tx_id(), 0x10, 0xF1);
  Receiver receiver(r);

  // Same identifier, wrong address extension: not for us.
  std::array<std::uint8_t, 8> body = {0xF2, 0x03, 1, 2, 3, 0, 0, 0};
  receiver.on_frame(FdFrame::make(tx_id(), body.data(), body.size()).value(), 0);
  EXPECT_EQ(receiver.result(), TransferResult::InProgress);
  EXPECT_TRUE(receiver.message().empty());

  body[0] = 0xF1;
  receiver.on_frame(FdFrame::make(tx_id(), body.data(), body.size()).value(), 0);
  EXPECT_EQ(receiver.result(), TransferResult::Ok);
  EXPECT_EQ(receiver.message(), (std::vector<std::uint8_t>{1, 2, 3}));
}

TEST(IsoTp, PaddingIsApplied) {
  Sender sender(sender_config());
  ASSERT_TRUE(sender.begin(pattern(3), 0).has_value());
  const auto frames = sender.poll(0);
  ASSERT_EQ(frames.size(), 1u);
  EXPECT_EQ(frames[0].size(), 8u) << "classic CAN frames are padded to 8";
  EXPECT_EQ(frames[0].data()[4], 0xCC);
  EXPECT_EQ(frames[0].data()[7], 0xCC);
}

TEST(IsoTp, PaddingCanBeTurnedOff) {
  Config c = sender_config();
  c.pad_frames = false;
  Sender sender(c);
  ASSERT_TRUE(sender.begin(pattern(3), 0).has_value());
  const auto frames = sender.poll(0);
  ASSERT_EQ(frames.size(), 1u);
  EXPECT_EQ(frames[0].size(), 4u) << "PCI plus three data bytes";
}

TEST(IsoTp, NBsTimesOutWhenNoFlowControlArrives) {
  Sender sender(sender_config());
  ASSERT_TRUE(sender.begin(pattern(100), 0).has_value());
  const auto first = sender.poll(0);
  ASSERT_EQ(first.size(), 1u) << "the First Frame goes out";

  // Just under the timeout: still waiting.
  EXPECT_TRUE(sender.poll(999 * kMs).empty());
  EXPECT_EQ(sender.result(), TransferResult::InProgress);

  // At the timeout: give up.
  sender.poll(1000 * kMs);
  EXPECT_EQ(sender.result(), TransferResult::TimeoutBs);
  EXPECT_TRUE(sender.done());
}

TEST(IsoTp, NBsAlsoAppliesBetweenBlocks) {
  Config s = sender_config();
  Sender sender(s);
  Config r = receiver_config();
  r.block_size = 2;
  Receiver receiver(r);

  ASSERT_TRUE(sender.begin(pattern(200), 0).has_value());
  std::uint64_t now = 0;
  // Run until a block has completed and the sender is stalled waiting for the
  // next Flow Control -- that is the window N_Bs covers.
  int rounds = 0;
  while (!sender.waiting_for_flow_control() || rounds < 3) {
    for (const FdFrame& f : sender.poll(now)) {
      receiver.on_frame(f, now);
    }
    if (rounds < 3) {
      for (const FdFrame& f : receiver.poll(now)) {
        sender.on_frame(f, now);
      }
    }
    now += kMs;
    ++rounds;
    ASSERT_LT(rounds, 200);
  }
  ASSERT_EQ(sender.result(), TransferResult::InProgress);
  ASSERT_TRUE(sender.waiting_for_flow_control());
  ASSERT_GT(sender.sent_bytes(), 6u) << "at least one block went out";

  // Stop answering: the sender must time out waiting for the next FC. The
  // stall began a round or two before `now`, so the bounds are loose either
  // side of the 1000 ms N_Bs rather than exact.
  sender.poll(now + 900 * kMs);
  EXPECT_EQ(sender.result(), TransferResult::InProgress) << "short of N_Bs";
  sender.poll(now + 1100 * kMs);
  EXPECT_EQ(sender.result(), TransferResult::TimeoutBs);
}

TEST(IsoTp, NCrTimesOutWhenNoConsecutiveFrameArrives) {
  Receiver receiver(receiver_config());
  // First Frame announcing 100 bytes.
  std::array<std::uint8_t, 8> ff = {0x10, 0x64, 1, 2, 3, 4, 5, 6};
  receiver.on_frame(FdFrame::make(tx_id(), ff.data(), ff.size()).value(), 0);
  const auto fc = receiver.poll(0);
  ASSERT_EQ(fc.size(), 1u) << "a flow control should have been produced";

  EXPECT_TRUE(receiver.poll(999 * kMs).empty());
  EXPECT_EQ(receiver.result(), TransferResult::InProgress);
  receiver.poll(1000 * kMs);
  EXPECT_EQ(receiver.result(), TransferResult::TimeoutCr);
}

TEST(IsoTp, StMinIsHonouredToTheMicrosecond) {
  Config s = sender_config();
  Sender sender(s);
  ASSERT_TRUE(sender.begin(pattern(100), 0).has_value());
  sender.poll(0);  // First Frame

  // Flow control asking for 500 us between frames, no block limit.
  std::array<std::uint8_t, 8> fc = {0x30, 0x00, 0xF5, 0, 0, 0, 0, 0};
  sender.on_frame(FdFrame::make(rx_id(), fc.data(), fc.size()).value(), 0);

  std::uint64_t now = 0;
  EXPECT_EQ(sender.poll(now).size(), 1u) << "the first CF goes immediately";
  now += 400 * kUs;
  EXPECT_TRUE(sender.poll(now).empty()) << "400 us is short of STmin";
  now += 100 * kUs;
  EXPECT_EQ(sender.poll(now).size(), 1u) << "500 us is exactly STmin";
}

TEST(IsoTp, StMinMillisecondsAreHonoured) {
  Sender sender(sender_config());
  ASSERT_TRUE(sender.begin(pattern(100), 0).has_value());
  sender.poll(0);
  std::array<std::uint8_t, 8> fc = {0x30, 0x00, 0x14, 0, 0, 0, 0, 0};  // 20 ms
  sender.on_frame(FdFrame::make(rx_id(), fc.data(), fc.size()).value(), 0);

  std::uint64_t now = 0;
  EXPECT_EQ(sender.poll(now).size(), 1u);
  now += 19 * kMs;
  EXPECT_TRUE(sender.poll(now).empty());
  now += 1 * kMs;
  EXPECT_EQ(sender.poll(now).size(), 1u);
}

TEST(IsoTp, NBrDelaysTheFlowControl) {
  Config r = receiver_config();
  r.n_br_ns = 5 * kMs;
  Receiver receiver(r);
  std::array<std::uint8_t, 8> ff = {0x10, 0x64, 1, 2, 3, 4, 5, 6};
  receiver.on_frame(FdFrame::make(tx_id(), ff.data(), ff.size()).value(), 0);

  EXPECT_TRUE(receiver.poll(4 * kMs).empty()) << "N_Br has not elapsed";
  EXPECT_EQ(receiver.poll(5 * kMs).size(), 1u);
}

TEST(IsoTp, WrongSequenceNumberAborts) {
  Receiver receiver(receiver_config());
  std::array<std::uint8_t, 8> ff = {0x10, 0x64, 1, 2, 3, 4, 5, 6};
  receiver.on_frame(FdFrame::make(tx_id(), ff.data(), ff.size()).value(), 0);
  receiver.poll(0);

  // Sequence numbers must run 1, 2, 3...; jumping to 2 means one was lost.
  std::array<std::uint8_t, 8> cf = {0x22, 9, 9, 9, 9, 9, 9, 9};
  receiver.on_frame(FdFrame::make(tx_id(), cf.data(), cf.size()).value(), kMs);
  EXPECT_EQ(receiver.result(), TransferResult::WrongSequence);
}

TEST(IsoTp, UnexpectedConsecutiveFrameAborts) {
  Receiver receiver(receiver_config());
  std::array<std::uint8_t, 8> cf = {0x21, 1, 2, 3, 4, 5, 6, 7};
  receiver.on_frame(FdFrame::make(tx_id(), cf.data(), cf.size()).value(), 0);
  EXPECT_EQ(receiver.result(), TransferResult::UnexpectedPdu);
}

TEST(IsoTp, UnexpectedFlowControlAtAReceiverAborts) {
  Receiver receiver(receiver_config());
  std::array<std::uint8_t, 8> fc = {0x30, 0, 0, 0, 0, 0, 0, 0};
  receiver.on_frame(FdFrame::make(tx_id(), fc.data(), fc.size()).value(), 0);
  EXPECT_EQ(receiver.result(), TransferResult::UnexpectedPdu);
}

TEST(IsoTp, UnexpectedSingleFrameAtASenderAborts) {
  Sender sender(sender_config());
  ASSERT_TRUE(sender.begin(pattern(100), 0).has_value());
  sender.poll(0);
  std::array<std::uint8_t, 8> sf = {0x02, 1, 2, 0, 0, 0, 0, 0};
  sender.on_frame(FdFrame::make(rx_id(), sf.data(), sf.size()).value(), 0);
  EXPECT_EQ(sender.result(), TransferResult::UnexpectedPdu);
}

TEST(IsoTp, OverflowIsReportedToBothSides) {
  Config r = receiver_config();
  r.max_receive_length = 100;
  Link link(sender_config(), r);
  ASSERT_TRUE(link.sender.begin(pattern(500), 0).has_value());
  link.run();

  EXPECT_EQ(link.receiver.result(), TransferResult::BufferOverflow);
  EXPECT_EQ(link.sender.result(), TransferResult::BufferOverflow)
      << "the sender must learn about it from FC.OVERFLOW";
}

TEST(IsoTp, WaitFramesExtendTheTimeoutUpToWftMax) {
  Config s = sender_config();
  s.wft_max = 3;
  Sender sender(s);
  ASSERT_TRUE(sender.begin(pattern(100), 0).has_value());
  sender.poll(0);

  std::array<std::uint8_t, 8> wait = {0x31, 0, 0, 0, 0, 0, 0, 0};
  std::uint64_t now = 0;
  for (int i = 0; i < 3; ++i) {
    now += 500 * kMs;
    sender.on_frame(FdFrame::make(rx_id(), wait.data(), wait.size()).value(), now);
    sender.poll(now);
    EXPECT_EQ(sender.result(), TransferResult::InProgress)
        << "wait frame " << i << " should extend N_Bs";
  }
  // The fourth exceeds wft_max.
  now += 500 * kMs;
  sender.on_frame(FdFrame::make(rx_id(), wait.data(), wait.size()).value(), now);
  EXPECT_EQ(sender.result(), TransferResult::WaitOverrun);
}

TEST(IsoTp, WaitThenContinueCompletesTheTransfer) {
  Config s = sender_config();
  Sender sender(s);
  Receiver receiver(receiver_config());
  const auto payload = pattern(60);
  ASSERT_TRUE(sender.begin(payload, 0).has_value());
  for (const FdFrame& f : sender.poll(0)) {
    receiver.on_frame(f, 0);
  }

  std::array<std::uint8_t, 8> wait = {0x31, 0, 0, 0, 0, 0, 0, 0};
  sender.on_frame(FdFrame::make(rx_id(), wait.data(), wait.size()).value(), kMs);
  EXPECT_EQ(sender.result(), TransferResult::InProgress);

  // Now let the real receiver take over.
  std::uint64_t now = 2 * kMs;
  for (int i = 0; i < 200 && !sender.done(); ++i) {
    for (const FdFrame& f : receiver.poll(now)) {
      sender.on_frame(f, now);
    }
    for (const FdFrame& f : sender.poll(now)) {
      receiver.on_frame(f, now);
    }
    now += kMs;
  }
  EXPECT_EQ(sender.result(), TransferResult::Ok);
  EXPECT_EQ(receiver.message(), payload);
}

TEST(IsoTp, MalformedFirstFrameIsRejected) {
  Receiver receiver(receiver_config());
  // Announces four bytes, which would have fitted in a single frame.
  std::array<std::uint8_t, 8> ff = {0x10, 0x04, 1, 2, 3, 4, 5, 6};
  receiver.on_frame(FdFrame::make(tx_id(), ff.data(), ff.size()).value(), 0);
  EXPECT_EQ(receiver.result(), TransferResult::InvalidPdu);
}

TEST(IsoTp, SingleFrameWithABadLengthIsRejected) {
  Receiver receiver(receiver_config());
  std::array<std::uint8_t, 3> sf = {0x07, 1, 2};  // claims 7 bytes, carries 2
  receiver.on_frame(FdFrame::make(tx_id(), sf.data(), sf.size()).value(), 0);
  EXPECT_EQ(receiver.result(), TransferResult::InvalidPdu);
}

TEST(IsoTp, FramesOnAnotherIdentifierAreIgnored) {
  Receiver receiver(receiver_config());
  std::array<std::uint8_t, 8> sf = {0x03, 1, 2, 3, 0, 0, 0, 0};
  const CanId other = CanId::standard(0x123).value();
  receiver.on_frame(FdFrame::make(other, sf.data(), sf.size()).value(), 0);
  EXPECT_EQ(receiver.result(), TransferResult::InProgress);
  EXPECT_TRUE(receiver.message().empty());
}

TEST(IsoTp, ReceiverCanBeReusedForTheNextMessage) {
  Receiver receiver(receiver_config());
  std::array<std::uint8_t, 8> sf = {0x02, 0xAA, 0xBB, 0, 0, 0, 0, 0};
  receiver.on_frame(FdFrame::make(tx_id(), sf.data(), sf.size()).value(), 0);
  ASSERT_EQ(receiver.result(), TransferResult::Ok);
  EXPECT_EQ(receiver.message(), (std::vector<std::uint8_t>{0xAA, 0xBB}));

  receiver.reset();
  EXPECT_EQ(receiver.result(), TransferResult::InProgress);
  sf[1] = 0xCC;
  receiver.on_frame(FdFrame::make(tx_id(), sf.data(), sf.size()).value(), kMs);
  EXPECT_EQ(receiver.message(), (std::vector<std::uint8_t>{0xCC, 0xBB}));
}

TEST(IsoTp, FourThousandBytesAcrossTheVirtualBus) {
  auto medium = transport::VirtualMedium::create({500000, 0});
  auto tester = medium->attach("Tester");
  auto ecu = medium->attach("ECU");
  ASSERT_TRUE(tester->open().has_value());
  ASSERT_TRUE(ecu->open().has_value());
  medium->set_loopback(false);  // a node does not hear its own frames

  Sender sender(sender_config());
  Receiver receiver(receiver_config());
  const auto payload = pattern(4000);
  ASSERT_TRUE(sender.begin(payload, medium->now_ns()).has_value());

  // The loop runs until the *receiver* is done: stopping when the sender
  // finishes would leave the last frame still on the wire.
  for (int round = 0; round < 200000 && !receiver.done(); ++round) {
    const std::uint64_t now = medium->now_ns();
    for (const FdFrame& f : sender.poll(now)) {
      ASSERT_TRUE(tester->send(f).has_value());
    }
    for (const FdFrame& f : receiver.poll(now)) {
      ASSERT_TRUE(ecu->send(f).has_value());
    }
    medium->advance(std::chrono::microseconds(200));
    while (auto got = tester->receive(std::chrono::nanoseconds(0))) {
      sender.on_frame(got.value(), medium->now_ns());
    }
    while (auto got = ecu->receive(std::chrono::nanoseconds(0))) {
      receiver.on_frame(got.value(), medium->now_ns());
    }
  }

  EXPECT_EQ(sender.result(), TransferResult::Ok) << to_string(sender.result());
  EXPECT_EQ(receiver.result(), TransferResult::Ok) << to_string(receiver.result());
  EXPECT_EQ(receiver.message(), payload);
  // Sanity on the timing model: 573 frames of about 135 bits at 500 kbit/s is
  // at least 150 ms of bus time.
  EXPECT_GT(medium->now_ns(), 150ULL * kMs);
}

}  // namespace
}  // namespace canforge::isotp
