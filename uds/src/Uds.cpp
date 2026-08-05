// SPDX-License-Identifier: MIT
#include "canforge/uds/Uds.hpp"

#include <algorithm>
#include <cstdio>

namespace canforge::uds {
namespace {

using core::Error;
using core::ErrorCode;

void push_be(std::vector<std::uint8_t>& out, std::uint32_t value, std::uint8_t bytes) {
  for (std::uint8_t i = bytes; i > 0; --i) {
    out.push_back(static_cast<std::uint8_t>((value >> ((i - 1u) * 8u)) & 0xFFu));
  }
}

}  // namespace

const char* to_string(Service s) noexcept {
  switch (s) {
      // clang-format off
    case Service::DiagnosticSessionControl:   return "DiagnosticSessionControl";
    case Service::EcuReset:                   return "ECUReset";
    case Service::ClearDiagnosticInformation: return "ClearDiagnosticInformation";
    case Service::ReadDtcInformation:         return "ReadDTCInformation";
    case Service::ReadDataByIdentifier:       return "ReadDataByIdentifier";
    case Service::SecurityAccess:             return "SecurityAccess";
    case Service::CommunicationControl:       return "CommunicationControl";
    case Service::WriteDataByIdentifier:      return "WriteDataByIdentifier";
    case Service::RoutineControl:             return "RoutineControl";
    case Service::RequestDownload:            return "RequestDownload";
    case Service::TransferData:               return "TransferData";
    case Service::RequestTransferExit:        return "RequestTransferExit";
    case Service::TesterPresent:              return "TesterPresent";
      // clang-format on
  }
  return "unknown service";
}

const char* to_string(SessionType s) noexcept {
  switch (s) {
      // clang-format off
    case SessionType::Default:                return "default";
    case SessionType::Programming:            return "programming";
    case SessionType::ExtendedDiagnostic:     return "extended diagnostic";
    case SessionType::SafetySystemDiagnostic: return "safety system diagnostic";
      // clang-format on
  }
  return "unknown session";
}

const char* to_string(Nrc code) noexcept {
  switch (code) {
      // clang-format off
    case Nrc::PositiveResponse:            return "positive response";
    case Nrc::GeneralReject:               return "general reject";
    case Nrc::ServiceNotSupported:         return "service not supported";
    case Nrc::SubFunctionNotSupported:     return "sub-function not supported";
    // clang-format on
    case Nrc::IncorrectMessageLengthOrInvalidFormat:
      return "incorrect message length or invalid format";
      // clang-format off
    case Nrc::ResponseTooLong:             return "response too long";
    case Nrc::BusyRepeatRequest:           return "busy, repeat request";
    case Nrc::ConditionsNotCorrect:        return "conditions not correct";
    case Nrc::RequestSequenceError:        return "request sequence error";
    // clang-format on
    case Nrc::NoResponseFromSubnetComponent:
      return "no response from subnet component";
    case Nrc::FailurePreventsExecutionOfRequestedAction:
      return "failure prevents execution of the requested action";
      // clang-format off
    case Nrc::RequestOutOfRange:           return "request out of range";
    case Nrc::SecurityAccessDenied:        return "security access denied";
    case Nrc::InvalidKey:                  return "invalid key";
    case Nrc::ExceedNumberOfAttempts:      return "exceeded number of attempts";
    case Nrc::RequiredTimeDelayNotExpired: return "required time delay not expired";
    case Nrc::UploadDownloadNotAccepted:   return "upload or download not accepted";
    case Nrc::TransferDataSuspended:       return "transfer data suspended";
    case Nrc::GeneralProgrammingFailure:   return "general programming failure";
    case Nrc::WrongBlockSequenceCounter:   return "wrong block sequence counter";
    // clang-format on
    case Nrc::RequestCorrectlyReceivedResponsePending:
      return "request correctly received, response pending";
    case Nrc::SubFunctionNotSupportedInActiveSession:
      return "sub-function not supported in the active session";
    case Nrc::ServiceNotSupportedInActiveSession:
      return "service not supported in the active session";
      // clang-format off
    case Nrc::RpmTooHigh:                  return "engine speed too high";
    case Nrc::RpmTooLow:                   return "engine speed too low";
    case Nrc::EngineIsRunning:             return "engine is running";
    case Nrc::EngineIsNotRunning:          return "engine is not running";
    case Nrc::EngineRunTimeTooLow:         return "engine run time too low";
    case Nrc::TemperatureTooHigh:          return "temperature too high";
    case Nrc::TemperatureTooLow:           return "temperature too low";
    case Nrc::VehicleSpeedTooHigh:         return "vehicle speed too high";
    case Nrc::VehicleSpeedTooLow:          return "vehicle speed too low";
    case Nrc::ThrottlePedalTooHigh:        return "throttle pedal too high";
    case Nrc::ThrottlePedalTooLow:         return "throttle pedal too low";
    // clang-format on
    case Nrc::TransmissionRangeNotInNeutral:
      return "transmission range not in neutral";
      // clang-format off
    case Nrc::TransmissionRangeNotInGear:  return "transmission range not in gear";
    case Nrc::BrakeSwitchNotClosed:        return "brake switch not closed";
    case Nrc::ShifterLeverNotInPark:       return "shifter lever not in park";
    case Nrc::TorqueConverterClutchLocked: return "torque converter clutch locked";
    case Nrc::VoltageTooHigh:              return "voltage too high";
    case Nrc::VoltageTooLow:               return "voltage too low";
      // clang-format on
  }
  return "unknown";
}

std::string describe_nrc(std::uint8_t raw) {
  const char* known = to_string(static_cast<Nrc>(raw));
  char buffer[96];
  if (std::string(known) != "unknown") {
    std::snprintf(buffer, sizeof(buffer), "0x%02X %s", raw, known);
    return buffer;
  }
  // The ranges ISO 14229 reserves, so an unrecognised code still says
  // something useful instead of just "unknown".
  const char* range = "reserved by ISO 14229";
  if (raw >= 0x38u && raw <= 0x4Fu) {
    range = "reserved for ISO 15764 secured data transmission";
  } else if (raw >= 0x94u && raw <= 0xEFu) {
    range = "reserved for condition-driven codes";
  } else if (raw >= 0xF0u) {
    range = "manufacturer specific";
  }
  std::snprintf(buffer, sizeof(buffer), "0x%02X (%s)", raw, range);
  return buffer;
}

std::string format_dtc(std::uint32_t code) {
  // ISO 15031-6 / SAE J2012: the top two bits select the letter, the next two
  // are the first digit, and the rest are hex.
  static const char kLetters[4] = {'P', 'C', 'B', 'U'};
  const auto high = static_cast<std::uint8_t>((code >> 16u) & 0xFFu);
  char buffer[16];
  // Letter, then one digit from bits 5..4, then three hex nibbles: four
  // characters after the letter, never five.
  std::snprintf(buffer, sizeof(buffer), "%c%X%X%02X", kLetters[(high >> 6u) & 0x03u],
                static_cast<unsigned>((high >> 4u) & 0x03u),
                static_cast<unsigned>(high & 0x0Fu),
                static_cast<unsigned>((code >> 8u) & 0xFFu));
  // The low byte of a three-byte DTC is the failure type; append it.
  std::string out(buffer);
  char tail[8];
  std::snprintf(tail, sizeof(tail), "-%02X", static_cast<unsigned>(code & 0xFFu));
  return out + tail;
}

std::string describe_dtc_status(std::uint8_t status) {
  struct Bit {
    DtcStatusBit bit;
    const char* name;
  };
  static const Bit kBits[] = {
      {DtcStatusBit::TestFailed, "testFailed"},
      {DtcStatusBit::TestFailedThisOperationCycle, "testFailedThisOperationCycle"},
      {DtcStatusBit::PendingDtc, "pendingDTC"},
      {DtcStatusBit::ConfirmedDtc, "confirmedDTC"},
      {DtcStatusBit::TestNotCompletedSinceLastClear, "testNotCompletedSinceLastClear"},
      {DtcStatusBit::TestFailedSinceLastClear, "testFailedSinceLastClear"},
      {DtcStatusBit::TestNotCompletedThisOperationCycle,
       "testNotCompletedThisOperationCycle"},
      {DtcStatusBit::WarningIndicatorRequested, "warningIndicatorRequested"},
  };
  std::string out;
  for (const Bit& b : kBits) {
    if ((status & static_cast<std::uint8_t>(b.bit)) != 0u) {
      if (!out.empty()) {
        out += ", ";
      }
      out += b.name;
    }
  }
  return out.empty() ? std::string("none") : out;
}

Client::Client(ClientConfig config)
    : config_(config), sender_(config.transport), receiver_(config.transport) {
  // The sender and the receiver share one address: this side transmits on
  // tx_id and listens on rx_id, and the receiver's flow control frames go back
  // out on tx_id. No mirroring is needed.
}

Status Client::request(std::vector<std::uint8_t> payload, std::uint64_t now_ns) {
  if (busy()) {
    return Error(ErrorCode::InvalidArgument, "a request is already in flight");
  }
  if (payload.empty()) {
    return Error(ErrorCode::InvalidArgument, "a UDS request needs a service id");
  }
  requested_service_ = payload[0];
  // A sub-function with bit 7 set tells the server to stay silent on success.
  expect_response_ = true;
  const bool has_sub_function =
      requested_service_ ==
          static_cast<std::uint8_t>(Service::DiagnosticSessionControl) ||
      requested_service_ == static_cast<std::uint8_t>(Service::EcuReset) ||
      requested_service_ == static_cast<std::uint8_t>(Service::SecurityAccess) ||
      requested_service_ == static_cast<std::uint8_t>(Service::CommunicationControl) ||
      requested_service_ == static_cast<std::uint8_t>(Service::RoutineControl) ||
      requested_service_ == static_cast<std::uint8_t>(Service::TesterPresent) ||
      requested_service_ == static_cast<std::uint8_t>(Service::ReadDtcInformation);
  if (has_sub_function && payload.size() >= 2 &&
      (payload[1] & kSuppressPositiveResponse) != 0u) {
    expect_response_ = false;
  }

  pending_request_ = std::move(payload);
  response_ = Response{};
  pending_count_ = 0;
  state_ = RequestState::Sending;
  begin_transfer(now_ns);
  return core::ok();
}

void Client::begin_transfer(std::uint64_t now_ns) {
  sender_ = isotp::Sender(config_.transport);
  receiver_.reset();
  const Status started = sender_.begin(pending_request_, now_ns);
  if (!started) {
    fail(started.error());
    return;
  }
  deadline_ns_ = now_ns + config_.p2_ns;
  last_activity_ns_ = now_ns;
}

void Client::finish(Response response) {
  response_ = std::move(response);
  response_.pending_count = pending_count_;
  state_ = RequestState::Complete;
}

void Client::fail(core::Error error) {
  failure_ = error;
  state_ = RequestState::Failed;
}

std::vector<core::FdFrame> Client::poll(std::uint64_t now_ns) {
  std::vector<core::FdFrame> out;

  if (state_ == RequestState::Sending) {
    for (const core::FdFrame& f : sender_.poll(now_ns)) {
      out.push_back(f);
    }
    if (sender_.done()) {
      if (sender_.result() != isotp::TransferResult::Ok) {
        fail(Error(ErrorCode::TransportWriteFailed,
                   "the request could not be transported",
                   {static_cast<std::uint32_t>(sender_.result()), 0}));
        return out;
      }
      if (!expect_response_) {
        // The suppressPositiveResponse bit was set, so nothing will come back
        // and waiting for P2 would be dead time.
        Response response;
        response.positive = true;
        response.service = requested_service_;
        finish(std::move(response));
        return out;
      }
      state_ = RequestState::AwaitingResponse;
      deadline_ns_ = now_ns + config_.p2_ns;
    }
  }

  if (state_ == RequestState::AwaitingResponse) {
    for (const core::FdFrame& f : receiver_.poll(now_ns)) {
      out.push_back(f);
    }
    if (receiver_.has_message()) {
      const std::vector<std::uint8_t> message = receiver_.message();
      receiver_.reset();
      handle_message(message, now_ns);
    } else if (receiver_.done()) {
      fail(Error(ErrorCode::TransportReadFailed,
                 "the response could not be reassembled",
                 {static_cast<std::uint32_t>(receiver_.result()), 0}));
    } else if (now_ns >= deadline_ns_) {
      fail(Error(ErrorCode::TransportTimeout, "the server did not answer within P2"));
    }
  }

  // TesterPresent keepalive. A non-default session lapses after S3 without
  // traffic; sending at 40% of S3 leaves margin for a lost frame.
  if (config_.tester_present_keepalive && state_ == RequestState::Complete &&
      session_ != SessionType::Default) {
    const std::uint64_t due = last_activity_ns_ + (config_.s3_ns * 2ULL) / 5ULL;
    if (now_ns >= due) {
      isotp::Sender keepalive(config_.transport);
      const std::vector<std::uint8_t> payload = {
          static_cast<std::uint8_t>(Service::TesterPresent),
          static_cast<std::uint8_t>(0x00u | kSuppressPositiveResponse)};
      if (keepalive.begin(payload, now_ns)) {
        for (const core::FdFrame& f : keepalive.poll(now_ns)) {
          out.push_back(f);
        }
        ++keepalives_;
        last_activity_ns_ = now_ns;
      }
    }
  }
  return out;
}

void Client::on_frame(const core::FdFrame& frame, std::uint64_t now_ns) {
  if (state_ == RequestState::Sending) {
    sender_.on_frame(frame, now_ns);
    return;
  }
  if (state_ == RequestState::AwaitingResponse) {
    receiver_.on_frame(frame, now_ns);
  }
}

void Client::handle_message(const std::vector<std::uint8_t>& message,
                            std::uint64_t now_ns) {
  last_activity_ns_ = now_ns;
  if (message.empty()) {
    fail(Error(ErrorCode::ParseSemantic, "empty UDS response"));
    return;
  }

  if (message[0] == kNegativeResponse) {
    if (message.size() < 3) {
      fail(Error(ErrorCode::ParseSemantic, "truncated negative response"));
      return;
    }
    const std::uint8_t service = message[1];
    const std::uint8_t nrc = message[2];

    if (nrc ==
        static_cast<std::uint8_t>(Nrc::RequestCorrectlyReceivedResponsePending)) {
      // 0x78: the server is still working. Switch to P2* and keep waiting.
      // This is the difference between a client that can flash an ECU and one
      // that cannot -- an erase routine easily takes 30 seconds.
      ++pending_count_;
      if (config_.max_response_pending != 0 &&
          pending_count_ > config_.max_response_pending) {
        fail(Error(ErrorCode::TransportTimeout,
                   "the server kept answering responsePending", {pending_count_, 0}));
        return;
      }
      deadline_ns_ = now_ns + config_.p2_star_ns;
      return;  // stay in AwaitingResponse
    }

    Response response;
    response.positive = false;
    response.service = service;
    response.nrc = nrc;
    finish(std::move(response));
    return;
  }

  if (message[0] != requested_service_ + kPositiveResponseOffset) {
    fail(Error(ErrorCode::ParseSemantic,
               "the response does not answer the request that was sent",
               {message[0], requested_service_}));
    return;
  }

  Response response;
  response.positive = true;
  response.service = message[0];
  response.data.assign(message.begin() + 1, message.end());

  if (requested_service_ ==
          static_cast<std::uint8_t>(Service::DiagnosticSessionControl) &&
      !response.data.empty()) {
    session_ = static_cast<SessionType>(response.data[0] & 0x7Fu);
  }
  finish(std::move(response));
}

Status Client::diagnostic_session_control(SessionType session, std::uint64_t now_ns,
                                          bool suppress_response) {
  auto sub = static_cast<std::uint8_t>(session);
  if (suppress_response) {
    sub = static_cast<std::uint8_t>(sub | kSuppressPositiveResponse);
  }
  return request({static_cast<std::uint8_t>(Service::DiagnosticSessionControl), sub},
                 now_ns);
}

Status Client::ecu_reset(ResetType type, std::uint64_t now_ns) {
  return request(
      {static_cast<std::uint8_t>(Service::EcuReset), static_cast<std::uint8_t>(type)},
      now_ns);
}

Status Client::clear_diagnostic_information(std::uint32_t group, std::uint64_t now_ns) {
  std::vector<std::uint8_t> payload = {
      static_cast<std::uint8_t>(Service::ClearDiagnosticInformation)};
  push_be(payload, group, 3);
  return request(std::move(payload), now_ns);
}

Status Client::read_dtc_information(DtcReportType type, std::uint8_t status_mask,
                                    std::uint64_t now_ns) {
  return request({static_cast<std::uint8_t>(Service::ReadDtcInformation),
                  static_cast<std::uint8_t>(type), status_mask},
                 now_ns);
}

Status Client::read_data_by_identifier(std::uint16_t did, std::uint64_t now_ns) {
  std::vector<std::uint8_t> payload = {
      static_cast<std::uint8_t>(Service::ReadDataByIdentifier)};
  push_be(payload, did, 2);
  return request(std::move(payload), now_ns);
}

Status Client::write_data_by_identifier(std::uint16_t did,
                                        const std::vector<std::uint8_t>& value,
                                        std::uint64_t now_ns) {
  std::vector<std::uint8_t> payload = {
      static_cast<std::uint8_t>(Service::WriteDataByIdentifier)};
  push_be(payload, did, 2);
  payload.insert(payload.end(), value.begin(), value.end());
  return request(std::move(payload), now_ns);
}

Status Client::security_access_request_seed(std::uint8_t level, std::uint64_t now_ns) {
  // Odd sub-functions request a seed, even ones send the key.
  if ((level & 0x01u) == 0u) {
    return Error(ErrorCode::InvalidArgument, "a requestSeed sub-function must be odd");
  }
  return request({static_cast<std::uint8_t>(Service::SecurityAccess), level}, now_ns);
}

Status Client::security_access_send_key(std::uint8_t level,
                                        const std::vector<std::uint8_t>& key,
                                        std::uint64_t now_ns) {
  if ((level & 0x01u) != 0u) {
    return Error(ErrorCode::InvalidArgument, "a sendKey sub-function must be even");
  }
  std::vector<std::uint8_t> payload = {
      static_cast<std::uint8_t>(Service::SecurityAccess), level};
  payload.insert(payload.end(), key.begin(), key.end());
  return request(std::move(payload), now_ns);
}

Status Client::routine_control(RoutineControlType type, std::uint16_t routine,
                               const std::vector<std::uint8_t>& parameters,
                               std::uint64_t now_ns) {
  std::vector<std::uint8_t> payload = {
      static_cast<std::uint8_t>(Service::RoutineControl),
      static_cast<std::uint8_t>(type)};
  push_be(payload, routine, 2);
  payload.insert(payload.end(), parameters.begin(), parameters.end());
  return request(std::move(payload), now_ns);
}

Status Client::request_download(std::uint32_t address, std::uint32_t size,
                                std::uint8_t address_bytes, std::uint8_t size_bytes,
                                std::uint64_t now_ns) {
  if (address_bytes == 0 || address_bytes > 4 || size_bytes == 0 || size_bytes > 4) {
    return Error(ErrorCode::InvalidArgument,
                 "address and size must each be one to four bytes");
  }
  std::vector<std::uint8_t> payload = {
      static_cast<std::uint8_t>(Service::RequestDownload),
      0x00,  // dataFormatIdentifier: no compression, no encryption
      static_cast<std::uint8_t>((size_bytes << 4u) | address_bytes)};
  push_be(payload, address, address_bytes);
  push_be(payload, size, size_bytes);
  return request(std::move(payload), now_ns);
}

Status Client::transfer_data(std::uint8_t block_counter,
                             const std::vector<std::uint8_t>& data,
                             std::uint64_t now_ns) {
  std::vector<std::uint8_t> payload = {static_cast<std::uint8_t>(Service::TransferData),
                                       block_counter};
  payload.insert(payload.end(), data.begin(), data.end());
  return request(std::move(payload), now_ns);
}

Status Client::request_transfer_exit(std::uint64_t now_ns) {
  return request({static_cast<std::uint8_t>(Service::RequestTransferExit)}, now_ns);
}

Status Client::tester_present(std::uint64_t now_ns, bool suppress_response) {
  const auto sub =
      static_cast<std::uint8_t>(suppress_response ? kSuppressPositiveResponse : 0x00u);
  return request({static_cast<std::uint8_t>(Service::TesterPresent), sub}, now_ns);
}

std::vector<Dtc> Client::parse_dtc_list(const std::vector<std::uint8_t>& data) {
  std::vector<Dtc> out;
  // Body of a 0x19/0x02 response: sub-function echo, status availability mask,
  // then four bytes per DTC (three of code, one of status).
  if (data.size() < 2) {
    return out;
  }
  for (std::size_t i = 2; i + 3 < data.size(); i += 4) {
    Dtc dtc;
    dtc.code = (static_cast<std::uint32_t>(data[i]) << 16u) |
               (static_cast<std::uint32_t>(data[i + 1]) << 8u) |
               static_cast<std::uint32_t>(data[i + 2]);
    dtc.status = data[i + 3];
    out.push_back(std::move(dtc));
  }
  return out;
}

}  // namespace canforge::uds
