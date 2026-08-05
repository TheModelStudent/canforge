// SPDX-License-Identifier: MIT
#include "canforge/transport/LogFormat.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <sstream>

#include "canforge/transport/IBus.hpp"

namespace canforge::transport {
namespace {

using core::CanId;
using core::Error;
using core::ErrorCode;
using core::FrameFlags;

Error malformed(std::string_view why, std::uint32_t line = 0) {
  return Error(ErrorCode::LogBadFormat, why, {line, 0});
}

bool hex_nibble(char c, std::uint8_t& out) noexcept {
  if (c >= '0' && c <= '9') {
    out = static_cast<std::uint8_t>(c - '0');
  } else if (c >= 'a' && c <= 'f') {
    out = static_cast<std::uint8_t>(c - 'a' + 10);
  } else if (c >= 'A' && c <= 'F') {
    out = static_cast<std::uint8_t>(c - 'A' + 10);
  } else {
    return false;
  }
  return true;
}

bool parse_hex(std::string_view text, std::uint64_t& out) noexcept {
  if (text.empty() || text.size() > 16) {
    return false;
  }
  out = 0;
  for (const char c : text) {
    std::uint8_t nibble = 0;
    if (!hex_nibble(c, nibble)) {
      return false;
    }
    out = (out << 4u) | nibble;
  }
  return true;
}

bool parse_hex_bytes(std::string_view text, std::uint8_t* out, std::size_t max,
                     std::size_t& count) noexcept {
  if ((text.size() % 2u) != 0u) {
    return false;
  }
  count = text.size() / 2u;
  if (count > max) {
    return false;
  }
  for (std::size_t i = 0; i < count; ++i) {
    std::uint8_t hi = 0;
    std::uint8_t lo = 0;
    if (!hex_nibble(text[2 * i], hi) || !hex_nibble(text[2 * i + 1], lo)) {
      return false;
    }
    out[i] = static_cast<std::uint8_t>((hi << 4u) | lo);
  }
  return true;
}

std::string_view trim(std::string_view s) noexcept {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) {
    s.remove_prefix(1);
  }
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) {
    s.remove_suffix(1);
  }
  return s;
}

/// Parse "seconds.fraction" into nanoseconds without going through a double.
///
/// A candump timestamp is a Unix epoch second with microsecond precision, so
/// the full value needs 19 significant digits -- three more than a double can
/// hold. Parsing via strtod silently rounds it, which shows up as a log that
/// no longer round-trips through the writer.
bool parse_seconds_to_ns(std::string_view text, std::uint64_t& out) noexcept {
  out = 0;
  if (text.empty()) {
    return false;
  }
  std::size_t i = 0;
  std::uint64_t whole = 0;
  while (i < text.size() && text[i] >= '0' && text[i] <= '9') {
    whole = whole * 10u + static_cast<std::uint64_t>(text[i] - '0');
    ++i;
  }
  if (i == 0) {
    return false;
  }
  std::uint64_t fraction = 0;
  if (i < text.size() && text[i] == '.') {
    ++i;
    std::uint64_t scale = 100000000ULL;  // first digit is 1e-1 s = 1e8 ns
    while (i < text.size() && text[i] >= '0' && text[i] <= '9') {
      if (scale != 0) {
        fraction += static_cast<std::uint64_t>(text[i] - '0') * scale;
        scale /= 10u;
      }
      ++i;  // digits beyond nanosecond resolution are discarded
    }
  }
  if (i != text.size()) {
    return false;
  }
  out = whole * 1000000000ULL + fraction;
  return true;
}

std::vector<std::string_view> split_whitespace(std::string_view s) {
  std::vector<std::string_view> parts;
  std::size_t i = 0;
  while (i < s.size()) {
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) {
      ++i;
    }
    const std::size_t start = i;
    while (i < s.size() && s[i] != ' ' && s[i] != '\t') {
      ++i;
    }
    if (i > start) {
      parts.push_back(s.substr(start, i - start));
    }
  }
  return parts;
}

