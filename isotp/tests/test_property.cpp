// SPDX-License-Identifier: MIT
//
// Property-based tests for ISO-TP segmentation: random payload sizes against
// random flow-control parameters. The state machine is deterministic and runs
// on a virtual clock, so ten thousand transfers cost milliseconds.

#include "canforge/isotp/IsoTp.hpp"

#include <gtest/gtest.h>

#include <random>
#include <vector>

namespace canforge::isotp {
namespace {

constexpr std::uint64_t kSeed = 0xC0FFEE1234ULL;

CanId a_id() {
  return CanId::standard(0x7E0).value();
}
CanId b_id() {
  return CanId::standard(0x7E8).value();
}

/// Run one transfer to completion and return the reassembled message.
/// `steps` bounds the loop so a bug cannot hang the suite.
std::vector<std::uint8_t> transfer(const std::vector<std::uint8_t>& payload,
                                   Config sender_config, Config receiver_config,
                                   TransferResult& sender_result,
                                   TransferResult& receiver_result,
                                   int steps = 2000000) {
  Sender sender(sender_config);
  Receiver receiver(receiver_config);
  EXPECT_TRUE(sender.begin(payload, 0).has_value());

  std::uint64_t now = 0;
  // The step has to be finer than the smallest STmin the generator produces,
  // or the sender would appear to miss its own pacing deadline.
  const std::uint64_t step = 50000;  // 50 us
  for (int i = 0; i < steps && !receiver.done(); ++i) {
    for (const FdFrame& f : sender.poll(now)) {
      receiver.on_frame(f, now);
    }
    for (const FdFrame& f : receiver.poll(now)) {
      sender.on_frame(f, now);
    }
    now += step;
  }
  sender_result = sender.result();
  receiver_result = receiver.result();
  return receiver.message();
}

TEST(IsoTpProperty, RandomSizesAgainstRandomFlowControl) {
  std::mt19937_64 rng(kSeed);
  std::uniform_int_distribution<std::size_t> size_dist(1, 3000);
  std::uniform_int_distribution<int> bs_dist(0, 12);
  std::uniform_int_distribution<int> stmin_dist(0, 12);
  std::bernoulli_distribution extended(0.25);
  std::bernoulli_distribution padded(0.5);

  int checked = 0;
  for (int iteration = 0; iteration < 400; ++iteration) {
    const std::size_t size = size_dist(rng);
    std::vector<std::uint8_t> payload(size);
    for (std::size_t i = 0; i < size; ++i) {
      payload[i] = static_cast<std::uint8_t>(rng());
    }

    Config sender_config;
    Config receiver_config;
    if (extended(rng)) {
      sender_config.address = Address::extended(a_id(), b_id(), 0xF1, 0x10);
      receiver_config.address = Address::extended(b_id(), a_id(), 0x10, 0xF1);
    } else {
      sender_config.address = Address::normal(a_id(), b_id());
      receiver_config.address = Address::normal(b_id(), a_id());
    }
    // The receiver dictates the block size and STmin, so those are what the
    // sender has to obey; that is the interaction worth randomising.
    receiver_config.block_size = static_cast<std::uint8_t>(bs_dist(rng));
    static const std::uint8_t kStMin[] = {0x00, 0x01, 0x02, 0x05, 0x0A, 0x14, 0xF1,
                                          0xF3, 0xF5, 0xF9, 0x80, 0xFF, 0x7F};
    receiver_config.st_min_raw = kStMin[static_cast<std::size_t>(stmin_dist(rng))];
    receiver_config.max_receive_length = 100000;
    sender_config.pad_frames = padded(rng);
    receiver_config.pad_frames = sender_config.pad_frames;

    // The reserved and slowest STmin values are 127 ms per frame, so a long
    // transfer would need a minute of virtual time; those are covered by the
    // dedicated STmin tests instead of here.
    const std::uint64_t st_min_ns = st_min_to_ns(receiver_config.st_min_raw);
    if (st_min_ns > 20000000ULL && size > 200) {
      continue;
    }

    TransferResult sender_result = TransferResult::InProgress;
    TransferResult receiver_result = TransferResult::InProgress;
    const std::vector<std::uint8_t> got = transfer(
        payload, sender_config, receiver_config, sender_result, receiver_result);

    ASSERT_EQ(receiver_result, TransferResult::Ok)
        << "size " << size << " bs " << int{receiver_config.block_size} << " stmin 0x"
        << std::hex << int{receiver_config.st_min_raw} << " -> "
        << to_string(receiver_result);
    ASSERT_EQ(sender_result, TransferResult::Ok);
    ASSERT_EQ(got.size(), payload.size()) << "size " << size;
    ASSERT_EQ(got, payload) << "size " << size;
    ++checked;
  }
  EXPECT_GT(checked, 300) << "too many combinations were skipped";
}

TEST(IsoTpProperty, EverySizeAcrossTheSingleFrameBoundary) {
  // The boundaries where the encoding changes shape: single frame to first
  // frame, and the first frame's payload split.
  Config sender_config;
  sender_config.address = Address::normal(a_id(), b_id());
  Config receiver_config;
  receiver_config.address = Address::normal(b_id(), a_id());
  receiver_config.block_size = 3;

  for (std::size_t size = 1; size <= 40; ++size) {
    std::vector<std::uint8_t> payload(size);
    for (std::size_t i = 0; i < size; ++i) {
      payload[i] = static_cast<std::uint8_t>(size * 7u + i);
    }
    TransferResult s = TransferResult::InProgress;
    TransferResult r = TransferResult::InProgress;
    const auto got = transfer(payload, sender_config, receiver_config, s, r);
    ASSERT_EQ(r, TransferResult::Ok) << "size " << size;
    ASSERT_EQ(got, payload) << "size " << size;
  }
}

TEST(IsoTpProperty, SizesAroundTheEscapeBoundary) {
  // FF_DL is 12 bits, so 4095 is the last length it can express and 4096 must
  // switch to the 32-bit escape. Off-by-one here loses a byte or a whole
  // transfer.
  Config sender_config;
  sender_config.address = Address::normal(a_id(), b_id());
  Config receiver_config;
  receiver_config.address = Address::normal(b_id(), a_id());
  receiver_config.block_size = 0;
  receiver_config.max_receive_length = 20000;

  for (const std::size_t size :
       {std::size_t{4094}, std::size_t{4095}, std::size_t{4096}, std::size_t{4097},
        std::size_t{8000}}) {
    std::vector<std::uint8_t> payload(size);
    for (std::size_t i = 0; i < size; ++i) {
      payload[i] = static_cast<std::uint8_t>(i * 13u + 5u);
    }
    TransferResult s = TransferResult::InProgress;
    TransferResult r = TransferResult::InProgress;
    const auto got = transfer(payload, sender_config, receiver_config, s, r);
    ASSERT_EQ(r, TransferResult::Ok) << "size " << size;
    ASSERT_EQ(got.size(), size) << "size " << size;
    ASSERT_EQ(got, payload) << "size " << size;
  }
}

TEST(IsoTpProperty, BlockSizeDoesNotChangeTheResult) {
  // The same payload through every block size must reassemble identically:
  // block size is a flow-control decision, not a framing one.
  Config sender_config;
  sender_config.address = Address::normal(a_id(), b_id());
  std::vector<std::uint8_t> payload(777);
  for (std::size_t i = 0; i < payload.size(); ++i) {
    payload[i] = static_cast<std::uint8_t>(i * 3u + 1u);
  }
  for (int bs = 0; bs <= 16; ++bs) {
    Config receiver_config;
    receiver_config.address = Address::normal(b_id(), a_id());
    receiver_config.block_size = static_cast<std::uint8_t>(bs);
    TransferResult s = TransferResult::InProgress;
    TransferResult r = TransferResult::InProgress;
    const auto got = transfer(payload, sender_config, receiver_config, s, r);
    ASSERT_EQ(r, TransferResult::Ok) << "block size " << bs;
    ASSERT_EQ(got, payload) << "block size " << bs;
  }
}

TEST(IsoTpProperty, StMinIsRespectedForEveryEncoding) {
  Config sender_config;
  sender_config.address = Address::normal(a_id(), b_id());
  for (const std::uint8_t raw :
       {std::uint8_t{0x00}, std::uint8_t{0x01}, std::uint8_t{0x05}, std::uint8_t{0x0A},
        std::uint8_t{0xF1}, std::uint8_t{0xF5}, std::uint8_t{0xF9}}) {
    Config receiver_config;
    receiver_config.address = Address::normal(b_id(), a_id());
    receiver_config.block_size = 0;
    receiver_config.st_min_raw = raw;

    Sender sender(sender_config);
    ASSERT_TRUE(sender.begin(std::vector<std::uint8_t>(200, 0xAB), 0).has_value());
    sender.poll(0);  // first frame
    std::array<std::uint8_t, 8> fc = {0x30, 0x00, raw, 0, 0, 0, 0, 0};
    sender.on_frame(FdFrame::make(b_id(), fc.data(), fc.size()).value(), 0);

    const std::uint64_t expected = st_min_to_ns(raw);
    std::uint64_t now = 0;
    ASSERT_EQ(sender.poll(now).size(), 1u) << "raw 0x" << std::hex << int{raw};
    if (expected == 0) {
      continue;
    }
    // One nanosecond short must not be enough; exactly STmin must be.
    EXPECT_TRUE(sender.poll(now + expected - 1).empty())
        << "raw 0x" << std::hex << int{raw};
    EXPECT_EQ(sender.poll(now + expected).size(), 1u)
        << "raw 0x" << std::hex << int{raw};
  }
}

}  // namespace
}  // namespace canforge::isotp
