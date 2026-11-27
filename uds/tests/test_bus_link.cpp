// SPDX-License-Identifier: MIT
//
// BusLink: the UDS client on a bus. The bus is a VirtualBus and the ECU is the
// simulated UDS server, both on the medium's virtual clock, so a five-second
// P2* costs microseconds and nothing here depends on the machine's speed.

#include "canforge/uds/BusLink.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <string>
#include <vector>

#include "canforge/sim/UdsServer.hpp"
#include "canforge/transport/VirtualBus.hpp"

namespace canforge::uds {
namespace {

using std::chrono::milliseconds;
using std::chrono::nanoseconds;

constexpr std::uint64_t kMs = 1000000ULL;

core::CanId request_id() {
  return core::CanId::standard(0x7E0).value();
}
core::CanId response_id() {
  return core::CanId::standard(0x7E8).value();
}

ClientConfig client_config() {
  ClientConfig c;
  c.transport.address = isotp::Address::normal(request_id(), response_id());
  c.tester_present_keepalive = false;  // switched on where it is tested
  return c;
}

sim::UdsServerConfig server_config() {
  isotp::Config transport;
  transport.address = isotp::Address::normal(response_id(), request_id());
  return sim::default_server_config(transport);
}

/// A tester and an ECU on one virtual bus. The link's hook runs the ECU between
/// two polls of the client, the way a second thread would on a real bus.
struct Bench {
  std::shared_ptr<transport::VirtualMedium> medium = transport::VirtualMedium::create();
  std::unique_ptr<transport::VirtualBus> tester_bus = medium->attach("tester");
  std::unique_ptr<transport::VirtualBus> ecu_bus = medium->attach("ecu");
  Client client;
  sim::UdsServer server;
  BusLink link;
  bool ecu_powered = true;

  explicit Bench(ClientConfig cc = client_config(),
                 sim::UdsServerConfig sc = server_config())
      : client(cc), server(std::move(sc)), link(*tester_bus, client) {
    EXPECT_TRUE(tester_bus->open().has_value());
    EXPECT_TRUE(ecu_bus->open().has_value());
    link.set_clock([this]() { return medium->now_ns(); });
    link.set_hook([this](std::uint64_t now) { run_ecu(now); });
  }

  void run_ecu(std::uint64_t now) {
    for (;;) {
      const auto frame = ecu_bus->receive(nanoseconds(0));
      if (!frame) {
        break;
      }
      if (ecu_powered && frame.value().id() == request_id()) {
        server.on_frame(frame.value(), now);
      }
    }
    if (ecu_powered) {
      for (const core::FdFrame& f : server.poll(now)) {
        EXPECT_TRUE(ecu_bus->send(f).has_value());
      }
    }
  }

  Response positive(milliseconds timeout = milliseconds(10000)) {
    const auto result = link.run(timeout);
    EXPECT_TRUE(result.has_value())
        << (result.has_value() ? "" : std::string(result.error().message()));
    if (!result) {
      return {};
    }
    EXPECT_TRUE(result.value().positive) << describe_nrc(result.value().nrc);
    return result.value();
  }

