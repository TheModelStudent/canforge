// SPDX-License-Identifier: MIT
#ifndef CANFORGE_UDS_UDS_HPP
#define CANFORGE_UDS_UDS_HPP

/// ISO 14229 (UDS) service identifiers, negative response codes, and a
/// poll-driven diagnostic client that runs over an ISO-TP session.
///
/// Two details carry most of the real-world weight. NRC 0x78
/// (requestCorrectlyReceived-ResponsePending) means the server needs longer
/// than P2, so the client switches to the much longer P2* and keeps waiting,
/// possibly many times -- a flash erase takes tens of seconds, and a client
/// that treats 0x78 as a failure cannot flash anything. Bit 7 of a sub-function
/// also suppresses the positive response entirely, so a client that waits for a
/// reply that will never come stalls for a full P2.

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "canforge/core/Result.hpp"
#include "canforge/isotp/IsoTp.hpp"

namespace canforge::uds {

using core::Result;
using core::Status;

enum class Service : std::uint8_t {
  DiagnosticSessionControl = 0x10,
  EcuReset = 0x11,
  ClearDiagnosticInformation = 0x14,
  ReadDtcInformation = 0x19,
  ReadDataByIdentifier = 0x22,
  SecurityAccess = 0x27,
  CommunicationControl = 0x28,
  WriteDataByIdentifier = 0x2E,
  RoutineControl = 0x31,
  RequestDownload = 0x34,
  TransferData = 0x36,
  RequestTransferExit = 0x37,
  TesterPresent = 0x3E,
};

/// A positive response echoes the request service with bit 6 set.
inline constexpr std::uint8_t kPositiveResponseOffset = 0x40;
inline constexpr std::uint8_t kNegativeResponse = 0x7F;
/// Bit 7 of a sub-function suppresses the positive response.
inline constexpr std::uint8_t kSuppressPositiveResponse = 0x80;

enum class SessionType : std::uint8_t {
  Default = 0x01,
  Programming = 0x02,
  ExtendedDiagnostic = 0x03,
  SafetySystemDiagnostic = 0x04,
};

enum class ResetType : std::uint8_t {
  HardReset = 0x01,
  KeyOffOnReset = 0x02,
  SoftReset = 0x03,
  EnableRapidPowerShutDown = 0x04,
  DisableRapidPowerShutDown = 0x05,
};

enum class RoutineControlType : std::uint8_t {
  StartRoutine = 0x01,
  StopRoutine = 0x02,
  RequestRoutineResults = 0x03,
};

enum class DtcReportType : std::uint8_t {
  ReportNumberOfDtcByStatusMask = 0x01,
  ReportDtcByStatusMask = 0x02,
  ReportSupportedDtc = 0x0A,
};

/// ISO 14229-1 DTC status bits.
enum class DtcStatusBit : std::uint8_t {
  TestFailed = 0x01,
  TestFailedThisOperationCycle = 0x02,
  PendingDtc = 0x04,
  ConfirmedDtc = 0x08,
  TestNotCompletedSinceLastClear = 0x10,
  TestFailedSinceLastClear = 0x20,
  TestNotCompletedThisOperationCycle = 0x40,
  WarningIndicatorRequested = 0x80,
};

struct Dtc {
  std::uint32_t code = 0;  ///< Three bytes, as sent on the wire.
  std::uint8_t status = 0;
  std::string text;  ///< Optional human label, not part of the protocol.
};

/// Format a three-byte DTC the way a scan tool shows it, e.g. P0301.
std::string format_dtc(std::uint32_t code);

const char* to_string(Service s) noexcept;
const char* to_string(SessionType s) noexcept;
std::string describe_dtc_status(std::uint8_t status);

enum class Nrc : std::uint8_t {
  PositiveResponse = 0x00,
  GeneralReject = 0x10,
  ServiceNotSupported = 0x11,
  SubFunctionNotSupported = 0x12,
  IncorrectMessageLengthOrInvalidFormat = 0x13,
  ResponseTooLong = 0x14,
  BusyRepeatRequest = 0x21,
  ConditionsNotCorrect = 0x22,
  RequestSequenceError = 0x24,
  NoResponseFromSubnetComponent = 0x25,
  FailurePreventsExecutionOfRequestedAction = 0x26,
  RequestOutOfRange = 0x31,
  SecurityAccessDenied = 0x33,
  InvalidKey = 0x35,
  ExceedNumberOfAttempts = 0x36,
  RequiredTimeDelayNotExpired = 0x37,
  UploadDownloadNotAccepted = 0x70,
  TransferDataSuspended = 0x71,
  GeneralProgrammingFailure = 0x72,
  WrongBlockSequenceCounter = 0x73,
  RequestCorrectlyReceivedResponsePending = 0x78,
  SubFunctionNotSupportedInActiveSession = 0x7E,
  ServiceNotSupportedInActiveSession = 0x7F,
  RpmTooHigh = 0x81,
  RpmTooLow = 0x82,
  EngineIsRunning = 0x83,
  EngineIsNotRunning = 0x84,
  EngineRunTimeTooLow = 0x85,
  TemperatureTooHigh = 0x86,
  TemperatureTooLow = 0x87,
  VehicleSpeedTooHigh = 0x88,
  VehicleSpeedTooLow = 0x89,
  ThrottlePedalTooHigh = 0x8A,
  ThrottlePedalTooLow = 0x8B,
  TransmissionRangeNotInNeutral = 0x8C,
  TransmissionRangeNotInGear = 0x8D,
  BrakeSwitchNotClosed = 0x8F,
  ShifterLeverNotInPark = 0x90,
  TorqueConverterClutchLocked = 0x91,
  VoltageTooHigh = 0x92,
  VoltageTooLow = 0x93,
};

/// Readable name for any NRC, including the reserved and manufacturer ranges.
const char* to_string(Nrc code) noexcept;
std::string describe_nrc(std::uint8_t raw);

struct ClientConfig {
  isotp::Config transport;
  /// P2: how long the server has to answer. ISO default 50 ms.
  std::uint64_t p2_ns = 50000000ULL;
  /// P2*: the extended timeout after an NRC 0x78. ISO default 5000 ms.
  std::uint64_t p2_star_ns = 5000000000ULL;
  /// How many 0x78 responses to accept before giving up. Zero means unlimited,
  /// which is what a flash routine sometimes needs.
  std::uint32_t max_response_pending = 100;
  /// S3: the server drops out of a non-default session after this long
  /// without traffic. TesterPresent is sent at 40% of it.
  std::uint64_t s3_ns = 5000000000ULL;
  bool tester_present_keepalive = true;
};

enum class RequestState : std::uint8_t {
  Idle,
  Sending,
  AwaitingResponse,
  Complete,
  Failed,
};

struct Response {
  bool positive = false;
  std::uint8_t service = 0;
  std::uint8_t nrc = 0;             ///< Zero when positive.
  std::vector<std::uint8_t> data;   ///< Payload after the service identifier.
  std::uint32_t pending_count = 0;  ///< How many 0x78 responses were absorbed.
};

/// One request/response exchange, driven the same way as an ISO-TP session:
/// `on_frame()` and `poll(now)`, no threads and no sleeping.
class Client {
 public:
  explicit Client(ClientConfig config);

