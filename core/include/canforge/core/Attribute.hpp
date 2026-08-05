// SPDX-License-Identifier: MIT
#ifndef CANFORGE_CORE_ATTRIBUTE_HPP
#define CANFORGE_CORE_ATTRIBUTE_HPP

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace canforge::core {

/// Which object a BA_DEF_ applies to; absent means the database itself.
enum class AttributeObject : std::uint8_t {
  Database = 0,
  Node,     ///< BU_
  Message,  ///< BO_
  Signal,   ///< SG_
  EnvVar,   ///< EV_
};

enum class AttributeType : std::uint8_t {
  Int = 0,
  Hex,    ///< Same storage as Int; the difference is only how it is written.
  Float,
  String,
  Enum,
};

class AttributeValue {
 public:
  AttributeValue() = default;

  static AttributeValue integer(std::int64_t v, bool hex = false) {
    AttributeValue a;
    a.type_ = hex ? AttributeType::Hex : AttributeType::Int;
    a.number_ = static_cast<double>(v);
    return a;
  }
  static AttributeValue floating(double v) {
    AttributeValue a;
    a.type_ = AttributeType::Float;
    a.number_ = v;
    return a;
  }
  static AttributeValue string(std::string v) {
    AttributeValue a;
    a.type_ = AttributeType::String;
    a.text_ = std::move(v);
    return a;
  }
  /// BA_DEF_DEF_ spells an enum value as a quoted name, BA_ as a zero-based
  /// index. Both are kept so the writer can emit whichever the grammar wants.
  static AttributeValue enumeration(std::int64_t index, std::string name) {
    AttributeValue a;
    a.type_ = AttributeType::Enum;
    a.number_ = static_cast<double>(index);
    a.text_ = std::move(name);
    return a;
  }

  AttributeType type() const noexcept { return type_; }
  double number() const noexcept { return number_; }
  std::int64_t as_int() const noexcept { return static_cast<std::int64_t>(number_); }
  const std::string& text() const noexcept { return text_; }

  friend bool operator==(const AttributeValue& a, const AttributeValue& b) {
    return a.type_ == b.type_ && a.number_ == b.number_ && a.text_ == b.text_;
  }
  friend bool operator!=(const AttributeValue& a, const AttributeValue& b) {
    return !(a == b);
  }

 private:
  AttributeType type_ = AttributeType::Int;
  double number_ = 0.0;
  std::string text_;
};

/// Ordered, so a database written back out is byte-stable.
using AttributeMap = std::map<std::string, AttributeValue>;

struct AttributeDefinition {
  std::string name;
  AttributeObject object = AttributeObject::Database;
  AttributeType type = AttributeType::Int;
  double minimum = 0.0;
  double maximum = 0.0;
  std::vector<std::string> enum_values;  ///< Only for AttributeType::Enum.
  AttributeValue default_value;
  bool has_default = false;

  friend bool operator==(const AttributeDefinition& a,
                         const AttributeDefinition& b) {
    return a.name == b.name && a.object == b.object && a.type == b.type &&
           a.minimum == b.minimum && a.maximum == b.maximum &&
           a.enum_values == b.enum_values && a.has_default == b.has_default &&
           (!a.has_default || a.default_value == b.default_value);
  }
};

const char* to_string(AttributeObject o) noexcept;
const char* to_string(AttributeType t) noexcept;

}  // namespace canforge::core

#endif  // CANFORGE_CORE_ATTRIBUTE_HPP
