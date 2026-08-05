// SPDX-License-Identifier: MIT
#include "canforge/text/Diagnostic.hpp"

#include <sstream>

namespace canforge::text {
namespace {

const char* severity_text(Severity s) noexcept {
  return s == Severity::Error ? "error" : "warning";
}

std::string gutter(std::uint32_t line, bool with_number) {
  const std::string number = std::to_string(line);
  std::string out;
  if (with_number) {
    out = " " + number + " | ";
  } else {
    out = std::string(number.size() + 2u, ' ') + "| ";
  }
  return out;
}

/// A tab in the source advances the caret by one column in our counting, so
/// the caret line reproduces tabs verbatim to stay aligned in a terminal.
std::string caret_padding(std::string_view line, std::uint32_t column) {
  std::string pad;
  const std::size_t stop = column > 0 ? column - 1u : 0u;
  for (std::size_t i = 0; i < stop; ++i) {
    pad.push_back(i < line.size() && line[i] == '\t' ? '\t' : ' ');
  }
  return pad;
}

}  // namespace

std::string Diagnostic::format(std::string_view filename) const {
  std::ostringstream os;
  os << filename << ':' << where.line << ':' << where.column << ": "
     << severity_text(severity) << ": " << message;
  if (!token.empty()) {
    os << " (found '" << token << "')";
  }
  os << '\n';
  if (!source_line.empty()) {
    os << gutter(where.line, true) << source_line << '\n'
       << gutter(where.line, false) << caret_padding(source_line, where.column)
       << "^\n";
  }
  if (!note.empty()) {
    os << gutter(where.line, false) << "= note: " << note << '\n';
  }
  return os.str();
}

std::string DiagnosticSink::format(std::string_view filename) const {
  std::ostringstream os;
  for (const Diagnostic& d : items_) {
    os << d.format(filename) << '\n';
  }
  if (errors_ != 0 || warnings_ != 0) {
    os << errors_ << " error" << (errors_ == 1 ? "" : "s") << ", " << warnings_
       << " warning" << (warnings_ == 1 ? "" : "s") << '\n';
  }
  return os.str();
}

}  // namespace canforge::text
