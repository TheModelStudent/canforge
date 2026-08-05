// SPDX-License-Identifier: MIT
#include "canforge/sim/UdsServer.hpp"

#include <algorithm>
#include <cstring>

namespace canforge::sim {
namespace {

using uds::Nrc;
using uds::Service;

constexpr std::uint8_t kPositive = uds::kPositiveResponseOffset;
constexpr std::uint8_t kNegative = uds::kNegativeResponse;
constexpr std::uint8_t kSuppress = uds::kSuppressPositiveResponse;

std::uint32_t read_be(const std::vector<std::uint8_t>& data, std::size_t at,
                      std::uint8_t bytes) {
  std::uint32_t value = 0;
  for (std::uint8_t i = 0; i < bytes; ++i) {
    value = (value << 8u) | data[at + i];
  }
  return value;
}

void push_be(std::vector<std::uint8_t>& out, std::uint32_t value, std::uint8_t bytes) {
  for (std::uint8_t i = bytes; i > 0; --i) {
    out.push_back(static_cast<std::uint8_t>((value >> ((i - 1u) * 8u)) & 0xFFu));
  }
}

}  // namespace

std::uint32_t toy_key_from_seed(std::uint32_t seed, std::uint32_t secret) noexcept {
  // NOT CRYPTOGRAPHY. A rotate and an exclusive-or, chosen because it is easy
  // to reimplement in a test and impossible to mistake for something strong.
  // Production seed/key algorithms are supplier secrets, frequently no better
  // than this, and that is a real industry problem rather than a model to
  // copy.
  const std::uint32_t rotated = (seed << 3u) | (seed >> 29u);
  return rotated ^ secret;
}

UdsServerConfig default_server_config(isotp::Config transport) {
  UdsServerConfig config;
  config.transport = transport;

  const std::string vin = "CANFORGE0SIM00001";
  config.data_by_identifier[0xF190] = std::vector<std::uint8_t>(vin.begin(), vin.end());
  const std::string sw = "SW-1.4.2";
  config.data_by_identifier[0xF189] = std::vector<std::uint8_t>(sw.begin(), sw.end());
  const std::string supplier = "canforge";
  config.data_by_identifier[0xF18A] =
      std::vector<std::uint8_t>(supplier.begin(), supplier.end());
  config.data_by_identifier[0x0100] = {0x00, 0x00};  // a writable scratch DID

  config.dtcs = {
      {0x010301,
       static_cast<std::uint8_t>(
           static_cast<std::uint8_t>(uds::DtcStatusBit::ConfirmedDtc) |
           static_cast<std::uint8_t>(uds::DtcStatusBit::TestFailed)),
       "cylinder 1 misfire"},
      {0x011716, static_cast<std::uint8_t>(uds::DtcStatusBit::PendingDtc),
       "coolant temperature circuit low"},
      {0xC00A00,
       static_cast<std::uint8_t>(
           static_cast<std::uint8_t>(uds::DtcStatusBit::ConfirmedDtc) |
           static_cast<std::uint8_t>(uds::DtcStatusBit::WarningIndicatorRequested)),
       "lost communication with TCM"},
  };
  return config;
}

UdsServer::UdsServer(UdsServerConfig config)
    : config_(std::move(config)),
      receiver_(config_.transport),
      sender_(config_.transport),
      dtcs_(config_.dtcs) {
  flash_.assign(config_.flash_size, 0xFFu);  // erased flash reads as all ones
}

std::vector<std::uint8_t> UdsServer::negative(std::uint8_t service, Nrc nrc) const {
  return {kNegative, service, static_cast<std::uint8_t>(nrc)};
}

void UdsServer::on_frame(const core::FdFrame& frame, std::uint64_t now_ns) {
  if (sending_) {
    sender_.on_frame(frame, now_ns);
    return;
  }
  receiver_.on_frame(frame, now_ns);
  if (receiver_.has_message()) {
    const std::vector<std::uint8_t> request = receiver_.message();
    receiver_.reset();
    ++handled_;
    std::vector<std::uint8_t> response = handle(request, now_ns);
    if (!response.empty()) {
      sender_ = isotp::Sender(config_.transport);
      if (sender_.begin(response, now_ns)) {
        sending_ = true;
      }
    }
  }
}

std::vector<core::FdFrame> UdsServer::poll(std::uint64_t now_ns) {
  std::vector<core::FdFrame> out;

  // A deferred response is one the server answered 0x78 to and is still
  // working on. When its time comes, the real answer goes out.
  if (!deferred_response_.empty() && now_ns >= deferred_until_ns_ && !sending_) {
    sender_ = isotp::Sender(config_.transport);
    if (sender_.begin(deferred_response_, now_ns)) {
      sending_ = true;
    }
    deferred_response_.clear();
  }

  if (sending_) {
    for (const core::FdFrame& f : sender_.poll(now_ns)) {
      out.push_back(f);
    }
    if (sender_.done()) {
      sending_ = false;
      receiver_.reset();
    }
    return out;
  }

  // While a response is pending, keep emitting 0x78 so the client's P2* logic
  // is actually exercised, not just present.
  if (pending_left_ > 0 && !deferred_response_.empty()) {
    --pending_left_;
    sender_ = isotp::Sender(config_.transport);
    const std::vector<std::uint8_t> busy = {
        kNegative, deferred_service_,
        static_cast<std::uint8_t>(Nrc::RequestCorrectlyReceivedResponsePending)};
    if (sender_.begin(busy, now_ns)) {
      sending_ = true;
    }
    return out;
  }

  for (const core::FdFrame& f : receiver_.poll(now_ns)) {
    out.push_back(f);
  }
  return out;
}

std::vector<std::uint8_t> UdsServer::handle(const std::vector<std::uint8_t>& request,
                                            std::uint64_t now_ns) {
  if (request.empty()) {
    return {};
  }
  const std::uint8_t service = request[0];
  // The suppressPositiveResponse bit only exists on services that *have* a
  // sub-function. Applying it to, say, ReadDataByIdentifier would read the
  // high bit of a data identifier as a suppression request and answer nothing
  // at all -- which looks exactly like a dead ECU.
  const bool has_sub_function =
      service == static_cast<std::uint8_t>(Service::DiagnosticSessionControl) ||
      service == static_cast<std::uint8_t>(Service::EcuReset) ||
      service == static_cast<std::uint8_t>(Service::ReadDtcInformation) ||
      service == static_cast<std::uint8_t>(Service::SecurityAccess) ||
      service == static_cast<std::uint8_t>(Service::CommunicationControl) ||
      service == static_cast<std::uint8_t>(Service::RoutineControl) ||
      service == static_cast<std::uint8_t>(Service::TesterPresent);
  const bool suppress =
      has_sub_function && request.size() >= 2 && (request[1] & kSuppress) != 0u;
  const auto sub =
      static_cast<std::uint8_t>(request.size() >= 2 ? (request[1] & 0x7Fu) : 0u);

  const auto positive = [&](std::vector<std::uint8_t> body) {
    if (suppress) {
      return std::vector<std::uint8_t>{};
    }
    std::vector<std::uint8_t> out = {static_cast<std::uint8_t>(service + kPositive)};
    out.insert(out.end(), body.begin(), body.end());
    return out;
  };

  switch (static_cast<Service>(service)) {
    case Service::DiagnosticSessionControl: {
      if (request.size() < 2) {
        return negative(service, Nrc::IncorrectMessageLengthOrInvalidFormat);
      }
      if (sub < 1 || sub > 4) {
        return negative(service, Nrc::SubFunctionNotSupported);
      }
      session_ = static_cast<uds::SessionType>(sub);
      if (session_ == uds::SessionType::Default) {
        unlocked_ = false;  // leaving a secured session drops security
      }
      // The body is the session echo plus P2 and P2* in the usual units:
      // milliseconds, and tens of milliseconds.
      std::vector<std::uint8_t> body = {sub};
      push_be(body, 50, 2);
      push_be(body, 500, 2);
      return positive(std::move(body));
    }

    case Service::EcuReset: {
      if (request.size() < 2) {
        return negative(service, Nrc::IncorrectMessageLengthOrInvalidFormat);
      }
      if (sub < 1 || sub > 5) {
        return negative(service, Nrc::SubFunctionNotSupported);
      }
      session_ = uds::SessionType::Default;
      unlocked_ = false;
      download_active_ = false;
      return positive({sub});
    }

    case Service::TesterPresent:
      ++tester_present_;
      if (request.size() < 2 || sub != 0) {
        return negative(service, Nrc::SubFunctionNotSupported);
      }
      return positive({0x00});

    case Service::ReadDataByIdentifier: {
      if (request.size() < 3) {
        return negative(service, Nrc::IncorrectMessageLengthOrInvalidFormat);
      }
      const auto did = static_cast<std::uint16_t>(read_be(request, 1, 2));
      const auto it = config_.data_by_identifier.find(did);
      if (it == config_.data_by_identifier.end()) {
        return negative(service, Nrc::RequestOutOfRange);
      }
      std::vector<std::uint8_t> body;
      push_be(body, did, 2);
      body.insert(body.end(), it->second.begin(), it->second.end());
      return positive(std::move(body));
    }

    case Service::WriteDataByIdentifier: {
      if (request.size() < 4) {
        return negative(service, Nrc::IncorrectMessageLengthOrInvalidFormat);
      }
      const auto did = static_cast<std::uint16_t>(read_be(request, 1, 2));
      const auto it = config_.data_by_identifier.find(did);
      if (it == config_.data_by_identifier.end()) {
        return negative(service, Nrc::RequestOutOfRange);
      }
      // Identifiers in the 0xF1xx range are read-only identification data.
      if (did >= 0xF100u) {
        return negative(service, Nrc::SecurityAccessDenied);
      }
      it->second.assign(request.begin() + 3, request.end());
      std::vector<std::uint8_t> body;
      push_be(body, did, 2);
      return positive(std::move(body));
    }

    case Service::SecurityAccess: {
      if (request.size() < 2) {
        return negative(service, Nrc::IncorrectMessageLengthOrInvalidFormat);
      }
      if (session_ == uds::SessionType::Default) {
        return negative(service, Nrc::ServiceNotSupportedInActiveSession);
      }
      if ((sub & 0x01u) != 0u) {
        // requestSeed. A seed of zero means "already unlocked", which is
        // the standard prescribes and what tools check for.
        security_level_ = sub;
        last_seed_ = unlocked_
                         ? 0u
                         : (0xC0FFEEu ^ static_cast<std::uint32_t>(now_ns & 0xFFFFFFu));
        std::vector<std::uint8_t> body = {sub};
        push_be(body, last_seed_, 4);
        return positive(std::move(body));
      }
      // sendKey
      if (sub != security_level_ + 1u) {
        return negative(service, Nrc::RequestSequenceError);
      }
      if (request.size() < 6) {
        return negative(service, Nrc::IncorrectMessageLengthOrInvalidFormat);
      }
      const std::uint32_t offered = read_be(request, 2, 4);
      if (offered != toy_key_from_seed(last_seed_, config_.security_secret)) {
        return negative(service, Nrc::InvalidKey);
      }
      unlocked_ = true;
      return positive({sub});
    }

    case Service::ClearDiagnosticInformation: {
      if (request.size() < 4) {
        return negative(service, Nrc::IncorrectMessageLengthOrInvalidFormat);
      }
      const std::uint32_t group = read_be(request, 1, 3);
      if (group == 0xFFFFFFu) {
        dtcs_.clear();  // the "all groups" wildcard
      } else {
        dtcs_.erase(std::remove_if(dtcs_.begin(), dtcs_.end(),
                                   [group](const uds::Dtc& d) {
                                     return (d.code >> 16u) == (group >> 16u);
                                   }),
                    dtcs_.end());
      }
      return positive({});
    }

    case Service::ReadDtcInformation: {
      if (request.size() < 2) {
        return negative(service, Nrc::IncorrectMessageLengthOrInvalidFormat);
      }
      const auto type = static_cast<uds::DtcReportType>(sub);
      if (type == uds::DtcReportType::ReportNumberOfDtcByStatusMask) {
        if (request.size() < 3) {
          return negative(service, Nrc::IncorrectMessageLengthOrInvalidFormat);
        }
        const std::uint8_t mask = request[2];
        std::uint16_t count = 0;
        for (const uds::Dtc& d : dtcs_) {
          if ((d.status & mask) != 0u) {
            ++count;
          }
        }
        std::vector<std::uint8_t> body = {sub, 0xFF, 0x01};  // format 0x01
        push_be(body, count, 2);
        return positive(std::move(body));
      }
      if (type == uds::DtcReportType::ReportDtcByStatusMask ||
          type == uds::DtcReportType::ReportSupportedDtc) {
        const std::uint8_t mask = type == uds::DtcReportType::ReportSupportedDtc
                                      ? 0xFFu
                                      : (request.size() >= 3 ? request[2] : 0xFFu);
        std::vector<std::uint8_t> body = {sub, 0xFF};  // status availability
        for (const uds::Dtc& d : dtcs_) {
          if (type == uds::DtcReportType::ReportSupportedDtc ||
              (d.status & mask) != 0u) {
            push_be(body, d.code, 3);
            body.push_back(d.status);
          }
        }
        return positive(std::move(body));
      }
      return negative(service, Nrc::SubFunctionNotSupported);
    }

    case Service::RoutineControl: {
      if (request.size() < 4) {
        return negative(service, Nrc::IncorrectMessageLengthOrInvalidFormat);
      }
      const auto routine = static_cast<std::uint16_t>(read_be(request, 2, 2));
      std::vector<std::uint8_t> body = {sub};
      push_be(body, routine, 2);

      if (routine == 0xFF00u) {  // eraseMemory
        if (!unlocked_) {
          return negative(service, Nrc::SecurityAccessDenied);
        }
        std::fill(flash_.begin(), flash_.end(), 0xFFu);
        if (config_.erase_duration_ns != 0) {
          // Answer 0x78 now and the real result later, which is what a real
          // ECU does for an erase and what the client's P2* handling exists
          // for.
          std::vector<std::uint8_t> real = {
              static_cast<std::uint8_t>(service + kPositive)};
          real.insert(real.end(), body.begin(), body.end());
          real.push_back(0x00);
          deferred_response_ = real;
          deferred_service_ = service;
          deferred_until_ns_ = now_ns + config_.erase_duration_ns;
          pending_left_ = 1;
          return {
              static_cast<std::uint8_t>(kNegative), service,
              static_cast<std::uint8_t>(Nrc::RequestCorrectlyReceivedResponsePending)};
        }
        body.push_back(0x00);
        return positive(std::move(body));
      }
      if (routine == 0xFF01u) {  // checkProgrammingDependencies, used as verify
        std::uint32_t sum = 0;
        for (std::uint32_t i = 0; i < download_written_; ++i) {
          sum = (sum + flash_[i]) & 0xFFFFFFFFu;
        }
        push_be(body, sum, 4);
        return positive(std::move(body));
      }
      return negative(service, Nrc::RequestOutOfRange);
    }

    case Service::RequestDownload: {
      if (request.size() < 4) {
        return negative(service, Nrc::IncorrectMessageLengthOrInvalidFormat);
      }
      if (session_ != uds::SessionType::Programming) {
        return negative(service, Nrc::ServiceNotSupportedInActiveSession);
      }
      if (!unlocked_) {
        return negative(service, Nrc::SecurityAccessDenied);
      }
      const std::uint8_t format = request[2];
      const auto address_bytes = static_cast<std::uint8_t>(format & 0x0Fu);
      const auto size_bytes = static_cast<std::uint8_t>((format >> 4u) & 0x0Fu);
      if (address_bytes == 0 || size_bytes == 0 ||
          request.size() < 3u + address_bytes + size_bytes) {
        return negative(service, Nrc::IncorrectMessageLengthOrInvalidFormat);
      }
      download_address_ = read_be(request, 3, address_bytes);
      download_size_ = read_be(request, 3u + address_bytes, size_bytes);
      if (download_address_ < config_.flash_base ||
          download_address_ - config_.flash_base + download_size_ >
              config_.flash_size) {
        return negative(service, Nrc::RequestOutOfRange);
      }
      download_active_ = true;
      download_complete_ = false;
      download_written_ = 0;
      expected_block_ = 1;

      // lengthFormatIdentifier says how many bytes the maxNumberOfBlockLength
      // field takes; then that maximum, which bounds each TransferData.
      std::vector<std::uint8_t> body = {0x20};  // two bytes follow
      push_be(body, 512, 2);
      return positive(std::move(body));
    }

    case Service::TransferData: {
      if (!download_active_) {
        return negative(service, Nrc::RequestSequenceError);
      }
      if (request.size() < 2) {
        return negative(service, Nrc::IncorrectMessageLengthOrInvalidFormat);
      }
      const std::uint8_t block = request[1];
      if (block != expected_block_) {
        // Repeating the previous block is allowed and idempotent; anything
        // else is a sequence error.
        const auto previous = static_cast<std::uint8_t>(expected_block_ - 1u);
        if (block == previous) {
          return positive({block});
        }
        return negative(service, Nrc::WrongBlockSequenceCounter);
      }
      const std::size_t count = request.size() - 2u;
      const std::uint32_t offset =
          download_address_ - config_.flash_base + download_written_;
      if (offset + count > flash_.size()) {
        return negative(service, Nrc::RequestOutOfRange);
      }
      std::memcpy(flash_.data() + offset, request.data() + 2, count);
      download_written_ += static_cast<std::uint32_t>(count);
      expected_block_ = static_cast<std::uint8_t>(expected_block_ + 1u);
      if (expected_block_ == 0) {
        expected_block_ = 1;  // the counter wraps 0xFF -> 0x01, never to zero
      }

      if (config_.pending_responses_per_block != 0) {
        std::vector<std::uint8_t> real = {
            static_cast<std::uint8_t>(service + kPositive), block};
        deferred_response_ = real;
        deferred_service_ = service;
        deferred_until_ns_ = now_ns;
        pending_left_ = config_.pending_responses_per_block;
        return {
            kNegative, service,
            static_cast<std::uint8_t>(Nrc::RequestCorrectlyReceivedResponsePending)};
      }
      return positive({block});
    }

    case Service::RequestTransferExit: {
      if (!download_active_) {
        return negative(service, Nrc::RequestSequenceError);
      }
      if (download_written_ != download_size_) {
        return negative(service, Nrc::GeneralProgrammingFailure);
      }
      download_active_ = false;
      download_complete_ = true;
      return positive({});
    }

    case Service::CommunicationControl:
      return positive({sub});
  }

  return negative(service, Nrc::ServiceNotSupported);
}

}  // namespace canforge::sim
