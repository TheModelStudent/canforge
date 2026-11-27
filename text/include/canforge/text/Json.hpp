// SPDX-License-Identifier: MIT
#ifndef CANFORGE_TEXT_JSON_HPP
#define CANFORGE_TEXT_JSON_HPP

/// A small, complete JSON reader: strings with escapes, numbers, true, false,
/// null, nested arrays and objects. Enough for the files canforge reads (DID
/// descriptions, firmware package manifests) without a dependency.
///
///   auto doc = canforge::text::parse_json(R"({"version": "1.1.0"})");
///   if (doc && doc.value().member("version") != nullptr) ...
///
/// Objects keep their members in file order; nothing here needs a map. A
/// syntax error comes back as an Error whose detail is the byte offset.

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "canforge/core/Result.hpp"

namespace canforge::text {

struct Json {
  enum class Type : std::uint8_t { Null, Bool, Number, String, Array, Object };
  Type type = Type::Null;
  bool boolean = false;
  double number = 0.0;
  std::string string;
  std::vector<Json> array;
  std::vector<std::pair<std::string, Json>> object;

  const Json* member(std::string_view key) const {
    for (const auto& [name, value] : object) {
      if (name == key) {
        return &value;
      }
    }
    return nullptr;
  }
};

/// Parses one JSON document; anything but whitespace after it is an error.
core::Result<Json> parse_json(std::string_view text);

}  // namespace canforge::text

#endif  // CANFORGE_TEXT_JSON_HPP
