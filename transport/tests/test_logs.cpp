// SPDX-License-Identifier: MIT
#include "canforge/transport/LogFormat.hpp"
#include "canforge/transport/LogReplayBus.hpp"

#include <gtest/gtest.h>

#include <cstdio>
#include <string>
#include <vector>

namespace canforge::transport {
namespace {

using core::CanId;
using core::FdFrame;
using core::FrameFlags;
using ms = std::chrono::milliseconds;

std::string data_path(const std::string& name) {
  return std::string(CANFORGE_TRANSPORT_TEST_DATA) + "/" + name;
}
std::string temp_path(const std::string& name) {
  return std::string(CANFORGE_TRANSPORT_TEST_DATA) + "/tmp_" + name;
}

TEST(Candump, ParsesEveryFrameShape) {
  const auto standard = parse_candump_line("(1735689600.000000) vcan0 123#DEADBEEF");
  ASSERT_TRUE(standard.has_value()) << standard.error().message();
  EXPECT_EQ(standard.value().channel, "vcan0");
  EXPECT_EQ(standard.value().frame.id().value(), 0x123u);
  EXPECT_FALSE(standard.value().frame.id().is_extended());
  EXPECT_EQ(standard.value().frame.size(), 4u);
  EXPECT_EQ(standard.value().frame.data()[0], 0xDE);
  EXPECT_EQ(standard.value().frame.timestamp_ns(), 1735689600000000000ULL);

  // Eight hex digits mean extended; the width is the only signal.
  const auto extended =
      parse_candump_line("(1735689600.010000) vcan0 18FEF100#0102030405060708");
  ASSERT_TRUE(extended.has_value());
  EXPECT_TRUE(extended.value().frame.id().is_extended());
  EXPECT_EQ(extended.value().frame.id().value(), 0x18FEF100u);

  const auto empty = parse_candump_line("(1735689600.020000) vcan0 7FF#");
  ASSERT_TRUE(empty.has_value());
  EXPECT_EQ(empty.value().frame.size(), 0u);

  const auto fd =
      parse_candump_line("(1735689600.030000) vcan0 123##1DEADBEEFCAFE1234");
  ASSERT_TRUE(fd.has_value());
  EXPECT_TRUE(fd.value().frame.is_fd());
  EXPECT_TRUE(fd.value().frame.is_brs());
  EXPECT_FALSE(fd.value().frame.is_esi());
  EXPECT_EQ(fd.value().frame.size(), 8u);

  const auto remote = parse_candump_line("(1735689600.040000) vcan0 456#R8");
  ASSERT_TRUE(remote.has_value());
  EXPECT_TRUE(remote.value().frame.is_remote());
  EXPECT_EQ(remote.value().frame.dlc(), 8u);

  const auto error =
      parse_candump_line("(1735689600.050000) vcan1 20000004#0000000000000000");
  ASSERT_TRUE(error.has_value());
  EXPECT_TRUE(error.value().frame.is_error_frame());
}

TEST(Candump, RejectsMalformedLines) {
  EXPECT_FALSE(parse_candump_line("").has_value());
  EXPECT_FALSE(parse_candump_line("nonsense").has_value());
  EXPECT_FALSE(parse_candump_line("(1.0) vcan0 123").has_value());
  EXPECT_FALSE(parse_candump_line("1.0 vcan0 123#00").has_value());
  EXPECT_FALSE(parse_candump_line("(1.0) vcan0 ZZZ#00").has_value());
  EXPECT_FALSE(parse_candump_line("(1.0) vcan0 123#0").has_value())
      << "an odd number of hex digits is not a whole byte";
  EXPECT_FALSE(parse_candump_line("(1.0) vcan0 123#00112233445566778899").has_value())
      << "a classic frame cannot hold ten bytes";
}

TEST(Candump, ReadsTheSampleFile) {
  auto reader = open_reader(data_path("sample.log"));
  ASSERT_TRUE(reader.has_value());
  auto records = reader.value()->read_all();
  ASSERT_TRUE(records.has_value());
  ASSERT_EQ(records.value().size(), 6u);
  EXPECT_EQ(records.value()[0].frame.id().value(), 0x123u);
  EXPECT_TRUE(records.value()[1].frame.id().is_extended());
  EXPECT_TRUE(records.value()[3].frame.is_fd());
  EXPECT_TRUE(records.value()[4].frame.is_remote());
  EXPECT_EQ(records.value()[5].channel, "vcan1");
}

TEST(Candump, RoundTripsThroughTheWriter) {
  auto reader = open_reader(data_path("sample.log"));
  ASSERT_TRUE(reader.has_value());
  const auto original = reader.value()->read_all().value();

  const std::string out = temp_path("roundtrip.log");
  {
    auto writer = open_writer(out, LogFormat::Candump);
    ASSERT_TRUE(writer.has_value());
    for (const LogRecord& r : original) {
      ASSERT_TRUE(writer.value()->write(r).has_value());
    }
    ASSERT_TRUE(writer.value()->finish().has_value());
  }
  auto again = open_reader(out, LogFormat::Candump);
  ASSERT_TRUE(again.has_value());
  const auto reread = again.value()->read_all().value();

  ASSERT_EQ(reread.size(), original.size());
  for (std::size_t i = 0; i < original.size(); ++i) {
    EXPECT_EQ(reread[i].frame, original[i].frame) << "record " << i;
    EXPECT_EQ(reread[i].channel, original[i].channel) << "record " << i;
    EXPECT_EQ(reread[i].frame.timestamp_ns(), original[i].frame.timestamp_ns())
        << "record " << i;
    EXPECT_EQ(reread[i].frame.is_fd(), original[i].frame.is_fd());
    EXPECT_EQ(reread[i].frame.is_brs(), original[i].frame.is_brs());
    EXPECT_EQ(reread[i].frame.is_remote(), original[i].frame.is_remote());
    EXPECT_EQ(reread[i].frame.is_error_frame(), original[i].frame.is_error_frame());
  }
  std::remove(out.c_str());
}

TEST(Asc, ReadsClassicAndFdLines) {
  auto reader = open_reader(data_path("sample.asc"));
  ASSERT_TRUE(reader.has_value());
  const auto records = reader.value()->read_all().value();
  ASSERT_EQ(records.size(), 5u) << "the statistics line must be skipped";

  EXPECT_EQ(records[0].frame.id().value(), 0x123u);
  EXPECT_EQ(records[0].frame.size(), 8u);
  EXPECT_EQ(records[0].frame.data()[0], 0xDE);
  EXPECT_TRUE(records[0].is_rx);
  EXPECT_EQ(records[0].frame.timestamp_ns(), 10000000ULL);

  EXPECT_TRUE(records[1].frame.id().is_extended()) << "the trailing x means extended";
  EXPECT_EQ(records[1].frame.id().value(), 0x18FEF100u);

  EXPECT_FALSE(records[2].is_rx) << "direction Tx must be preserved";

  EXPECT_TRUE(records[3].frame.is_remote());
  EXPECT_EQ(records[3].frame.dlc(), 4u);

  EXPECT_TRUE(records[4].frame.is_fd());
  EXPECT_TRUE(records[4].frame.is_brs());
  EXPECT_FALSE(records[4].frame.is_esi());
  EXPECT_EQ(records[4].frame.id().value(), 0x1ABu);
  EXPECT_EQ(records[4].frame.size(), 12u);
  EXPECT_EQ(records[4].frame.data()[11], 0x0B);
}

TEST(Asc, RoundTripsThroughTheWriter) {
  auto reader = open_reader(data_path("sample.asc"));
  const auto original = reader.value()->read_all().value();

  const std::string out = temp_path("roundtrip.asc");
  {
    auto writer = open_writer(out, LogFormat::Asc);
    ASSERT_TRUE(writer.has_value());
    for (const LogRecord& r : original) {
      ASSERT_TRUE(writer.value()->write(r).has_value());
    }
    ASSERT_TRUE(writer.value()->finish().has_value());
  }
  auto again = open_reader(out, LogFormat::Asc);
  ASSERT_TRUE(again.has_value());
  const auto reread = again.value()->read_all().value();

  ASSERT_EQ(reread.size(), original.size());
  for (std::size_t i = 0; i < original.size(); ++i) {
    EXPECT_EQ(reread[i].frame, original[i].frame) << "record " << i;
    EXPECT_EQ(reread[i].is_rx, original[i].is_rx) << "record " << i;
    EXPECT_EQ(reread[i].frame.is_fd(), original[i].frame.is_fd());
  }
  std::remove(out.c_str());
}

TEST(Blf, ReadsACompressedContainer) {
  auto reader = open_reader(data_path("sample.blf"));
  ASSERT_TRUE(reader.has_value()) << reader.error().message();
  const auto records = reader.value()->read_all();
  ASSERT_TRUE(records.has_value()) << records.error().message();
  ASSERT_EQ(records.value().size(), 5u);

  const auto& r = records.value();
  EXPECT_EQ(r[0].frame.id().value(), 0x123u);
  EXPECT_EQ(r[0].frame.size(), 4u);
  EXPECT_EQ(r[0].frame.data()[0], 0xDE);
  EXPECT_EQ(r[0].frame.timestamp_ns(), 1000000ULL);
  EXPECT_EQ(r[0].channel, "can0");

  EXPECT_TRUE(r[1].frame.id().is_extended());
  EXPECT_EQ(r[1].frame.id().value(), 0x18FEF100u);

  EXPECT_EQ(r[2].channel, "can1");
  EXPECT_FALSE(r[2].is_rx) << "the transmit direction flag must be honoured";

  EXPECT_TRUE(r[3].frame.is_fd());
  EXPECT_TRUE(r[3].frame.is_brs());
  EXPECT_EQ(r[3].frame.size(), 16u);

  EXPECT_TRUE(r[4].frame.is_fd());
  EXPECT_TRUE(r[4].frame.is_esi());
  EXPECT_TRUE(r[4].frame.id().is_extended());
  EXPECT_EQ(r[4].frame.size(), 64u);
  EXPECT_EQ(r[4].frame.data()[63], 63u);
}

TEST(Blf, HandlesBothPublishedContainerLayouts) {
  // Vector's SDK puts the compressed payload at offset 32 from the object
  // start; python-can reads it at 28. Both fixtures must read identically.
  auto sdk = open_reader(data_path("sample.blf")).value()->read_all().value();
  auto other =
      open_reader(data_path("sample_pythoncan.blf")).value()->read_all().value();
  ASSERT_EQ(sdk.size(), other.size());
  for (std::size_t i = 0; i < sdk.size(); ++i) {
    EXPECT_EQ(sdk[i].frame, other[i].frame) << "record " << i;
  }
}

TEST(Blf, HandlesAnUncompressedContainer) {
  auto records = open_reader(data_path("sample_uncompressed.blf")).value()->read_all();
  ASSERT_TRUE(records.has_value()) << records.error().message();
  EXPECT_EQ(records.value().size(), 5u);
}

TEST(Blf, CrossesContainerBoundaries) {
  auto records = open_reader(data_path("two_containers.blf")).value()->read_all();
  ASSERT_TRUE(records.has_value()) << records.error().message();
  ASSERT_EQ(records.value().size(), 2u);
  EXPECT_EQ(records.value()[0].frame.id().value(), 0x100u);
  EXPECT_EQ(records.value()[1].frame.id().value(), 0x200u);
}

TEST(Blf, RejectsACorruptFile) {
  auto reader = open_reader(data_path("corrupt.blf"));
  if (!reader) {
    SUCCEED() << "rejected while opening";
    return;
  }
  const auto records = reader.value()->read_all();
  EXPECT_FALSE(records.has_value())
      << "a container whose checksum fails must not be reported as valid data";
}

TEST(Blf, WritingIsRefusedRatherThanFaked) {
  const auto writer = open_writer(temp_path("nope.blf"), LogFormat::Blf);
  ASSERT_FALSE(writer.has_value());
  EXPECT_EQ(writer.error().code(), core::ErrorCode::TransportUnsupported);
}

TEST(Blf, DetectsTheFormatFromTheSignature) {
  EXPECT_EQ(detect_format(data_path("sample.blf")).value(), LogFormat::Blf);
  EXPECT_EQ(detect_format(data_path("sample.asc")).value(), LogFormat::Asc);
  EXPECT_EQ(detect_format(data_path("sample.log")).value(), LogFormat::Candump);
  EXPECT_FALSE(detect_format(data_path("does_not_exist")).has_value());
}

std::vector<LogRecord> synthetic_records() {
  std::vector<LogRecord> out;
  for (int i = 0; i < 5; ++i) {
    LogRecord r;
    const auto id = CanId::standard(0x100u + static_cast<std::uint32_t>(i)).value();
    const std::uint64_t when =
        std::uint64_t{1000000000} + static_cast<std::uint64_t>(i) * 10000000u;
    r.frame = FdFrame::make(id, nullptr, 2, FrameFlags::None, when).value();
    out.push_back(r);
  }
  return out;
}

TEST(Replay, PreservesTheOriginalInterFrameTiming) {
  auto bus = LogReplayBus::from_records(synthetic_records());
  ASSERT_TRUE(bus->open().has_value());

  // Frames are 10 ms apart. A 5 ms budget gets the first one only.
  const auto first = bus->receive(ms(5));
  ASSERT_TRUE(first.has_value());
  EXPECT_EQ(first.value().id().value(), 0x100u);
  EXPECT_FALSE(bus->receive(ms(4)).has_value()) << "the next frame is not due yet";

  const auto second = bus->receive(ms(10));
  ASSERT_TRUE(second.has_value());
  EXPECT_EQ(second.value().id().value(), 0x101u);
  EXPECT_EQ(second.value().timestamp_ns(), 10000000ULL)
      << "timestamps are rebased to the start of the recording";
}

TEST(Replay, SpeedMultiplierScalesTheTimeline) {
  ReplayOptions fast;
  fast.speed = 10.0;
  auto bus = LogReplayBus::from_records(synthetic_records(), fast);
  ASSERT_TRUE(bus->open().has_value());
  // At 10x the 40 ms recording fits inside 4 ms.
  int count = 0;
  while (bus->receive(ms(5)).has_value()) {
    ++count;
  }
  EXPECT_EQ(count, 5);

  ReplayOptions slow;
  slow.speed = 0.5;
  auto slow_bus = LogReplayBus::from_records(synthetic_records(), slow);
  ASSERT_TRUE(slow_bus->open().has_value());
  ASSERT_TRUE(slow_bus->receive(ms(1)).has_value());
  EXPECT_FALSE(slow_bus->receive(ms(15)).has_value())
      << "at half speed the second frame is due after 20 ms";
  EXPECT_TRUE(slow_bus->receive(ms(10)).has_value());
}

TEST(Replay, ReportsEndOfLog) {
  auto bus = LogReplayBus::from_records(synthetic_records());
  ASSERT_TRUE(bus->open().has_value());
  for (int i = 0; i < 5; ++i) {
    ASSERT_TRUE(bus->receive(ms(100)).has_value()) << i;
  }
  const auto done = bus->receive(ms(100));
  ASSERT_FALSE(done.has_value());
  EXPECT_EQ(done.error().code(), core::ErrorCode::LogEndOfFile);
  EXPECT_TRUE(bus->at_end());
}

TEST(Replay, Loops) {
  ReplayOptions opt;
  opt.loop = true;
  opt.speed = 1000.0;
  auto bus = LogReplayBus::from_records(synthetic_records(), opt);
  ASSERT_TRUE(bus->open().has_value());
  int count = 0;
  while (count < 12 && bus->receive(ms(100)).has_value()) {
    ++count;
  }
  EXPECT_EQ(count, 12) << "a looping replay never reaches the end";
}

TEST(Replay, RewindAndSeek) {
  ReplayOptions opt;
  opt.start_offset = ms(20);
  auto bus = LogReplayBus::from_records(synthetic_records(), opt);
  ASSERT_TRUE(bus->open().has_value());
  const auto got = bus->receive(ms(1));
  ASSERT_TRUE(got.has_value());
  EXPECT_EQ(got.value().id().value(), 0x102u) << "the first two frames were skipped";

  bus->rewind();
  const auto again = bus->receive(ms(1));
  ASSERT_TRUE(again.has_value());
  EXPECT_EQ(again.value().id().value(), 0x102u);
}

TEST(Replay, FiltersApply) {
  auto bus = LogReplayBus::from_records(synthetic_records());
  ASSERT_TRUE(bus->open().has_value());
  ASSERT_TRUE(
      bus->set_filters({Filter::exact(CanId::standard(0x103).value())}).has_value());
  const auto got = bus->receive(ms(100));
  ASSERT_TRUE(got.has_value());
  EXPECT_EQ(got.value().id().value(), 0x103u);
  EXPECT_FALSE(bus->receive(ms(100)).has_value());
}

TEST(Replay, IsReadOnly) {
  auto bus = LogReplayBus::from_records(synthetic_records());
  ASSERT_TRUE(bus->open().has_value());
  const auto sent =
      bus->send(FdFrame::make(CanId::standard(1).value(), nullptr, 1).value());
  ASSERT_FALSE(sent.has_value());
  EXPECT_EQ(sent.error().code(), core::ErrorCode::TransportUnsupported);
}

TEST(Replay, ReplaysARealLogFile) {
  auto bus = LogReplayBus::from_file(data_path("sample.log"));
  ASSERT_TRUE(bus.has_value()) << bus.error().message();
  ASSERT_TRUE(bus.value()->open().has_value());
  EXPECT_EQ(bus.value()->size(), 6u);

  int count = 0;
  while (bus.value()->receive(ms(100)).has_value()) {
    ++count;
  }
  EXPECT_EQ(count, 6);
  EXPECT_EQ(bus.value()->statistics().frames_received, 6u);
}

TEST(Replay, ReplaysABlfFile) {
  auto bus = LogReplayBus::from_file(data_path("sample.blf"));
  ASSERT_TRUE(bus.has_value()) << bus.error().message();
  ASSERT_TRUE(bus.value()->open().has_value());
  EXPECT_EQ(bus.value()->size(), 5u);
  const auto first = bus.value()->receive(ms(100));
  ASSERT_TRUE(first.has_value());
  EXPECT_EQ(first.value().id().value(), 0x123u);
}

}  // namespace
}  // namespace canforge::transport
