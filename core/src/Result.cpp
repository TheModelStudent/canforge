// SPDX-License-Identifier: MIT
#include "canforge/core/Result.hpp"

#include <cstdio>
#include <cstdlib>

namespace canforge::core {

const char* to_string(ErrorCode code) noexcept {
  switch (code) {
      // clang-format off
    case ErrorCode::Ok:                      return "ok";
    case ErrorCode::InvalidArgument:         return "invalid argument";
    case ErrorCode::OutOfRange:              return "out of range";
    case ErrorCode::NotImplemented:          return "not implemented";
    case ErrorCode::Unsupported:             return "unsupported";
    case ErrorCode::FrameBadIdentifier:      return "bad CAN identifier";
    case ErrorCode::FrameBadDlc:             return "bad data length code";
    case ErrorCode::FrameBadFlags:           return "bad frame flag combination";
    case ErrorCode::FramePayloadTooLarge:    return "payload too large";
    case ErrorCode::CodecSignalOutOfBounds:  return "signal does not fit in the payload";
    case ErrorCode::CodecBadBitLength:       return "bad signal bit length";
    case ErrorCode::CodecBadFactor:          return "bad signal scale factor";
    case ErrorCode::CodecValueNotFinite:     return "value is not finite";
    case ErrorCode::CodecMultiplexMismatch:  return "signal is not active for this multiplex value";
    case ErrorCode::CodecUnknownSignal:      return "unknown signal";
    case ErrorCode::ParseIoError:            return "i/o error";
    case ErrorCode::ParseBadEncoding:        return "bad character encoding";
    case ErrorCode::ParseUnexpectedToken:    return "unexpected token";
    case ErrorCode::ParseUnexpectedEof:      return "unexpected end of file";
    case ErrorCode::ParseBadNumber:          return "malformed number";
    case ErrorCode::ParseUnterminatedString: return "unterminated string";
    case ErrorCode::ParseUnknownKeyword:     return "unknown keyword";
    // clang-format on
    case ErrorCode::ParseDuplicateDefinition:
      return "duplicate definition";
      // clang-format off
    case ErrorCode::ParseUndefinedReference: return "reference to something undefined";
    case ErrorCode::ParseSemantic:           return "semantic error";
    case ErrorCode::TransportOpenFailed:     return "could not open the bus";
    case ErrorCode::TransportNotOpen:        return "bus is not open";
    case ErrorCode::TransportWriteFailed:    return "write failed";
    case ErrorCode::TransportReadFailed:     return "read failed";
    case ErrorCode::TransportTimeout:        return "timed out";
    case ErrorCode::TransportUnsupported:    return "unsupported by this backend";
    case ErrorCode::TransportOverflow:       return "receive buffer overflow";
    case ErrorCode::LogBadFormat:            return "malformed log file";
    case ErrorCode::LogUnsupportedVersion:   return "unsupported log version";
    case ErrorCode::LogEndOfFile:            return "end of log";
      // clang-format on
  }
  return "unknown error";
}

namespace detail {

void result_no_value(ErrorCode code, std::string_view msg) noexcept {
  // Not an exception: nothing throws across a canforge library boundary.
  std::fprintf(stderr,
               "canforge: Result::value() called on an error result: %s (%.*s)\n",
               to_string(code), static_cast<int>(msg.size()), msg.data());
  std::abort();
}

void result_no_error() noexcept {
  std::fprintf(stderr, "canforge: Result::error() called on a success result\n");
  std::abort();
}

}  // namespace detail
}  // namespace canforge::core