  /// Queue a request. `payload` is the full service payload including the
  /// service identifier byte.
  Status request(std::vector<std::uint8_t> payload, std::uint64_t now_ns);

  Status diagnostic_session_control(SessionType session, std::uint64_t now_ns,
                                    bool suppress_response = false);
  Status ecu_reset(ResetType type, std::uint64_t now_ns);
  Status clear_diagnostic_information(std::uint32_t group, std::uint64_t now_ns);
  Status read_dtc_information(DtcReportType type, std::uint8_t status_mask,
                              std::uint64_t now_ns);
  Status read_data_by_identifier(std::uint16_t did, std::uint64_t now_ns);
  Status write_data_by_identifier(std::uint16_t did,
                                  const std::vector<std::uint8_t>& value,
                                  std::uint64_t now_ns);
  Status security_access_request_seed(std::uint8_t level, std::uint64_t now_ns);
  Status security_access_send_key(std::uint8_t level,
                                  const std::vector<std::uint8_t>& key,
                                  std::uint64_t now_ns);
  Status routine_control(RoutineControlType type, std::uint16_t routine,
                         const std::vector<std::uint8_t>& parameters,
                         std::uint64_t now_ns);
  Status request_download(std::uint32_t address, std::uint32_t size,
                          std::uint8_t address_bytes, std::uint8_t size_bytes,
                          std::uint64_t now_ns);
  Status transfer_data(std::uint8_t block_counter,
                       const std::vector<std::uint8_t>& data, std::uint64_t now_ns);
  Status request_transfer_exit(std::uint64_t now_ns);
  Status tester_present(std::uint64_t now_ns, bool suppress_response = true);

  void on_frame(const core::FdFrame& frame, std::uint64_t now_ns);
  std::vector<core::FdFrame> poll(std::uint64_t now_ns);

  RequestState state() const noexcept { return state_; }
  bool busy() const noexcept {
    return state_ == RequestState::Sending || state_ == RequestState::AwaitingResponse;
  }
  /// Valid once state() is Complete.
  const Response& response() const noexcept { return response_; }
  /// Set when state() is Failed.
  const core::Error& failure() const noexcept { return failure_; }

  /// Parse a ReadDTCInformation 0x02 response body into DTC records.
  static std::vector<Dtc> parse_dtc_list(const std::vector<std::uint8_t>& data);

  /// The session the client believes the server is in, updated when a
  /// DiagnosticSessionControl succeeds. Drives the keepalive.
  SessionType session() const noexcept { return session_; }
  std::uint64_t tester_present_sent() const noexcept { return keepalives_; }

  const ClientConfig& config() const noexcept { return config_; }

 private:
  void begin_transfer(std::uint64_t now_ns);
  void finish(Response response);
  void fail(core::Error error);
  void handle_message(const std::vector<std::uint8_t>& message, std::uint64_t now_ns);

  ClientConfig config_;
  isotp::Sender sender_;
  isotp::Receiver receiver_;
  std::vector<std::uint8_t> pending_request_;
  std::uint8_t requested_service_ = 0;
  bool expect_response_ = true;
  RequestState state_ = RequestState::Idle;
  Response response_;
  core::Error failure_;
  std::uint64_t deadline_ns_ = 0;
  std::uint32_t pending_count_ = 0;
  SessionType session_ = SessionType::Default;
  std::uint64_t last_activity_ns_ = 0;
  std::uint64_t keepalives_ = 0;
};

}  // namespace canforge::uds

#endif  // CANFORGE_UDS_UDS_HPP