std::string to_hex(const std::uint8_t* data, std::size_t n, bool spaced) {
  static const char* kDigits = "0123456789ABCDEF";
  std::string out;
  out.reserve(n * (spaced ? 3u : 2u));
  for (std::size_t i = 0; i < n; ++i) {
    if (spaced && i != 0) {
      out.push_back(' ');
    }
    out.push_back(kDigits[data[i] >> 4u]);
    out.push_back(kDigits[data[i] & 0x0Fu]);
  }
  return out;
}

}  // namespace

const char* to_string(LogFormat f) noexcept {
  switch (f) {
    case LogFormat::Candump: return "candump";
    case LogFormat::Asc:     return "asc";
    case LogFormat::Blf:     return "blf";
  }
  return "unknown";
}

Result<std::vector<LogRecord>> LogReader::read_all() {
  std::vector<LogRecord> out;
  for (;;) {
    auto r = next();
    if (!r) {
      if (r.error().code() == ErrorCode::LogEndOfFile) {
        return out;
      }
      return r.error();
    }
    out.push_back(std::move(r).value());
  }
}

// candump
//
// The format written by `candump -l`:
//
//     (1735689600.123456) vcan0 123#DEADBEEF
//     (1735689600.124000) vcan0 18FEF100#0102030405060708
//     (1735689600.125000) vcan0 123##1DEADBEEF          <- CAN FD
//     (1735689600.126000) vcan0 20000004#0000000000000000  <- error frame
//     (1735689600.127000) vcan0 123#R8                    <- remote, DLC 8
//
// An 8-digit identifier means an extended frame; 3 digits means standard.
// A doubled '#' introduces CAN FD, with one hex digit of flags before the
// data (bit 0 BRS, bit 1 ESI).

Result<LogRecord> parse_candump_line(std::string_view line) {
  line = trim(line);
  if (line.empty() || line.front() == '#') {
    return malformed("blank or comment line");
  }
  const auto parts = split_whitespace(line);
  if (parts.size() < 3) {
    return malformed("a candump line needs a timestamp, an interface and a frame");
  }

  std::string_view stamp = parts[0];
  if (stamp.size() < 3 || stamp.front() != '(' || stamp.back() != ')') {
    return malformed("the timestamp must be wrapped in parentheses");
  }
  stamp.remove_prefix(1);
  stamp.remove_suffix(1);
  std::uint64_t timestamp_ns = 0;
  if (!parse_seconds_to_ns(stamp, timestamp_ns)) {
    return malformed("the timestamp is not a decimal number of seconds");
  }

  LogRecord record;
  record.channel = std::string(parts[1]);

  std::string_view body = parts[2];
  const std::size_t hash = body.find('#');
  if (hash == std::string_view::npos) {
    return malformed("no '#' separating the identifier from the data");
  }
  const std::string_view id_text = body.substr(0, hash);
  std::string_view rest = body.substr(hash + 1);

  std::uint64_t raw_id = 0;
  if (!parse_hex(id_text, raw_id)) {
    return malformed("the identifier is not hexadecimal");
  }
  // can-utils writes exactly three hex digits for a standard identifier and
  // exactly eight for an extended one; the width is the only signal.
  const bool extended = id_text.size() > 3;

  FrameFlags flags = FrameFlags::None;
  bool fd = false;
  if (!rest.empty() && rest.front() == '#') {
    fd = true;
    rest.remove_prefix(1);
    if (rest.empty()) {
      return malformed("a CAN FD frame needs a flags nibble after '##'");
    }
    std::uint8_t nibble = 0;
    if (!hex_nibble(rest.front(), nibble)) {
      return malformed("the CAN FD flags nibble is not hexadecimal");
    }
    rest.remove_prefix(1);
    flags |= FrameFlags::Fd;
    if ((nibble & 0x1u) != 0u) {
      flags |= FrameFlags::Brs;
    }
    if ((nibble & 0x2u) != 0u) {
      flags |= FrameFlags::Esi;
    }
  }

  // Error frames carry the CAN_ERR_FLAG in the identifier.
  constexpr std::uint32_t kErrFlag = 0x20000000u;
  if (extended && (raw_id & kErrFlag) != 0u && id_text.size() == 8) {
    flags |= FrameFlags::Error;
  }

  auto id = extended ? CanId::extended(static_cast<std::uint32_t>(raw_id) & 0x1FFFFFFFu)
                     : CanId::standard(static_cast<std::uint32_t>(raw_id));
  if (!id) {
    return id.error();
  }

  if (!rest.empty() && (rest.front() == 'R' || rest.front() == 'r')) {
    rest.remove_prefix(1);
    std::uint64_t requested = 0;
    if (!rest.empty() && !parse_hex(rest, requested)) {
      return malformed("the remote frame length is not a number");
    }
    auto frame = core::FdFrame::make_remote(
        id.value(), static_cast<std::uint8_t>(requested), timestamp_ns);
    if (!frame) {
      return frame.error();
    }
    record.frame = frame.value();
    return record;
  }

  std::array<std::uint8_t, 64> payload{};
  std::size_t length = 0;
  if (!parse_hex_bytes(rest, payload.data(), payload.size(), length)) {
    return malformed("the payload is not an even number of hex digits");
  }
  if (!fd && length > 8) {
    return malformed("a classic CAN frame cannot carry more than eight bytes");
  }
  auto frame =
      core::FdFrame::make(id.value(), payload.data(), length, flags, timestamp_ns);
  if (!frame) {
    return frame.error();
  }
  record.frame = frame.value();
  return record;
}