  void unlock() {
    ASSERT_TRUE(
        client.diagnostic_session_control(SessionType::Programming, link.now_ns())
            .has_value());
    (void)positive();
    ASSERT_TRUE(client.security_access_request_seed(0x01, link.now_ns()).has_value());
    const Response seed = positive();
    ASSERT_GE(seed.data.size(), 5u);
    std::uint32_t s = 0;
    for (std::size_t i = 1; i <= 4; ++i) {
      s = (s << 8u) | seed.data[i];
    }
    const std::uint32_t key = sim::toy_key_from_seed(s, 0xA5C3F00Du);
    ASSERT_TRUE(
        client
            .security_access_send_key(0x02,
                                      {static_cast<std::uint8_t>(key >> 24u),
                                       static_cast<std::uint8_t>((key >> 16u) & 0xFFu),
                                       static_cast<std::uint8_t>((key >> 8u) & 0xFFu),
                                       static_cast<std::uint8_t>(key & 0xFFu)},
                                      link.now_ns())
            .has_value());
    (void)positive();
    ASSERT_TRUE(server.unlocked());
  }
};

TEST(BusLink, ReadsASegmentedResponseOverTheBus) {
  Bench bench;
  ASSERT_TRUE(
      bench.client.read_data_by_identifier(0xF190, bench.link.now_ns()).has_value());
  const Response response = bench.positive();
  // The VIN is 17 characters: a first frame, the client's flow control going
  // back through the bus, then consecutive frames.
  ASSERT_EQ(response.data.size(), 2u + 17u);
  EXPECT_EQ(response.data[0], 0xF1);
  EXPECT_EQ(response.data[1], 0x90);
  EXPECT_EQ(bench.server.requests_handled(), 1u);
}

TEST(BusLink, ANegativeResponseIsAResponseNotAnError) {
  Bench bench;
  ASSERT_TRUE(
      bench.client.read_data_by_identifier(0x1234, bench.link.now_ns()).has_value());
  const auto result = bench.link.run(milliseconds(2000));
  ASSERT_TRUE(result.has_value());
  EXPECT_FALSE(result.value().positive);
  EXPECT_EQ(result.value().nrc, static_cast<std::uint8_t>(Nrc::RequestOutOfRange));
}

TEST(BusLink, SeveralRequestsInARow) {
  Bench bench;
  for (int i = 0; i < 20; ++i) {
    ASSERT_TRUE(bench.client
                    .read_dtc_information(DtcReportType::ReportDtcByStatusMask, 0xFF,
                                          bench.link.now_ns())
                    .has_value());
    const Response response = bench.positive();
    EXPECT_FALSE(Client::parse_dtc_list(response.data).empty());
  }
  EXPECT_EQ(bench.server.requests_handled(), 20u);
}

TEST(BusLink, WaitsThroughResponsePending) {
  sim::UdsServerConfig sc = server_config();
  sc.erase_duration_ns = 3000 * kMs;  // longer than P2, so the server says 0x78
  Bench bench(client_config(), sc);
  bench.unlock();

  const std::uint64_t before = bench.link.now_ns();
  ASSERT_TRUE(bench.client
                  .routine_control(RoutineControlType::StartRoutine, 0xFF00, {},
                                   bench.link.now_ns())
                  .has_value());
  const Response response = bench.positive();
  EXPECT_GE(response.pending_count, 1u);
  EXPECT_GE(bench.link.now_ns() - before, 3000 * kMs)
      << "on the bus's clock, not the wall's";
}

TEST(BusLink, ASilentEcuEndsInTheClientsTimeout) {
  Bench bench;
  bench.ecu_powered = false;
  const std::uint64_t before = bench.link.now_ns();
  ASSERT_TRUE(
      bench.client.read_data_by_identifier(0xF190, bench.link.now_ns()).has_value());
  const auto result = bench.link.run(milliseconds(10000));
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code(), core::ErrorCode::TransportTimeout);
  // P2 is 50 ms: the link came back after that, not after its own ten seconds.
  EXPECT_LT(bench.link.now_ns() - before, 200 * kMs);
}

TEST(BusLink, TheOuterTimeoutEndsAnExchangeThatNeverSettles) {
  ClientConfig cc = client_config();
  cc.max_response_pending = 0;  // unlimited: only the link's timeout can end this
  Bench bench(cc);
  bench.ecu_powered = false;
  // An ECU that answers "still working" every 20 ms (inside P2), for ever.
  std::uint64_t last = 0;
  bench.link.set_hook([&](std::uint64_t now) {
    bench.run_ecu(now);
    if (now - last >= 20 * kMs) {
      last = now;
      const std::uint8_t pending[8] = {0x03, 0x7F, 0x22, 0x78, 0xCC, 0xCC, 0xCC, 0xCC};
      EXPECT_TRUE(
          bench.ecu_bus->send(core::FdFrame::make(response_id(), pending, 8).value())
              .has_value());
    }
  });
  ASSERT_TRUE(
      bench.client.read_data_by_identifier(0xF190, bench.link.now_ns()).has_value());
  const auto result = bench.link.run(milliseconds(3000));
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code(), core::ErrorCode::TransportTimeout);
  EXPECT_TRUE(bench.client.busy()) << "the client would still be waiting";
}

