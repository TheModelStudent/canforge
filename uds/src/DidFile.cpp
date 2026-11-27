// SPDX-License-Identifier: MIT
#include "canforge/uds/DidFile.hpp"

#include "canforge/text/Json.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <utility>

namespace canforge::uds {

using core::Error;
using core::ErrorCode;
using core::Result;

namespace {

// The JSON reader is canforge/text/Json.hpp, shared with `canforge flash`.
using text::Json;

// ---------------------------------------------------------------------------
// From JSON to a catalog.
// ---------------------------------------------------------------------------

Error semantic(std::string_view message, std::uint32_t did = 0) {
  return Error(ErrorCode::ParseSemantic, message, {did, 0});
}

bool is_whole(double v) {
  return std::isfinite(v) && std::floor(v) == v && std::fabs(v) < 9.0e15;
}

/// A hexadecimal string such as "F190", or a plain number.
bool read_did(const Json& value, std::uint16_t& did) {
  if (value.type == Json::Type::Number) {
    if (!is_whole(value.number) || value.number < 0.0 || value.number > 65535.0) {
      return false;
    }
    did = static_cast<std::uint16_t>(value.number);
    return true;
  }
  if (value.type != Json::Type::String || value.string.empty() ||
      value.string.size() > 4) {
    return false;
  }
  std::uint32_t out = 0;
  for (const char c : value.string) {
    std::uint32_t digit = 0;
    if (c >= '0' && c <= '9') {
      digit = static_cast<std::uint32_t>(c - '0');
    } else if (c >= 'a' && c <= 'f') {
      digit = static_cast<std::uint32_t>(c - 'a') + 10u;
    } else if (c >= 'A' && c <= 'F') {
      digit = static_cast<std::uint32_t>(c - 'A') + 10u;
    } else {
      return false;
    }
    out = (out << 4u) | digit;
  }
  did = static_cast<std::uint16_t>(out);
  return true;
}

bool read_size(const Json* value, std::size_t& out) {
  if (value == nullptr || value->type != Json::Type::Number ||
      !is_whole(value->number) || value->number < 0.0 || value->number > 4095.0) {
    return false;
  }
  out = static_cast<std::size_t>(value->number);
  return true;
}

Result<DidField> read_field(const Json& json, const DidEntry& entry) {
  const std::uint32_t did = entry.did;
  if (json.type != Json::Type::Object) {
    return semantic("a field must be a JSON object", did);
  }
  DidField field;
  const Json* name = json.member("name");
  if (name == nullptr || name->type != Json::Type::String || name->string.empty()) {
    return semantic("a field needs a \"name\"", did);
  }
  field.name = name->string;
  if (!read_size(json.member("offset"), field.offset) ||
      !read_size(json.member("size"), field.size) || field.size == 0) {
    return semantic("a field needs a whole \"offset\" and a \"size\" of at least 1",
                    did);
  }
  if (field.offset + field.size > entry.length) {
    return semantic("a field reaches past the end of its identifier", did);
  }

  if (const Json* type = json.member("type")) {
    if (type->type != Json::Type::String) {
      return semantic("a field \"type\" must be a string", did);
    }
    if (type->string == "uint") {
      field.type = DidFieldType::Unsigned;
    } else if (type->string == "int") {
      field.type = DidFieldType::Signed;
    } else if (type->string == "ascii") {
      field.type = DidFieldType::Ascii;
    } else if (type->string == "hex") {
      field.type = DidFieldType::Hex;
    } else {
      return semantic("a field \"type\" must be uint, int, ascii or hex", did);
    }
  }
  const bool numeric =
      field.type == DidFieldType::Unsigned || field.type == DidFieldType::Signed;
  if (numeric && field.size > 8) {
    return semantic("a numeric field is at most 8 bytes", did);
  }

  if (const Json* factor = json.member("factor")) {
    if (factor->type != Json::Type::Number || factor->number == 0.0) {
      return semantic("a field \"factor\" must be a number other than 0", did);
    }
    field.factor = factor->number;
  }
  if (const Json* add = json.member("offset_value")) {
    if (add->type != Json::Type::Number) {
      return semantic("a field \"offset_value\" must be a number", did);
    }
    field.offset_value = add->number;
  }
  if (const Json* unit = json.member("unit")) {
    if (unit->type != Json::Type::String) {
      return semantic("a field \"unit\" must be a string", did);
    }
    field.unit = unit->string;
  }
  if (const Json* invalid = json.member("invalid")) {
    if (invalid->type != Json::Type::Number || !is_whole(invalid->number)) {
      return semantic("a field \"invalid\" must be a whole number", did);
    }
    field.has_invalid = true;
    field.invalid = static_cast<std::int64_t>(invalid->number);
  }
  if (const Json* values = json.member("values")) {
    if (values->type != Json::Type::Object) {
      return semantic("field \"values\" must be an object of raw value to label", did);
    }
    for (const auto& [key, label] : values->object) {
      char* end = nullptr;
      const long long raw = std::strtoll(key.c_str(), &end, 0);
      if (key.empty() || end != key.c_str() + key.size() ||
          label.type != Json::Type::String) {
        return semantic("field \"values\" must map whole numbers to strings", did);
      }
      field.values[static_cast<std::int64_t>(raw)] = label.string;
    }
  }
  return field;
}

Result<DidEntry> read_entry(const Json& json) {
  if (json.type != Json::Type::Object) {
    return semantic("each entry of \"dids\" must be a JSON object");
  }
  DidEntry entry;
  const Json* did = json.member("did");
  if (did == nullptr || !read_did(*did, entry.did)) {
    return semantic("an entry needs a \"did\": up to four hex digits, in quotes");
  }
  if (const Json* name = json.member("name")) {
    if (name->type != Json::Type::String) {
      return semantic("an entry \"name\" must be a string", entry.did);
    }
    entry.name = name->string;
  }
  if (!read_size(json.member("length"), entry.length) || entry.length == 0) {
    return semantic("an entry needs a whole \"length\" of at least 1", entry.did);
  }
  if (const Json* fields = json.member("fields")) {
    if (fields->type != Json::Type::Array) {
      return semantic("an entry's \"fields\" must be an array", entry.did);
    }
    for (const Json& f : fields->array) {
      CANFORGE_TRY(DidField field, read_field(f, entry));
      entry.fields.push_back(std::move(field));
    }
  }
  return entry;
}

// ---------------------------------------------------------------------------
// Decoding.
// ---------------------------------------------------------------------------

/// Decimals needed to show every step of `factor`: 0.1 -> 1, 0.5 -> 1, 0.01 -> 2.
int decimals_for(double factor) {
  double step = std::fabs(factor);
  int decimals = 0;
  while (decimals < 6 && std::fabs(step - std::round(step)) > 1e-9) {
    step *= 10.0;
    ++decimals;
  }
  return decimals;
}

std::string format_number(double value, int decimals) {
  char buffer[64];
  std::snprintf(buffer, sizeof(buffer), "%.*f", decimals, value);
  return buffer;
}

}  // namespace

Result<DidCatalog> DidCatalog::parse(std::string_view json) {
  CANFORGE_TRY(const Json root, text::parse_json(json));
  if (root.type != Json::Type::Object) {
    return semantic("a DID file is a JSON object with a \"dids\" array");
  }
  DidCatalog catalog;
  if (const Json* order = root.member("byte_order")) {
    if (order->type != Json::Type::String ||
        (order->string != "big" && order->string != "little")) {
      return semantic("\"byte_order\" must be \"big\" or \"little\"");
    }
    catalog.little_endian_ = order->string == "little";
  }
  const Json* dids = root.member("dids");
  if (dids == nullptr || dids->type != Json::Type::Array) {
    return semantic("a DID file is a JSON object with a \"dids\" array");
  }
  for (const Json& item : dids->array) {
    CANFORGE_TRY(DidEntry entry, read_entry(item));
    if (catalog.find(entry.did) != nullptr) {
      return Error(ErrorCode::ParseDuplicateDefinition,
                   "the same DID is described twice", {entry.did, 0});
    }
    catalog.entries_.push_back(std::move(entry));
  }
  return catalog;
}

Result<DidCatalog> DidCatalog::load(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return Error(ErrorCode::ParseIoError, "the DID file cannot be read");
  }
  std::ostringstream text;
  text << in.rdbuf();
  return parse(text.str());
}

