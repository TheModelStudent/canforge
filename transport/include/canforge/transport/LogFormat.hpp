// SPDX-License-Identifier: MIT
#ifndef CANFORGE_TRANSPORT_LOGFORMAT_HPP
#define CANFORGE_TRANSPORT_LOGFORMAT_HPP

/// Reading and writing CAN log files.
///
/// Three formats are supported:
///
///   candump   the can-utils text format, the one every Linux tool speaks
///   ASC       Vector's ASCII trace, read and written
///   BLF       Vector's binary trace, read only
///
/// BLF is where real automotive logs actually arrive, which is why read
/// support is here despite the format being undocumented and compressed. See
/// `BlfLog.cpp` for what had to be reverse-engineered and what is guessed.

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "canforge/core/Frame.hpp"
#include "canforge/core/Result.hpp"

namespace canforge::transport {

using core::FdFrame;
using core::Result;
using core::Status;

enum class LogFormat : std::uint8_t { Candump, Asc, Blf };

/// Guess the format from a file name, then from the leading bytes.
Result<LogFormat> detect_format(const std::string& path);
const char* to_string(LogFormat f) noexcept;

struct LogRecord {
  FdFrame frame;
  std::string channel = "can0";
  /// False for a frame the logging tool itself transmitted. ASC and BLF both
  /// record the direction; candump does not, so it always reads back as Rx.
  bool is_rx = true;
};

/// Sequential reader. `next()` returns `LogEndOfFile` when the log is
/// exhausted, which is an expected outcome, not an error.
class LogReader {
 public:
  LogReader() = default;
  virtual ~LogReader() = default;
  LogReader(const LogReader&) = delete;
  LogReader& operator=(const LogReader&) = delete;

  virtual Result<LogRecord> next() = 0;

  /// Read whatever is left. Convenient for tests and for LogReplayBus, which
  /// needs the whole timeline before it can replay it.
  Result<std::vector<LogRecord>> read_all();
};

class LogWriter {
 public:
  LogWriter() = default;
  virtual ~LogWriter() = default;
  LogWriter(const LogWriter&) = delete;
  LogWriter& operator=(const LogWriter&) = delete;

  virtual Status write(const LogRecord& record) = 0;
  virtual Status finish() = 0;
};

Result<std::unique_ptr<LogReader>> open_reader(const std::string& path);
Result<std::unique_ptr<LogReader>> open_reader(const std::string& path,
                                               LogFormat format);
Result<std::unique_ptr<LogWriter>> open_writer(const std::string& path,
                                               LogFormat format);

/// Parse a single candump line, e.g.
///   (1735689600.123456) vcan0 18FEF100#0102030405060708
/// Exposed because it is genuinely useful on its own, and because it is the
/// unit the candump tests target.
Result<LogRecord> parse_candump_line(std::string_view line);
std::string format_candump_line(const LogRecord& record);

}  // namespace canforge::transport

#endif  // CANFORGE_TRANSPORT_LOGFORMAT_HPP
