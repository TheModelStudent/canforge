// SPDX-License-Identifier: MIT
#ifndef CANFORGE_UDS_DIDFILE_HPP
#define CANFORGE_UDS_DIDFILE_HPP

/// Data identifier descriptions, read from a JSON file.
///
/// ReadDataByIdentifier returns bytes; what they mean is the ECU's business. A
/// DID file says it once, outside the tool: for each identifier its name, its
/// length and its fields, each with an offset, a size and how to turn the raw
/// number into a physical value. With that, one generic decoder prints any
/// ECU's data, and nothing about a particular ECU is compiled into canforge.
///
///   {
///     "byte_order": "big",
///     "dids": [
///       { "did": "0100", "name": "Temperatures", "length": 4,
///         "fields": [
///           { "name": "coolant", "offset": 0, "size": 2, "type": "int",
///             "factor": 0.1, "offset_value": 0, "unit": "degC", "invalid": 32767 },
///           { "name": "mode", "offset": 2, "size": 1,
///             "values": { "0": "INIT", "1": "NORMAL" } },
///           { "name": "tag", "offset": 3, "size": 1, "type": "hex" } ] } ]
///   }
///
/// Field types: "uint" (the default), "int" (two's complement), "ascii" (text,
/// trailing NULs dropped) and "hex" (bytes as they are). A numeric field is
/// shown as raw * factor + offset_value, or as the matching entry of "values",
/// or as "invalid" when it equals the field's "invalid" marker.
///
/// The JSON reader behind this is small and complete (strings with escapes,
/// numbers, nested arrays and objects); canforge still has no dependencies.

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "canforge/core/Result.hpp"

namespace canforge::uds {

enum class DidFieldType : std::uint8_t { Unsigned, Signed, Ascii, Hex };

struct DidField {
  std::string name;
  std::size_t offset = 0;
  std::size_t size = 0;
  DidFieldType type = DidFieldType::Unsigned;
  double factor = 1.0;
  double offset_value = 0.0;
  std::string unit;
  std::map<std::int64_t, std::string> values;  ///< Raw value -> label.
  bool has_invalid = false;
  std::int64_t invalid = 0;  ///< Raw value that means "not available".
};

struct DidEntry {
  std::uint16_t did = 0;
  std::string name;
  std::size_t length = 0;
  std::vector<DidField> fields;
};

/// One field of one response, ready to print.
struct DecodedField {
  std::string name;
  std::string text;      ///< The value as shown, unit included.
  bool numeric = false;  ///< `value` is meaningful.
  bool valid = true;     ///< False when the raw value was the "invalid" marker.
  double value = 0.0;    ///< The physical value of a numeric field.
  std::int64_t raw = 0;
};

class DidCatalog {
 public:
  /// Parse a DID file. The detail of a returned error is the byte offset in
  /// the text for a JSON syntax error, and the DID concerned for an error in
  /// what the JSON says.
  static core::Result<DidCatalog> parse(std::string_view json);
  static core::Result<DidCatalog> load(const std::string& path);

  const DidEntry* find(std::uint16_t did) const noexcept;
  const std::vector<DidEntry>& entries() const noexcept { return entries_; }
  bool little_endian() const noexcept { return little_endian_; }

  /// Decode the data of one identifier: the bytes after the DID in a
  /// ReadDataByIdentifier response. Fails if there are fewer bytes than the
  /// entry's length.
  core::Result<std::vector<DecodedField>> decode(const DidEntry& entry,
                                                 const std::uint8_t* data,
                                                 std::size_t size) const;

 private:
  std::vector<DidEntry> entries_;
  bool little_endian_ = false;
};

}  // namespace canforge::uds

#endif  // CANFORGE_UDS_DIDFILE_HPP