const DidEntry* DidCatalog::find(std::uint16_t did) const noexcept {
  for (const DidEntry& entry : entries_) {
    if (entry.did == did) {
      return &entry;
    }
  }
  return nullptr;
}

Result<std::vector<DecodedField>> DidCatalog::decode(const DidEntry& entry,
                                                     const std::uint8_t* data,
                                                     std::size_t size) const {
  if (data == nullptr || size < entry.length) {
    return Error(
        ErrorCode::OutOfRange,
        "the response is shorter than the DID file says this identifier is",
        {static_cast<std::uint32_t>(size), static_cast<std::uint32_t>(entry.length)});
  }
  std::vector<DecodedField> out;
  out.reserve(entry.fields.size());
  for (const DidField& field : entry.fields) {
    DecodedField decoded;
    decoded.name = field.name;
    const std::uint8_t* bytes = data + field.offset;

    if (field.type == DidFieldType::Ascii) {
      std::size_t length = field.size;
      while (length > 0 && bytes[length - 1] == 0) {
        --length;  // NUL padding is not part of the text
      }
      for (std::size_t i = 0; i < length; ++i) {
        const char c = static_cast<char>(bytes[i]);
        decoded.text.push_back(c >= 0x20 && c < 0x7F ? c : '.');
      }
      out.push_back(std::move(decoded));
      continue;
    }
    if (field.type == DidFieldType::Hex) {
      char buffer[4];
      for (std::size_t i = 0; i < field.size; ++i) {
        std::snprintf(buffer, sizeof(buffer), i == 0 ? "%02X" : " %02X",
                      static_cast<unsigned>(bytes[i]));
        decoded.text += buffer;
      }
      out.push_back(std::move(decoded));
      continue;
    }

    std::uint64_t raw = 0;
    for (std::size_t i = 0; i < field.size; ++i) {
      const std::size_t index = little_endian_ ? field.size - 1 - i : i;
      raw = (raw << 8u) | bytes[index];
    }
    std::int64_t value = static_cast<std::int64_t>(raw);
    if (field.type == DidFieldType::Signed && field.size < 8) {
      const std::uint64_t sign = 1ULL << (field.size * 8u - 1u);
      if ((raw & sign) != 0u) {
        value = static_cast<std::int64_t>(raw) - static_cast<std::int64_t>(sign << 1u);
      }
    }
    decoded.numeric = true;
    decoded.raw = value;
    decoded.value = static_cast<double>(value) * field.factor + field.offset_value;

    if (field.has_invalid && value == field.invalid) {
      decoded.valid = false;
      decoded.text = "invalid";
    } else if (const auto label = field.values.find(value);
               label != field.values.end()) {
      decoded.text = label->second;
    } else {
      decoded.text = format_number(decoded.value, decimals_for(field.factor));
      if (field.offset_value != std::floor(field.offset_value)) {
        decoded.text = format_number(
            decoded.value,
            std::max(decimals_for(field.factor), decimals_for(field.offset_value)));
      }
      if (!field.unit.empty()) {
        decoded.text += " " + field.unit;
      }
    }
    out.push_back(std::move(decoded));
  }
  return out;
}

}  // namespace canforge::uds