TEST(BusLink, OtherTrafficOnTheBusIsIgnored) {
  Bench bench;
  auto noise = bench.medium->attach("noise");
  ASSERT_TRUE(noise->open().has_value());
  int sent = 0;
  bench.link.set_hook([&](std::uint64_t now) {
    // Cyclic traffic, and a frame that looks like ISO-TP on a neighbouring ID.
    const std::uint8_t data[8] = {0x10, 0x14, 1, 2, 3, 4, 5, 6};
    (void)noise->send(
        core::FdFrame::make(core::CanId::standard(0x300).value(), data, 8).value());
    (void)noise->send(
        core::FdFrame::make(core::CanId::standard(0x7E9).value(), data, 8).value());
    sent += 2;
    bench.run_ecu(now);
  });
  ASSERT_TRUE(
      bench.client.read_data_by_identifier(0xF190, bench.link.now_ns()).has_value());
  const Response response = bench.positive();
  EXPECT_EQ(response.data.size(), 2u + 17u);
  EXPECT_GT(sent, 4);
}

TEST(BusLink, InstallFilterAsksForTheResponseIdentifierOnly) {
  Bench bench;
  ASSERT_TRUE(bench.link.install_filter().has_value());
  ASSERT_EQ(bench.tester_bus->filters().size(), 1u);
  EXPECT_TRUE(transport::accepted_by(bench.tester_bus->filters(), response_id()));
  EXPECT_FALSE(transport::accepted_by(bench.tester_bus->filters(), request_id()));
  EXPECT_FALSE(transport::accepted_by(bench.tester_bus->filters(),
                                      core::CanId::standard(0x300).value()));

  ASSERT_TRUE(
      bench.client.read_data_by_identifier(0xF190, bench.link.now_ns()).has_value());
  EXPECT_EQ(bench.positive().data.size(), 2u + 17u) << "and the exchange still works";
}

TEST(BusLink, IdleSendsTheKeepaliveOfANonDefaultSession) {
  ClientConfig cc = client_config();
  cc.tester_present_keepalive = true;
  Bench bench(cc);
  ASSERT_TRUE(bench.client
                  .diagnostic_session_control(SessionType::ExtendedDiagnostic,
                                              bench.link.now_ns())
                  .has_value());
  (void)bench.positive();
  ASSERT_EQ(bench.server.tester_present_seen(), 0u);

  ASSERT_TRUE(bench.link.idle(milliseconds(7000)).has_value());
  // S3 is 5 s and the client sends at 40 % of it: 2 s, 4 s, 6 s.
  EXPECT_EQ(bench.server.tester_present_seen(), 3u);
  EXPECT_EQ(bench.server.session(), SessionType::ExtendedDiagnostic);
}

TEST(BusLink, RunWithoutARequestIsAnError) {
  Bench bench;
  const auto result = bench.link.run(milliseconds(100));
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code(), core::ErrorCode::InvalidArgument);
}

TEST(BusLink, AClosedBusIsReported) {
  Bench bench;
  bench.tester_bus->close();
  ASSERT_TRUE(
      bench.client.read_data_by_identifier(0xF190, bench.link.now_ns()).has_value());
  const auto result = bench.link.run(milliseconds(100));
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code(), core::ErrorCode::TransportNotOpen);
}

TEST(BusLink, TheDefaultClockIsTheWallClock) {
  auto medium = transport::VirtualMedium::create();
  auto bus = medium->attach("tester");
  Client client(client_config());
  const BusLink link(*bus, client);
  const std::uint64_t a = link.now_ns();
  const std::uint64_t b = link.now_ns();
  EXPECT_GE(b, a);
  EXPECT_LT(a, 1000 * kMs) << "zero at construction";
}

}  // namespace
}  // namespace canforge::uds
