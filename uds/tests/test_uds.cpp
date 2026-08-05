// SPDX-License-Identifier: MIT
//
// UDS client tests, driven against the simulated server ECU. Like the ISO-TP
// tests these run on a virtual clock, so a three-second simulated flash erase
// costs microseconds of real time.

#include "canforge/uds/Uds.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <numeric>
#include <string>
#include <vector>

#include "canforge/sim/UdsServer.hpp"

namespace canforge::uds {
namespace {

constexpr std::uint64_t kMs = 1000000ULL;

core::CanId tester_tx() { return core::CanId::standard(0x7E0).value(); }
core::CanId ecu_tx() { return core::CanId::standard(0x7E8).value(); }

isotp::Config client_transport() {
  isotp::Config c;
  c.address = isotp::Address::normal(tester_tx(), ecu_tx());
  return c;
}
isotp::Config server_transport() {
  isotp::Config c;
  c.address = isotp::Address::normal(ecu_tx(), tester_tx());
  return c;
}

ClientConfig client_config() {
  ClientConfig c;
  c.transport = client_transport();
  c.p2_ns = 50 * kMs;
  c.p2_star_ns = 5000 * kMs;
  c.tester_present_keepalive = false;  // enabled explicitly where it is tested
  return c;
}

/// Client and simulated server wired directly together on a virtual clock.
struct Bench {
  Client client;
  sim::UdsServer server;
  std::uint64_t now = 0;

  Bench() : client(client_config()),
            server(sim::default_server_config(server_transport())) {}

  explicit Bench(sim::UdsServerConfig server_config)
      : client(client_config()), server(std::move(server_config)) {}

  /// Pump both sides until the client's request settles.
  bool run(std::uint64_t step_ns = 200000ULL, int max_rounds = 400000) {
    for (int i = 0; i < max_rounds; ++i) {
      for (const core::FdFrame& f : client.poll(now)) {
        server.on_frame(f, now);
      }
      for (const core::FdFrame& f : server.poll(now)) {
        client.on_frame(f, now);
      }
      if (!client.busy()) {
        return true;
      }
      now += step_ns;
    }
    return false;
  }

  Response expect_positive() {
    EXPECT_TRUE(run());
    EXPECT_EQ(client.state(), RequestState::Complete);
    EXPECT_TRUE(client.response().positive)
        << "negative response: " << describe_nrc(client.response().nrc);
    return client.response();
  }

  Response expect_negative(Nrc nrc) {
    EXPECT_TRUE(run());
    EXPECT_EQ(client.state(), RequestState::Complete);
    EXPECT_FALSE(client.response().positive);
    EXPECT_EQ(client.response().nrc, static_cast<std::uint8_t>(nrc))
        << "got " << describe_nrc(client.response().nrc);
    return client.response();
  }