std::string format_candump_line(const LogRecord& record) {
  const FdFrame& f = record.frame;
  std::ostringstream os;
  const std::uint64_t ns = f.timestamp_ns();
  os << '(' << (ns / 1000000000ULL) << '.' << std::setfill('0') << std::setw(6)
     << ((ns % 1000000000ULL) / 1000ULL) << ") " << record.channel << ' ';

  std::uint32_t id = f.id().value();
  if (f.is_error_frame()) {
    id |= 0x20000000u;
  }
  os << std::hex << std::uppercase << std::setfill('0')
     << std::setw(f.id().is_extended() || f.is_error_frame() ? 8 : 3) << id;

  if (f.is_remote()) {
    os << "#R" << std::dec << unsigned{f.dlc()};
    return os.str();
  }
  if (f.is_fd()) {
    std::uint32_t nibble = 0;
    if (f.is_brs()) {
      nibble |= 1u;
    }
    if (f.is_esi()) {
      nibble |= 2u;
    }
    os << "##" << nibble;
  } else {
    os << '#';
  }
  os << to_hex(f.data(), f.size(), false);
  return os.str();
}

namespace {

class CandumpReader final : public LogReader {
 public:
  explicit CandumpReader(std::ifstream in) : in_(std::move(in)) {}

  Result<LogRecord> next() override {
    std::string line;
    while (std::getline(in_, line)) {
      ++line_number_;
      const std::string_view trimmed = trim(line);
      if (trimmed.empty() || trimmed.front() == '#') {
        continue;
      }
      auto record = parse_candump_line(trimmed);
      if (!record) {
        return record.error().with({line_number_, 0});
      }
      return record;
    }
    return Error(ErrorCode::LogEndOfFile, "end of candump log");
  }

 private:
  std::ifstream in_;
  std::uint32_t line_number_ = 0;
};

class CandumpWriter final : public LogWriter {
 public:
  explicit CandumpWriter(std::ofstream out) : out_(std::move(out)) {}

  Status write(const LogRecord& record) override {
    out_ << format_candump_line(record) << '\n';
    return out_ ? core::ok()
                : Status(Error(ErrorCode::ParseIoError, "write failed"));
  }
  Status finish() override {
    out_.flush();
    return core::ok();
  }

