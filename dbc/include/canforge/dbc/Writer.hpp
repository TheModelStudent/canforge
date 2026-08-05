// SPDX-License-Identifier: MIT
#ifndef CANFORGE_DBC_WRITER_HPP
#define CANFORGE_DBC_WRITER_HPP

/// \file
/// Emits a `core::Database` back out as DBC text.
/// The round-trip guarantee is parse -> write -> parse comparing equal, not
/// byte-identical output: a real input file has whitespace, ordering and
/// encoding quirks no writer should reproduce, and a text diff of the input
/// against itself would still pass if the parser dropped a field.

#include <string>

#include "canforge/core/Database.hpp"
#include "canforge/core/Result.hpp"

namespace canforge::dbc {

struct WriteOptions {
  /// Emit the standard NS_ symbol list even for a programmatically built database.
  bool synthesise_new_symbols = true;
  /// Use CRLF line endings, as Vector's tools do on Windows.
  bool crlf = false;
};

std::string write_string(const core::Database& db, const WriteOptions& opt = {});

core::Status write_file(const core::Database& db, const std::string& path,
                        const WriteOptions& opt = {});

/// The shortest decimal string that reads back as exactly `v`. Exposed
/// because the ASC and candump log writers need the same guarantee.
std::string format_number(double v);

}  // namespace canforge::dbc

#endif  // CANFORGE_DBC_WRITER_HPP
