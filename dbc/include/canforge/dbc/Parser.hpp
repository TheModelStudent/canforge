// SPDX-License-Identifier: MIT
#ifndef CANFORGE_DBC_PARSER_HPP
#define CANFORGE_DBC_PARSER_HPP

/// \file
/// Recursive descent parser for the DBC format.
///
/// Error handling has two channels on purpose: Result carries only what a
/// caller can branch on -- a code and a location, with a literal message,
/// because core::Error must stay trivially copyable -- while DiagnosticSink
/// carries the human-readable text and holds many problems, not one.
/// Parsing resynchronises on the next ; or top-level keyword, so a single typo
/// does not hide the other nine.

#include <string>
#include <string_view>

#include "canforge/core/Database.hpp"
#include "canforge/core/Result.hpp"
#include "canforge/dbc/Diagnostic.hpp"

namespace canforge::dbc {

struct ParseResult {
  core::Database database;
  DiagnosticSink diagnostics;
  /// Which real-world tolerances the input actually needed.
  bool had_bom = false;
  bool had_crlf = false;
  bool had_latin1 = false;
  bool had_no_final_newline = false;

  bool ok() const noexcept { return !diagnostics.has_errors(); }
};

/// Parse DBC text. Never fails at the Result level for grammar problems: those
/// land in `diagnostics` with as much of the database recovered as possible.
ParseResult parse_string(std::string_view text,
                         std::string_view filename = "<memory>");

/// Same, reading from disk. Fails only if the file cannot be read.
core::Result<ParseResult> parse_file(const std::string& path);

/// For callers that treat any error as fatal. The Error carries the first
/// code and location only; use parse_file when the message matters.
core::Result<core::Database> load(const std::string& path);

}  // namespace canforge::dbc

#endif  // CANFORGE_DBC_PARSER_HPP
