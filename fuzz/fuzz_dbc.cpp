// SPDX-License-Identifier: MIT
#include <cstddef>
#include <cstdint>
#include <string>

#include "canforge/dbc/Parser.hpp"
#include "canforge/dbc/Writer.hpp"

/// The DBC parser is the largest attack surface in the project: it takes an
/// arbitrary file from a supplier and builds an object graph from it. The
/// target asserts nothing about the result -- a malformed file is expected to
/// produce diagnostics -- only that nothing crashes, reads out of bounds or
/// hangs, and that a database which parsed cleanly survives a rewrite.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  const std::string text(reinterpret_cast<const char*>(data), size);
  auto parsed = canforge::dbc::parse_string(text, "fuzz.dbc");

  // Rendering the diagnostics exercises the caret arithmetic, which indexes
  // into the source line using a column the lexer computed.
  static_cast<void>(parsed.diagnostics.format("fuzz.dbc"));

  if (parsed.ok()) {
    static_cast<void>(parsed.database.validate());
    static_cast<void>(parsed.database.lint());
    // parse -> write -> parse must not crash either; the writer runs on
    // whatever object graph the parser was talked into building.
    const std::string rewritten = canforge::dbc::write_string(parsed.database);
    static_cast<void>(canforge::dbc::parse_string(rewritten, "rewritten.dbc"));
  }
  return 0;
}