 private:
  std::ofstream out_;
};

// Vector ASC
//
//     date Wed Jan  1 00:00:00 2025
//     base hex  timestamps absolute
//     internal events logged
//     // version 8.5.0
//        0.010000 1  123             Rx   d 8 DE AD BE EF 00 00 00 00
//        0.011000 1  18FEF100x       Rx   d 8 01 02 03 04 05 06 07 08
//        0.012000 CANFD   1 Rx   123  0 0 8 8 11 22 ...
//
// The trailing 'x' on an identifier marks an extended frame. Classic lines
// use `d <len> <bytes>`; a remote frame uses `r`. CAN FD lines start with the
// literal CANFD and have a different field order, which is why they are
// handled separately.

class AscReader final : public LogReader {
 public:
  explicit AscReader(std::ifstream in) : in_(std::move(in)) {}

  Result<LogRecord> next() override {
    std::string line;
    while (std::getline(in_, line)) {
      ++line_number_;
      const std::string_view trimmed = trim(line);
      if (trimmed.empty() || trimmed.rfind("//", 0) == 0) {
        continue;
      }
      // Header lines. `base hex` vs `base dec` changes how identifiers and
      // payload bytes are read; almost every file in the wild is hex.
      if (trimmed.rfind("base ", 0) == 0) {
        hex_base_ = trimmed.find("hex") != std::string_view::npos;
        continue;
      }
      if (!std::isdigit(static_cast<unsigned char>(trimmed.front()))) {
        continue;  // date, "internal events logged", "Begin Triggerblock", ...
      }
      auto record = parse_line(trimmed);
      if (!record) {
        if (record.error().code() == ErrorCode::LogEndOfFile) {
          continue;  // a line we deliberately skip, e.g. a bus statistic event
        }
        return record.error().with({line_number_, 0});
      }
      return record;
    }
    return Error(ErrorCode::LogEndOfFile, "end of ASC log");
  }

 private:
  Result<LogRecord> parse_line(std::string_view line) const {
    const auto parts = split_whitespace(line);
    if (parts.size() < 3) {
      return Error(ErrorCode::LogEndOfFile, "not a frame line");
    }
    std::uint64_t timestamp_ns = 0;
    if (!parse_seconds_to_ns(parts[0], timestamp_ns)) {
      return Error(ErrorCode::LogEndOfFile, "not a frame line");
    }

    if (parts[1] == "CANFD") {
      return parse_fd(parts, timestamp_ns);
    }
    // Anything that is not a frame -- ErrorFrame, Statistic, J1939TP -- is
    // skipped rather than treated as a parse failure.
    if (parts.size() < 6) {
      return Error(ErrorCode::LogEndOfFile, "not a frame line");
    }
    return parse_classic(parts, timestamp_ns);
  }

  Result<LogRecord> parse_classic(const std::vector<std::string_view>& parts,
                                  std::uint64_t timestamp_ns) const {
    LogRecord record;
    record.channel = std::string(parts[1]);

    std::string_view id_text = parts[2];
    bool extended = false;
    if (!id_text.empty() && (id_text.back() == 'x' || id_text.back() == 'X')) {
      extended = true;
      id_text.remove_suffix(1);
    }
    std::uint64_t raw_id = 0;
    if (hex_base_) {
      if (!parse_hex(id_text, raw_id)) {
        return Error(ErrorCode::LogEndOfFile, "not a frame line");
      }
    } else {
      raw_id = std::strtoull(std::string(id_text).c_str(), nullptr, 10);
    }

    const std::string_view direction = parts[3];
    record.is_rx = (direction == "Rx");

    // parts[4] is 'd' for a data frame or 'r' for a remote one.
    const std::string_view kind = parts[4];
    auto id = extended
                  ? CanId::extended(static_cast<std::uint32_t>(raw_id) & 0x1FFFFFFFu)
                  : CanId::standard(static_cast<std::uint32_t>(raw_id));
    if (!id) {
      return id.error();
    }

    const auto length = static_cast<std::size_t>(
        std::strtoul(std::string(parts[5]).c_str(), nullptr, hex_base_ ? 16 : 10));

    if (kind == "r" || kind == "R") {
      auto frame = core::FdFrame::make_remote(
          id.value(), static_cast<std::uint8_t>(length), timestamp_ns);
      if (!frame) {
        return frame.error();
      }
      record.frame = frame.value();
      return record;
    }

    std::array<std::uint8_t, 64> payload{};
    const std::size_t available = parts.size() > 6 ? parts.size() - 6 : 0;
    const std::size_t take = std::min({length, available, payload.size()});
    for (std::size_t i = 0; i < take; ++i) {
      std::uint64_t byte = 0;
      if (!parse_hex(parts[6 + i], byte)) {
        break;
      }
      payload[i] = static_cast<std::uint8_t>(byte);
    }
    auto frame = core::FdFrame::make(id.value(), payload.data(), take,
                                     FrameFlags::None, timestamp_ns);
    if (!frame) {
      return frame.error();
    }
    record.frame = frame.value();
    return record;
  }