  /// Unlock the ECU, which every programming operation needs first.
  void unlock(std::uint32_t secret = 0xA5C3F00Du) {
    ASSERT_TRUE(client.diagnostic_session_control(SessionType::Programming, now)
                    .has_value());
    expect_positive();
    ASSERT_TRUE(client.security_access_request_seed(0x01, now).has_value());
    const Response seed_response = expect_positive();
    ASSERT_GE(seed_response.data.size(), 5u);
    std::uint32_t seed = 0;
    for (std::size_t i = 1; i <= 4; ++i) {
      seed = (seed << 8u) | seed_response.data[i];
    }
    const std::uint32_t key = sim::toy_key_from_seed(seed, secret);
    const std::vector<std::uint8_t> key_bytes = {
        static_cast<std::uint8_t>((key >> 24u) & 0xFFu),
        static_cast<std::uint8_t>((key >> 16u) & 0xFFu),
        static_cast<std::uint8_t>((key >> 8u) & 0xFFu),
        static_cast<std::uint8_t>(key & 0xFFu)};
    ASSERT_TRUE(client.security_access_send_key(0x02, key_bytes, now).has_value());
    expect_positive();
    ASSERT_TRUE(server.unlocked());
  }
};

TEST(Nrc, EveryDefinedCodeHasAName) {
  const Nrc codes[] = {
      Nrc::GeneralReject, Nrc::ServiceNotSupported, Nrc::SubFunctionNotSupported,
      Nrc::IncorrectMessageLengthOrInvalidFormat, Nrc::ResponseTooLong,
      Nrc::BusyRepeatRequest, Nrc::ConditionsNotCorrect, Nrc::RequestSequenceError,
      Nrc::RequestOutOfRange, Nrc::SecurityAccessDenied, Nrc::InvalidKey,
      Nrc::ExceedNumberOfAttempts, Nrc::RequiredTimeDelayNotExpired,
      Nrc::UploadDownloadNotAccepted, Nrc::TransferDataSuspended,
      Nrc::GeneralProgrammingFailure, Nrc::WrongBlockSequenceCounter,
      Nrc::RequestCorrectlyReceivedResponsePending,
      Nrc::SubFunctionNotSupportedInActiveSession,
      Nrc::ServiceNotSupportedInActiveSession, Nrc::VoltageTooHigh,
      Nrc::VoltageTooLow, Nrc::ShifterLeverNotInPark};
  for (const Nrc code : codes) {
    EXPECT_STRNE(to_string(code), "unknown")
        << "0x" << std::hex << int{static_cast<std::uint8_t>(code)};
  }
}

TEST(Nrc, UnknownCodesReportTheirReservedRange) {
  EXPECT_NE(describe_nrc(0x40).find("ISO 15764"), std::string::npos);
  EXPECT_NE(describe_nrc(0xA0).find("condition-driven"), std::string::npos);
  EXPECT_NE(describe_nrc(0xF5).find("manufacturer"), std::string::npos);
  EXPECT_NE(describe_nrc(0x33).find("security access denied"), std::string::npos);
}

TEST(Dtc, Formatting) {
  // 0x010301: the top two bits of 0x01 are 00, giving the letter P, so this is
  // P0103 with a failure type of 01.
  EXPECT_EQ(format_dtc(0x010301), "P0103-01");
  EXPECT_EQ(format_dtc(0xC00A00), "U000A-00");
  EXPECT_EQ(describe_dtc_status(0x00), "none");
  EXPECT_NE(describe_dtc_status(0x08).find("confirmedDTC"), std::string::npos);
  EXPECT_NE(describe_dtc_status(0x09).find("testFailed"), std::string::npos);
}

TEST(Service, Names) {
  EXPECT_STREQ(to_string(Service::ReadDataByIdentifier), "ReadDataByIdentifier");
  EXPECT_STREQ(to_string(Service::RequestDownload), "RequestDownload");
  EXPECT_STREQ(to_string(SessionType::Programming), "programming");
}

TEST(Client, DiagnosticSessionControl) {
  Bench bench;
  ASSERT_TRUE(bench.client
                  .diagnostic_session_control(SessionType::ExtendedDiagnostic,
                                              bench.now)
                  .has_value());
  const Response r = bench.expect_positive();
  ASSERT_GE(r.data.size(), 1u);
  EXPECT_EQ(r.data[0], static_cast<std::uint8_t>(SessionType::ExtendedDiagnostic));
  EXPECT_EQ(bench.server.session(), SessionType::ExtendedDiagnostic);
  EXPECT_EQ(bench.client.session(), SessionType::ExtendedDiagnostic);
}

TEST(Client, UnsupportedSubFunctionIsRejected) {
  Bench bench;
  ASSERT_TRUE(bench.client.request({0x10, 0x77}, bench.now).has_value());
  bench.expect_negative(Nrc::SubFunctionNotSupported);
}

TEST(Client, UnsupportedServiceIsRejected) {
  Bench bench;
  ASSERT_TRUE(bench.client.request({0x99, 0x00}, bench.now).has_value());
  bench.expect_negative(Nrc::ServiceNotSupported);
}

TEST(Client, ReadDataByIdentifier) {
  Bench bench;
  ASSERT_TRUE(bench.client.read_data_by_identifier(0xF190, bench.now).has_value());
  const Response r = bench.expect_positive();
  ASSERT_GT(r.data.size(), 2u);
  EXPECT_EQ(r.data[0], 0xF1);
  EXPECT_EQ(r.data[1], 0x90);
  const std::string vin(r.data.begin() + 2, r.data.end());
  EXPECT_EQ(vin, "CANFORGE0SIM00001");
  // Seventeen characters plus the identifier does not fit in one CAN frame, so
  // this exercised a real segmented ISO-TP transfer end to end.
  EXPECT_EQ(vin.size(), 17u);
}

TEST(Client, ReadUnknownIdentifier) {
  Bench bench;
  ASSERT_TRUE(bench.client.read_data_by_identifier(0xDEAD, bench.now).has_value());
  bench.expect_negative(Nrc::RequestOutOfRange);
}

TEST(Client, WriteDataByIdentifier) {
  Bench bench;
  ASSERT_TRUE(bench.client
                  .write_data_by_identifier(0x0100, {0xAB, 0xCD}, bench.now)
                  .has_value());
  bench.expect_positive();

  ASSERT_TRUE(bench.client.read_data_by_identifier(0x0100, bench.now).has_value());
  const Response r = bench.expect_positive();
  ASSERT_EQ(r.data.size(), 4u);
  EXPECT_EQ(r.data[2], 0xAB);
  EXPECT_EQ(r.data[3], 0xCD);
}

TEST(Client, IdentificationDataIsReadOnly) {
  Bench bench;
  ASSERT_TRUE(bench.client.write_data_by_identifier(0xF190, {0x00}, bench.now)
                  .has_value());
  bench.expect_negative(Nrc::SecurityAccessDenied);
}

TEST(Client, ReadDtcInformation) {
  Bench bench;
  ASSERT_TRUE(bench.client
                  .read_dtc_information(DtcReportType::ReportDtcByStatusMask,
                                        0xFF, bench.now)
                  .has_value());
  const Response r = bench.expect_positive();
  const std::vector<Dtc> dtcs = Client::parse_dtc_list(r.data);
  ASSERT_EQ(dtcs.size(), 3u);
  EXPECT_EQ(dtcs[0].code, 0x010301u);
  EXPECT_EQ(format_dtc(dtcs[0].code), "P0103-01");
  EXPECT_NE(describe_dtc_status(dtcs[0].status).find("confirmedDTC"),
            std::string::npos);
  EXPECT_EQ(dtcs[2].code, 0xC00A00u);
}

TEST(Client, ReadDtcByStatusMaskFiltersProperly) {
  Bench bench;
  const auto pending = static_cast<std::uint8_t>(DtcStatusBit::PendingDtc);
  ASSERT_TRUE(bench.client
                  .read_dtc_information(DtcReportType::ReportDtcByStatusMask,
                                        pending, bench.now)
                  .has_value());
  const std::vector<Dtc> dtcs = Client::parse_dtc_list(bench.expect_positive().data);
  ASSERT_EQ(dtcs.size(), 1u);
  EXPECT_EQ(dtcs[0].code, 0x011716u);
}

TEST(Client, ClearDiagnosticInformation) {
  Bench bench;
  EXPECT_EQ(bench.server.dtcs().size(), 3u);
  ASSERT_TRUE(bench.client.clear_diagnostic_information(0xFFFFFF, bench.now)
                  .has_value());
  bench.expect_positive();
  EXPECT_TRUE(bench.server.dtcs().empty());

  ASSERT_TRUE(bench.client
                  .read_dtc_information(DtcReportType::ReportDtcByStatusMask,
                                        0xFF, bench.now)
                  .has_value());
  EXPECT_TRUE(Client::parse_dtc_list(bench.expect_positive().data).empty());
}

TEST(Client, EcuResetDropsTheSession) {
  Bench bench;
  ASSERT_TRUE(bench.client
                  .diagnostic_session_control(SessionType::ExtendedDiagnostic,
                                              bench.now)
                  .has_value());
  bench.expect_positive();
  ASSERT_EQ(bench.server.session(), SessionType::ExtendedDiagnostic);

  ASSERT_TRUE(bench.client.ecu_reset(ResetType::HardReset, bench.now).has_value());
  bench.expect_positive();
  EXPECT_EQ(bench.server.session(), SessionType::Default);
  EXPECT_FALSE(bench.server.unlocked());
}

TEST(Client, TesterPresentWithSuppressedResponseCompletesImmediately) {
  Bench bench;
  ASSERT_TRUE(bench.client.tester_present(bench.now, true).has_value());
  ASSERT_TRUE(bench.run());
  EXPECT_EQ(bench.client.state(), RequestState::Complete);
  EXPECT_TRUE(bench.client.response().positive);
  EXPECT_EQ(bench.server.tester_present_seen(), 1u);
  // The point of the suppress bit: no answer comes back, and the client must
  // not sit through a full P2 waiting for one.
  EXPECT_LT(bench.now, 40 * kMs);
}

TEST(Client, TesterPresentWithoutSuppressionGetsAnAnswer) {
  Bench bench;
  ASSERT_TRUE(bench.client.tester_present(bench.now, false).has_value());
  bench.expect_positive();
  EXPECT_EQ(bench.server.tester_present_seen(), 1u);
}

TEST(Client, KeepaliveSendsTesterPresentInANonDefaultSession) {
  ClientConfig config = client_config();
  config.tester_present_keepalive = true;
  config.s3_ns = 100 * kMs;  // keepalive due every 40 ms
  Client client(config);
  sim::UdsServer server(sim::default_server_config(server_transport()));

  std::uint64_t now = 0;
  ASSERT_TRUE(client.diagnostic_session_control(SessionType::Programming, now)
                  .has_value());
  for (int i = 0; i < 20000; ++i) {
    for (const core::FdFrame& f : client.poll(now)) {
      server.on_frame(f, now);
    }
    for (const core::FdFrame& f : server.poll(now)) {
      client.on_frame(f, now);
    }
    now += 200000ULL;
    if (now > 500 * kMs) {
      break;
    }
  }
  EXPECT_EQ(client.session(), SessionType::Programming);
  EXPECT_GE(client.tester_present_sent(), 5u)
      << "the session must be held open without any request traffic";
  EXPECT_GE(server.tester_present_seen(), 5u);
}

TEST(Client, SecurityAccessIsRefusedInTheDefaultSession) {
  Bench bench;
  ASSERT_TRUE(
      bench.client.security_access_request_seed(0x01, bench.now).has_value());
  bench.expect_negative(Nrc::ServiceNotSupportedInActiveSession);
}

TEST(Client, SecurityAccessSeedAndKey) {
  Bench bench;
  bench.unlock();
  EXPECT_TRUE(bench.server.unlocked());
}

TEST(Client, WrongKeyIsRejected) {
  Bench bench;
  ASSERT_TRUE(bench.client
                  .diagnostic_session_control(SessionType::Programming, bench.now)
                  .has_value());
  bench.expect_positive();
  ASSERT_TRUE(
      bench.client.security_access_request_seed(0x01, bench.now).has_value());
  bench.expect_positive();
  ASSERT_TRUE(bench.client.security_access_send_key(0x02, {0, 0, 0, 0}, bench.now)
                  .has_value());
  bench.expect_negative(Nrc::InvalidKey);
  EXPECT_FALSE(bench.server.unlocked());
}

TEST(Client, SeedAndKeySubFunctionParityIsChecked) {
  Bench bench;
  EXPECT_FALSE(
      bench.client.security_access_request_seed(0x02, bench.now).has_value())
      << "requestSeed must be odd";
  EXPECT_FALSE(
      bench.client.security_access_send_key(0x01, {0}, bench.now).has_value())
      << "sendKey must be even";
}

TEST(Client, RoutineControlNeedsSecurity) {
  Bench bench;
  ASSERT_TRUE(bench.client
                  .routine_control(RoutineControlType::StartRoutine, 0xFF00, {},
                                   bench.now)
                  .has_value());
  bench.expect_negative(Nrc::SecurityAccessDenied);
}

TEST(Client, ResponsePendingExtendsTheTimeout) {
  // A three-second erase: far past P2, comfortably inside P2*. A client that
  // treats NRC 0x78 as a failure cannot flash an ECU at all.
  sim::UdsServerConfig config = sim::default_server_config(server_transport());
  config.erase_duration_ns = 3000 * kMs;
  Bench bench(config);
  bench.unlock();

  const std::uint64_t started = bench.now;
  ASSERT_TRUE(bench.client
                  .routine_control(RoutineControlType::StartRoutine, 0xFF00, {},
                                   bench.now)
                  .has_value());
  const Response r = bench.expect_positive();
  EXPECT_GE(r.pending_count, 1u) << "at least one 0x78 should have been absorbed";
  EXPECT_GT(bench.now - started, 2900 * kMs)
      << "the client waited out the erase instead of timing out at P2";
}

TEST(Client, TimesOutWhenTheServerSaysNothing) {
  Client client(client_config());
  ASSERT_TRUE(client.read_data_by_identifier(0xF190, 0).has_value());
  std::uint64_t now = 0;
  for (int i = 0; i < 10000 && client.busy(); ++i) {
    client.poll(now);
    now += kMs;
  }
  EXPECT_EQ(client.state(), RequestState::Failed);
  EXPECT_EQ(client.failure().code(), core::ErrorCode::TransportTimeout);
  EXPECT_LE(now, 60 * kMs) << "it should give up at P2, not later";
}

TEST(Client, RejectsASecondConcurrentRequest) {
  Bench bench;
  ASSERT_TRUE(bench.client.read_data_by_identifier(0xF190, bench.now).has_value());
  EXPECT_FALSE(bench.client.read_data_by_identifier(0xF189, bench.now).has_value());
}

TEST(Client, CompleteFirmwareDownload) {
  sim::UdsServerConfig config = sim::default_server_config(server_transport());
  config.flash_size = 8 * 1024;
  Bench bench(config);

  std::vector<std::uint8_t> firmware(4096);
  for (std::size_t i = 0; i < firmware.size(); ++i) {
    firmware[i] = static_cast<std::uint8_t>((i * 7u + 3u) & 0xFFu);
  }

  // 1. Programming session and security access.
  bench.unlock();
  ASSERT_EQ(bench.server.session(), SessionType::Programming);

  // 2. Erase.
  ASSERT_TRUE(bench.client
                  .routine_control(RoutineControlType::StartRoutine, 0xFF00, {},
                                   bench.now)
                  .has_value());
  bench.expect_positive();

  // 3. RequestDownload announces where and how much.
  ASSERT_TRUE(bench.client
                  .request_download(0x08000000u,
                                    static_cast<std::uint32_t>(firmware.size()),
                                    4, 4, bench.now)
                  .has_value());
  const Response download = bench.expect_positive();
  ASSERT_GE(download.data.size(), 3u);
  const auto max_block =
      static_cast<std::uint16_t>((download.data[1] << 8u) | download.data[2]);
  EXPECT_EQ(max_block, 512u);

  // 4. TransferData, block by block, with the counter wrapping 0xFF -> 0x01
  //    rather than to zero.
  std::uint8_t block = 1;
  std::size_t offset = 0;
  const std::size_t chunk = 256;
  while (offset < firmware.size()) {
    const std::size_t take = std::min(chunk, firmware.size() - offset);
    const std::vector<std::uint8_t> piece(
        firmware.begin() + static_cast<std::ptrdiff_t>(offset),
        firmware.begin() + static_cast<std::ptrdiff_t>(offset + take));
    ASSERT_TRUE(bench.client.transfer_data(block, piece, bench.now).has_value());
    const Response r = bench.expect_positive();
    ASSERT_GE(r.data.size(), 1u);
    EXPECT_EQ(r.data[0], block);
    offset += take;
    block = static_cast<std::uint8_t>(block + 1u);
    if (block == 0) {
      block = 1;
    }
  }

  // 5. RequestTransferExit.
  ASSERT_TRUE(bench.client.request_transfer_exit(bench.now).has_value());
  bench.expect_positive();
  EXPECT_TRUE(bench.server.download_complete());

  // 6. The bytes really landed in the ECU's flash.
  ASSERT_GE(bench.server.flash().size(), firmware.size());
  for (std::size_t i = 0; i < firmware.size(); ++i) {
    ASSERT_EQ(bench.server.flash()[i], firmware[i]) << "flash byte " << i;
  }

  // 7. Verify through RoutineControl, as a real flow does.
  ASSERT_TRUE(bench.client
                  .routine_control(RoutineControlType::StartRoutine, 0xFF01, {},
                                   bench.now)
                  .has_value());
  const Response verify = bench.expect_positive();
  ASSERT_GE(verify.data.size(), 7u);
  std::uint32_t reported = 0;
  for (std::size_t i = 3; i < 7; ++i) {
    reported = (reported << 8u) | verify.data[i];
  }
  const std::uint32_t expected = std::accumulate(
      firmware.begin(), firmware.end(), std::uint32_t{0},
      [](std::uint32_t acc, std::uint8_t b) { return acc + b; });
  EXPECT_EQ(reported, expected) << "the ECU's checksum must match the image";

  // 8. Back to the default session.
  ASSERT_TRUE(bench.client.ecu_reset(ResetType::HardReset, bench.now).has_value());
  bench.expect_positive();
  EXPECT_EQ(bench.server.session(), SessionType::Default);
}

TEST(Client, DownloadIsRefusedOutsideTheProgrammingSession) {
  Bench bench;
  ASSERT_TRUE(bench.client.request_download(0x08000000u, 16, 4, 4, bench.now)
                  .has_value());
  bench.expect_negative(Nrc::ServiceNotSupportedInActiveSession);
}

TEST(Client, DownloadOutOfRangeIsRefused) {
  sim::UdsServerConfig config = sim::default_server_config(server_transport());
  config.flash_size = 1024;
  Bench bench(config);
  bench.unlock();
  ASSERT_TRUE(bench.client.request_download(0x08000000u, 999999, 4, 4, bench.now)
                  .has_value());
  bench.expect_negative(Nrc::RequestOutOfRange);
}

TEST(Client, TransferDataBeforeRequestDownloadIsASequenceError) {
  Bench bench;
  bench.unlock();
  ASSERT_TRUE(bench.client.transfer_data(1, {1, 2, 3}, bench.now).has_value());
  bench.expect_negative(Nrc::RequestSequenceError);
}

TEST(Client, WrongBlockSequenceCounterIsCaught) {
  Bench bench;
  bench.unlock();
  ASSERT_TRUE(bench.client.request_download(0x08000000u, 512, 4, 4, bench.now)
                  .has_value());
  bench.expect_positive();
  // Skipping straight to block 5 must be rejected.
  ASSERT_TRUE(bench.client.transfer_data(5, {1, 2, 3}, bench.now).has_value());
  bench.expect_negative(Nrc::WrongBlockSequenceCounter);
}

TEST(Client, TransferExitBeforeTheImageIsCompleteFails) {
  Bench bench;
  bench.unlock();
  ASSERT_TRUE(bench.client.request_download(0x08000000u, 512, 4, 4, bench.now)
                  .has_value());
  bench.expect_positive();
  ASSERT_TRUE(bench.client.transfer_data(1, {1, 2, 3}, bench.now).has_value());
  bench.expect_positive();
  ASSERT_TRUE(bench.client.request_transfer_exit(bench.now).has_value());
  bench.expect_negative(Nrc::GeneralProgrammingFailure);
}

TEST(Client, RepeatingABlockIsIdempotent) {
  Bench bench;
  bench.unlock();
  ASSERT_TRUE(bench.client.request_download(0x08000000u, 8, 4, 4, bench.now)
                  .has_value());
  bench.expect_positive();
  ASSERT_TRUE(bench.client.transfer_data(1, {1, 2, 3, 4}, bench.now).has_value());
  bench.expect_positive();
  // Retransmitting the block just accepted is answered positively and must not
  // write the data twice.
  ASSERT_TRUE(bench.client.transfer_data(1, {1, 2, 3, 4}, bench.now).has_value());
  bench.expect_positive();
  ASSERT_TRUE(bench.client.transfer_data(2, {5, 6, 7, 8}, bench.now).has_value());
  bench.expect_positive();
  ASSERT_TRUE(bench.client.request_transfer_exit(bench.now).has_value());
  bench.expect_positive();
  EXPECT_EQ(bench.server.flash()[0], 1);
  EXPECT_EQ(bench.server.flash()[7], 8);
}

TEST(SeedKey, ToyAlgorithmIsReproducible) {
  EXPECT_EQ(sim::toy_key_from_seed(0, 0), 0u);
  EXPECT_EQ(sim::toy_key_from_seed(1, 0), 8u);
  // A rotation, not a shift: the top bits come round to the bottom.
  EXPECT_EQ(sim::toy_key_from_seed(0x80000000u, 0), 0x4u);
  EXPECT_EQ(sim::toy_key_from_seed(0x12345678u, 0xA5C3F00Du),
            ((0x12345678u << 3u) | (0x12345678u >> 29u)) ^ 0xA5C3F00Du);
}

}  // namespace
}  // namespace canforge::uds
