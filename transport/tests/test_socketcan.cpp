// SPDX-License-Identifier: MIT
//
// SocketCAN tests. Everything that needs a real interface is skipped unless
// vcan0 exists, so the suite still runs on a machine with no kernel module and
// no root:
//
//     sudo modprobe vcan
//     sudo ip link add dev vcan0 type vcan
//     sudo ip link set up vcan0

#include "canforge/transport/SocketCanBus.hpp"

#include <gtest/gtest.h>

#include <chrono>

namespace canforge::transport {
namespace {

using core::CanId;
using core::FdFrame;
using core::FrameFlags;
using ms = std::chrono::milliseconds;

constexpr const char* kInterface = "vcan0";

bool have_vcan() {
  return SocketCanBus::interface_exists(kInterface);
}

TEST(SocketCan, OpeningAMissingInterfaceFails) {
  SocketCanBus bus("definitely_not_a_can_interface");
  const auto st = bus.open();
  ASSERT_FALSE(st.has_value());
#ifdef __linux__
  EXPECT_EQ(st.error().code(), core::ErrorCode::TransportOpenFailed);
#else
  EXPECT_EQ(st.error().code(), core::ErrorCode::TransportUnsupported);
#endif
  EXPECT_FALSE(bus.is_open());
}

TEST(SocketCan, SendingOnAClosedBusFails) {
  SocketCanBus bus(kInterface);
  const auto st =
      bus.send(FdFrame::make(CanId::standard(0x123).value(), nullptr, 1).value());
  EXPECT_FALSE(st.has_value());
}

#ifdef __linux__

TEST(SocketCan, LoopbackRoundTrip) {
  if (!have_vcan()) {
    GTEST_SKIP() << "vcan0 is not up; see the comment at the top of this file";
  }
  SocketCanOptions options;
  options.receive_own_messages = true;  // a single-process test needs the echo
  SocketCanBus bus(kInterface, options);
  ASSERT_TRUE(bus.open().has_value());
  ASSERT_TRUE(bus.is_open());

  const auto sent =
      FdFrame::make(CanId::standard(0x123).value(), {0xDE, 0xAD, 0xBE, 0xEF}).value();
  ASSERT_TRUE(bus.send(sent).has_value());

  const auto got = bus.receive(ms(500));
  ASSERT_TRUE(got.has_value()) << got.error().message();
  EXPECT_EQ(got.value().id().value(), 0x123u);
  EXPECT_EQ(got.value().size(), 4u);
  EXPECT_EQ(got.value().data()[0], 0xDE);
  EXPECT_GT(got.value().timestamp_ns(), 0u) << "a timestamp must be attached";
  EXPECT_EQ(bus.statistics().frames_sent, 1u);
  EXPECT_EQ(bus.statistics().frames_received, 1u);
}

TEST(SocketCan, ExtendedIdentifiers) {
  if (!have_vcan()) {
    GTEST_SKIP() << "vcan0 is not up";
  }
  SocketCanOptions options;
  options.receive_own_messages = true;
  SocketCanBus bus(kInterface, options);
  ASSERT_TRUE(bus.open().has_value());
  const auto sent =
      FdFrame::make(CanId::extended(0x18FEF100).value(), {1, 2, 3}).value();
  ASSERT_TRUE(bus.send(sent).has_value());
  const auto got = bus.receive(ms(500));
  ASSERT_TRUE(got.has_value());
  EXPECT_TRUE(got.value().id().is_extended());
  EXPECT_EQ(got.value().id().value(), 0x18FEF100u);
}

TEST(SocketCan, CanFdWhenTheInterfaceSupportsIt) {
  if (!have_vcan()) {
    GTEST_SKIP() << "vcan0 is not up";
  }
  SocketCanOptions options;
  options.receive_own_messages = true;
  SocketCanBus bus(kInterface, options);
  ASSERT_TRUE(bus.open().has_value());
  if (!bus.fd_enabled()) {
    GTEST_SKIP() << "the interface did not accept CAN_RAW_FD_FRAMES";
  }
  const auto sent = FdFrame::make(CanId::standard(0x456).value(), nullptr, 32,
                                  FrameFlags::Fd | FrameFlags::Brs)
                        .value();
  ASSERT_TRUE(bus.send(sent).has_value());
  const auto got = bus.receive(ms(500));
  ASSERT_TRUE(got.has_value());
  EXPECT_TRUE(got.value().is_fd());
  EXPECT_EQ(got.value().size(), 32u);
}

TEST(SocketCan, FiltersAreInstalledInTheKernel) {
  if (!have_vcan()) {
    GTEST_SKIP() << "vcan0 is not up";
  }
  SocketCanOptions options;
  options.receive_own_messages = true;
  SocketCanBus bus(kInterface, options);
  ASSERT_TRUE(bus.open().has_value());
  ASSERT_TRUE(
      bus.set_filters({Filter::exact(CanId::standard(0x200).value())}).has_value());

  ASSERT_TRUE(
      bus.send(FdFrame::make(CanId::standard(0x100).value(), {1}).value()).has_value());
  ASSERT_TRUE(
      bus.send(FdFrame::make(CanId::standard(0x200).value(), {2}).value()).has_value());

  const auto got = bus.receive(ms(500));
  ASSERT_TRUE(got.has_value());
  EXPECT_EQ(got.value().id().value(), 0x200u)
      << "the kernel should have dropped 0x100 before it reached us";
}

TEST(SocketCan, ReceiveTimesOut) {
  if (!have_vcan()) {
    GTEST_SKIP() << "vcan0 is not up";
  }
  SocketCanBus bus(kInterface);
  ASSERT_TRUE(bus.open().has_value());
  ASSERT_TRUE(
      bus.set_filters({Filter::exact(CanId::standard(0x7AB).value())}).has_value());
  const auto got = bus.receive(ms(50));
  ASSERT_FALSE(got.has_value());
  EXPECT_EQ(got.error().code(), core::ErrorCode::TransportTimeout);
}

TEST(SocketCan, TwoSocketsSeeEachOther) {
  if (!have_vcan()) {
    GTEST_SKIP() << "vcan0 is not up";
  }
  SocketCanBus sender(kInterface);
  SocketCanBus receiver(kInterface);
  ASSERT_TRUE(sender.open().has_value());
  ASSERT_TRUE(receiver.open().has_value());

  ASSERT_TRUE(
      sender.send(FdFrame::make(CanId::standard(0x321).value(), {0xAA, 0xBB}).value())
          .has_value());
  const auto got = receiver.receive(ms(500));
  ASSERT_TRUE(got.has_value());
  EXPECT_EQ(got.value().id().value(), 0x321u);
  EXPECT_EQ(got.value().size(), 2u);
}

#endif  // __linux__

}  // namespace
}  // namespace canforge::transport