  Result<LogRecord> parse_fd(const std::vector<std::string_view>& parts,
                             std::uint64_t timestamp_ns) const {
    //  t CANFD ch dir id brs esi dlc len data...
    //  0 1     2  3   4  5   6   7   8   9
    if (parts.size() < 9) {
      return Error(ErrorCode::LogEndOfFile, "truncated CAN FD line");
    }
    LogRecord record;
    record.channel = std::string(parts[2]);
    record.is_rx = (parts[3] == "Rx");

    std::string_view id_text = parts[4];
    bool extended = false;
    if (!id_text.empty() && (id_text.back() == 'x' || id_text.back() == 'X')) {
      extended = true;
      id_text.remove_suffix(1);
    }
    std::uint64_t raw_id = 0;
    if (!parse_hex(id_text, raw_id)) {
      return Error(ErrorCode::LogEndOfFile, "not a frame line");
    }
    auto id = extended
                  ? CanId::extended(static_cast<std::uint32_t>(raw_id) & 0x1FFFFFFFu)
                  : CanId::standard(static_cast<std::uint32_t>(raw_id));
    if (!id) {
      return id.error();
    }

    FrameFlags flags = FrameFlags::Fd;
    if (parts[5] != "0") {
      flags |= FrameFlags::Brs;
    }
    if (parts[6] != "0") {
      flags |= FrameFlags::Esi;
    }
    const auto length =
        static_cast<std::size_t>(std::strtoul(std::string(parts[8]).c_str(), nullptr, 10));

    std::array<std::uint8_t, 64> payload{};
    const std::size_t available = parts.size() > 9 ? parts.size() - 9 : 0;
    const std::size_t take = std::min({length, available, payload.size()});
    for (std::size_t i = 0; i < take; ++i) {
      std::uint64_t byte = 0;
      if (!parse_hex(parts[9 + i], byte)) {
        break;
      }
      payload[i] = static_cast<std::uint8_t>(byte);
    }
    auto frame =
        core::FdFrame::make(id.value(), payload.data(), take, flags, timestamp_ns);
    if (!frame) {
      return frame.error();
    }
    record.frame = frame.value();
    return record;
  }

  std::ifstream in_;
  std::uint32_t line_number_ = 0;
  bool hex_base_ = true;
};

class AscWriter final : public LogWriter {
 public:
  explicit AscWriter(std::ofstream out) : out_(std::move(out)) {
    out_ << "date Wed Jan 1 00:00:00 2025\n"
            "base hex  timestamps absolute\n"
            "internal events logged\n"
            "// canforge\n";
  }

