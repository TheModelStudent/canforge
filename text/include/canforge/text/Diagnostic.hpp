// SPDX-License-Identifier: MIT
#ifndef CANFORGE_TEXT_DIAGNOSTIC_HPP
#define CANFORGE_TEXT_DIAGNOSTIC_HPP

/// Compiler-style diagnostics for the DBC parser: file, line, column, what was
/// expected, what was found, and the offending line with a caret under it.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "canforge/core/Result.hpp"

namespace canforge::text {

struct SourceLocation {
  std::uint32_t line = 1;    ///< 1-based
  std::uint32_t column = 1;  ///< 1-based, counted in bytes
};

enum class Severity : std::uint8_t { Warning, Error };

/// `note` carries the 'which tool does this' explanation for tolerated deviations.
struct Diagnostic {
  Severity severity = Severity::Error;
  core::ErrorCode code = core::ErrorCode::ParseUnexpectedToken;
  SourceLocation where;
  std::string message;
  std::string token;        ///< The offending token text, if there was one.
  std::string source_line;  ///< The full line, for the caret rendering.
  std::string note;

  /// Renders as
  ///
  ///   powertrain.dbc:12:24: error: expected '|' after the start bit
  ///      12 |  SG_ EngineSpeed : 24@1+ (0.125,0) [0|8031] "rpm" Vector__XXX
  ///         |                        ^
  ///         = note: ...
  std::string format(std::string_view filename) const;
};

class DiagnosticSink {
 public:
  void add(Diagnostic d) {
    if (d.severity == Severity::Error) {
      ++errors_;
    } else {
      ++warnings_;
    }
    items_.push_back(std::move(d));
  }

  const std::vector<Diagnostic>& items() const noexcept { return items_; }
  std::size_t error_count() const noexcept { return errors_; }
  std::size_t warning_count() const noexcept { return warnings_; }
  bool has_errors() const noexcept { return errors_ != 0; }

  /// All diagnostics, one per paragraph, in source order.
  std::string format(std::string_view filename) const;

 private:
  std::vector<Diagnostic> items_;
  std::size_t errors_ = 0;
  std::size_t warnings_ = 0;
};

}  // namespace canforge::text

#endif  // CANFORGE_TEXT_DIAGNOSTIC_HPP
