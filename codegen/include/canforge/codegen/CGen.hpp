// SPDX-License-Identifier: MIT
#ifndef CANFORGE_CODEGEN_CGEN_HPP
#define CANFORGE_CODEGEN_CGEN_HPP

/// \file
/// DBC to C. Turns a database into `<prefix>_com.h` and `<prefix>_com.c`: one
/// struct plus a pack and an unpack function per message, for firmware that
/// cannot link a DBC parser.
///
/// The output is C11, includes only <stdint.h> and <stdbool.h>, allocates
/// nothing, and has no loops in pack or unpack: every signal becomes one
/// shift-and-mask statement per byte it touches, with all positions computed
/// here. Bit placement comes from SignalLayout::canonical_start(), the same
/// arithmetic the runtime codec uses, so Intel and Motorola share one path and
/// test_cgen.cpp can demand byte-for-byte agreement with Signal::encode.
///
/// What a message needs to be generated:
///   - no multiplexed signals and no SIG_VALTYPE_ float signals
///   - scaled signals of at most 32 bits (unscaled ones may use all 64)
/// A message that does not qualify is left out and named in CGenOutput::skipped.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "canforge/core/Database.hpp"
#include "canforge/core/Result.hpp"
#include "canforge/core/Signal.hpp"

namespace canforge::codegen {

struct CGenOptions {
  /// Names the files (`<prefix>_com.h`) and the tables (`<prefix>_rx_table`).
  /// Message and signal names come from the database, not from this.
  std::string prefix;
  /// Node the rx and tx tables are built for. Empty: no tables.
  std::string node;
  /// Directory generate_c() writes into; created if missing.
  std::string out_dir;
  /// Shown in the "generated from" banner, usually the DBC file name. Keep it
  /// free of machine-specific paths so the output is the same everywhere.
  std::string source_name;
  /// Raw integers only, for 8-bit targets without floating point: every field
  /// holds the signal's raw value in raw_field_type(), scaled or not, and the
  /// limit macros give the raw range. Pack cuts each value to the signal's
  /// bits but does not clamp it to the database's [min|max]: raw values
  /// outside it, such as a "not available" marker, go out as they are.
  bool raw = false;
};

/// The C type a signal gets as a struct field.
enum class CType : std::uint8_t { U8, U16, U32, U64, I8, I16, I32, I64, F32, F64 };

const char* to_string(CType type) noexcept;

/// `BTMS_CoolantTemp` -> `btms_coolant_temp`, `EngineSpeedJ1939` ->
/// `engine_speed_j1939`. The result is always a valid C identifier.
std::string snake_case(std::string_view name);

/// A signal with factor 1, offset 0 and whole-number limits keeps its raw
/// integer, in the narrowest type that holds its bit length. Anything scaled
/// becomes `float`, or `double` when single precision could not resolve one
/// raw step across the signal's range.
CType field_type(const core::SignalLayout& layout) noexcept;

/// The narrowest integer type that holds the signal's raw value: what an
/// unscaled signal gets, and what every signal gets with CGenOptions::raw.
CType raw_field_type(const core::SignalLayout& layout) noexcept;

/// Empty if gen-c can generate `message`, otherwise why not.
std::string unsupported_reason(const core::Message& message);

struct CGenOutput {
  std::string header;
  std::string source;
  /// One line per message that was left out, with the reason.
  std::vector<std::string> skipped;
};

/// Renders both files in memory. Fails on bad options (prefix not an
/// identifier, unknown node) or when two messages would get the same C name.
core::Result<CGenOutput> render_c(const core::Database& db, const CGenOptions& opt);

/// render_c(), then writes `<out_dir>/<prefix>_com.h` and `.c`.
core::Status generate_c(const core::Database& db, const CGenOptions& opt);

}  // namespace canforge::codegen

#endif  // CANFORGE_CODEGEN_CGEN_HPP