  Status write(const LogRecord& record) override {
    const FdFrame& f = record.frame;
    const std::uint64_t ns = f.timestamp_ns();
    std::ostringstream os;
    os << (ns / 1000000000ULL) << '.' << std::setfill('0') << std::setw(6)
       << ((ns % 1000000000ULL) / 1000ULL) << ' ';

    std::ostringstream id_text;
    id_text << std::hex << std::uppercase << f.id().value();
    if (f.id().is_extended()) {
      id_text << 'x';
    }
    const char* direction = record.is_rx ? "Rx" : "Tx";

    if (f.is_fd()) {
      os << "CANFD " << record.channel << ' ' << direction << ' ' << id_text.str()
         << ' ' << (f.is_brs() ? 1 : 0) << ' ' << (f.is_esi() ? 1 : 0) << ' '
         << unsigned{f.dlc()} << ' ' << f.size() << ' '
         << to_hex(f.data(), f.size(), true);
    } else if (f.is_remote()) {
      os << record.channel << "  " << id_text.str() << "             " << direction
         << "   r " << unsigned{f.dlc()};
    } else {
      os << record.channel << "  " << id_text.str() << "             " << direction
         << "   d " << f.size() << ' ' << to_hex(f.data(), f.size(), true);
    }
    out_ << os.str() << '\n';
    return out_ ? core::ok()
                : Status(Error(ErrorCode::ParseIoError, "write failed"));
  }

  Status finish() override {
    out_ << "End TriggerBlock\n";
    out_.flush();
    return core::ok();
  }

 private:
  std::ofstream out_;
};

}  // namespace

// Defined in BlfLog.cpp.
Result<std::unique_ptr<LogReader>> open_blf_reader(const std::string& path);

Result<LogFormat> detect_format(const std::string& path) {
  const auto ends_with = [&path](std::string_view suffix) {
    if (path.size() < suffix.size()) {
      return false;
    }
    std::string tail = path.substr(path.size() - suffix.size());
    std::transform(tail.begin(), tail.end(), tail.begin(), [](unsigned char c) {
      return static_cast<char>(std::tolower(c));
    });
    return tail == suffix;
  };
  if (ends_with(".blf")) {
    return LogFormat::Blf;
  }
  if (ends_with(".asc")) {
    return LogFormat::Asc;
  }
  if (ends_with(".log") || ends_with(".candump") || ends_with(".txt")) {
    return LogFormat::Candump;
  }

  // Fall back to sniffing the first bytes: BLF starts with LOGG, ASC with a
  // date line, candump with a parenthesised timestamp.
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return Error(ErrorCode::ParseIoError, "cannot open the log file");
  }
  char head[8] = {};
  in.read(head, sizeof(head));
  const std::string_view sniff(head, static_cast<std::size_t>(in.gcount()));
  if (sniff.rfind("LOGG", 0) == 0) {
    return LogFormat::Blf;
  }
  if (sniff.rfind("date ", 0) == 0 || sniff.rfind("base ", 0) == 0) {
    return LogFormat::Asc;
  }
  if (!sniff.empty() && sniff.front() == '(') {
    return LogFormat::Candump;
  }
  return Error(ErrorCode::LogBadFormat,
               "cannot tell what log format this file is");
}

Result<std::unique_ptr<LogReader>> open_reader(const std::string& path) {
  CANFORGE_TRY(const auto format, detect_format(path));
  return open_reader(path, format);
}

Result<std::unique_ptr<LogReader>> open_reader(const std::string& path,
                                               LogFormat format) {
  if (format == LogFormat::Blf) {
    return open_blf_reader(path);
  }
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return Error(ErrorCode::ParseIoError, "cannot open the log file");
  }
  if (format == LogFormat::Asc) {
    return std::unique_ptr<LogReader>(new AscReader(std::move(in)));
  }
  return std::unique_ptr<LogReader>(new CandumpReader(std::move(in)));
}

Result<std::unique_ptr<LogWriter>> open_writer(const std::string& path,
                                               LogFormat format) {
  if (format == LogFormat::Blf) {
    return Error(ErrorCode::TransportUnsupported,
                 "canforge reads BLF but does not write it");
  }
  std::ofstream out(path, std::ios::binary);
  if (!out) {
    return Error(ErrorCode::ParseIoError, "cannot open the log file for writing");
  }
  if (format == LogFormat::Asc) {
    return std::unique_ptr<LogWriter>(new AscWriter(std::move(out)));
  }
  return std::unique_ptr<LogWriter>(new CandumpWriter(std::move(out)));
}

}  // namespace canforge::transport
