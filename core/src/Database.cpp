// SPDX-License-Identifier: MIT
#include "canforge/core/Database.hpp"

#include <algorithm>
#include <array>
#include <sstream>

namespace canforge::core {

const char* to_string(AttributeObject o) noexcept {
  switch (o) {
      // clang-format off
    case AttributeObject::Database: return "database";
    case AttributeObject::Node:     return "node";
    case AttributeObject::Message:  return "message";
    case AttributeObject::Signal:   return "signal";
    case AttributeObject::EnvVar:   return "environment variable";
      // clang-format on
  }
  return "unknown";
}

const char* to_string(AttributeType t) noexcept {
  switch (t) {
      // clang-format off
    case AttributeType::Int:    return "INT";
    case AttributeType::Hex:    return "HEX";
    case AttributeType::Float:  return "FLOAT";
    case AttributeType::String: return "STRING";
    case AttributeType::Enum:   return "ENUM";
      // clang-format on
  }
  return "UNKNOWN";
}

const Message* Database::find_message(CanId id) const noexcept {
  for (const Message& m : messages_) {
    if (m.id() == id) {
      return &m;
    }
  }
  return nullptr;
}

Message* Database::find_message(CanId id) noexcept {
  return const_cast<Message*>(  // NOLINT
      static_cast<const Database*>(this)->find_message(id));
}

const Message* Database::find_message(std::string_view name) const noexcept {
  for (const Message& m : messages_) {
    if (m.name() == name) {
      return &m;
    }
  }
  return nullptr;
}

Message* Database::find_message(std::string_view name) noexcept {
  return const_cast<Message*>(  // NOLINT
      static_cast<const Database*>(this)->find_message(name));
}

const Node* Database::find_node(std::string_view name) const noexcept {
  for (const Node& n : nodes_) {
    if (n.name == name) {
      return &n;
    }
  }
  return nullptr;
}

Node* Database::find_node(std::string_view name) noexcept {
  return const_cast<Node*>(  // NOLINT
      static_cast<const Database*>(this)->find_node(name));
}

const ValueTable* Database::find_value_table(std::string_view name) const noexcept {
  for (const ValueTable& t : value_tables_) {
    if (t.name == name) {
      return &t;
    }
  }
  return nullptr;
}

const AttributeDefinition* Database::find_attribute_definition(
    std::string_view name) const noexcept {
  for (const AttributeDefinition& d : attribute_definitions_) {
    if (d.name == name) {
      return &d;
    }
  }
  return nullptr;
}

Status Database::validate() const noexcept {
  for (std::size_t i = 0; i < messages_.size(); ++i) {
    const Status st = messages_[i].validate();
    if (!st) {
      return st;
    }
    for (std::size_t j = i + 1; j < messages_.size(); ++j) {
      if (messages_[i].id() == messages_[j].id()) {
        return Error(ErrorCode::ParseDuplicateDefinition,
                     "two messages share a CAN identifier",
                     {messages_[i].id().value(), 0});
      }
      if (messages_[i].name() == messages_[j].name()) {
        return Error(ErrorCode::ParseDuplicateDefinition, "two messages share a name");
      }
    }
  }
  for (std::size_t i = 0; i < nodes_.size(); ++i) {
    for (std::size_t j = i + 1; j < nodes_.size(); ++j) {
      if (nodes_[i].name == nodes_[j].name) {
        return Error(ErrorCode::ParseDuplicateDefinition, "two nodes share a name");
      }
    }
  }
  return ok();
}

namespace {

/// Bitmap of the payload bits a signal occupies, in canonical space.
void mark_bits(const SignalLayout& s, std::array<std::uint8_t, 64>& bits) {
  for (std::uint8_t k = 0; k < s.bit_length; ++k) {
    const std::size_t q = s.canonical_start() + k;
    if (q / 8u >= bits.size()) {
      return;
    }
    const std::size_t byte = q / 8u;
    const std::uint8_t bit = s.byte_order == ByteOrder::Intel
                                 ? static_cast<std::uint8_t>(q % 8u)
                                 : static_cast<std::uint8_t>(7u - (q % 8u));
    bits[byte] = static_cast<std::uint8_t>(bits[byte] | (1u << bit));
  }
}

bool overlaps(const SignalLayout& a, const SignalLayout& b) {
  std::array<std::uint8_t, 64> x{};
  std::array<std::uint8_t, 64> y{};
  mark_bits(a, x);
  mark_bits(b, y);
  for (std::size_t i = 0; i < x.size(); ++i) {
    if ((x[i] & y[i]) != 0u) {
      return true;
    }
  }
  return false;
}

}  // namespace

std::vector<std::string> Database::lint() const {
  std::vector<std::string> out;
  for (const Message& m : messages_) {
    if (m.id().violates_base_id_rule()) {
      std::ostringstream os;
      os << "message '" << m.name() << "' uses base identifier 0x" << std::hex
         << m.id().value()
         << ", which ISO 11898-1 reserves (the seven most significant bits "
            "must not all be recessive); hardware accepts it, tools may not";
      out.push_back(os.str());
    }
    if (m.transmitter().empty() || m.transmitter() == kNoNode) {
      out.push_back("message '" + m.name() + "' has no transmitting node");
    }
    // Signals that share bits are only meaningful when multiplexing keeps
    // them apart.
    for (std::size_t i = 0; i < m.signals().size(); ++i) {
      for (std::size_t j = i + 1; j < m.signals().size(); ++j) {
        const Signal& a = m.signals()[i];
        const Signal& b = m.signals()[j];
        const bool mux_separated = a.multiplex_role() == MultiplexRole::Multiplexed &&
                                   b.multiplex_role() == MultiplexRole::Multiplexed &&
                                   !(a.is_present_for(b.multiplex_value()) &&
                                     b.is_present_for(a.multiplex_value()));
        if (!mux_separated && overlaps(a.layout(), b.layout())) {
          out.push_back("signals '" + a.name() + "' and '" + b.name() +
                        "' in message '" + m.name() + "' overlap");
        }
      }
    }
    for (const Signal& s : m.signals()) {
      if (!s.layout().fits(m.dlc())) {
        out.push_back("signal '" + s.name() + "' does not fit in message '" + m.name() +
                      "'");
      }
      if (s.layout().has_range() && s.layout().minimum > s.layout().maximum) {
        out.push_back("signal '" + s.name() + "' declares a minimum above its maximum");
      }
    }
    if (m.is_multiplexed() && m.multiplexor() == nullptr) {
      out.push_back("message '" + m.name() +
                    "' has multiplexed signals but no multiplexor");
    }
  }
  const auto check_attrs = [&](const AttributeMap& map, const std::string& owner) {
    for (const auto& [name, value] : map) {
      static_cast<void>(value);
      if (find_attribute_definition(name) == nullptr) {
        out.push_back("attribute '" + name + "' on " + owner +
                      " has no BA_DEF_ definition");
      }
    }
  };
  check_attrs(attributes_, "the database");
  for (const Node& n : nodes_) {
    check_attrs(n.attributes, "node '" + n.name + "'");
  }
  for (const Message& m : messages_) {
    check_attrs(m.attributes(), "message '" + m.name() + "'");
    for (const Signal& s : m.signals()) {
      check_attrs(s.attributes(), "signal '" + s.name() + "'");
    }
  }
  return out;
}

bool operator==(const Database& a, const Database& b) {
  return a.version_ == b.version_ && a.comment_ == b.comment_ &&
         a.new_symbols_ == b.new_symbols_ && a.bit_timing_ == b.bit_timing_ &&
         a.nodes_ == b.nodes_ && a.messages_ == b.messages_ &&
         a.value_tables_ == b.value_tables_ &&
         a.attribute_definitions_ == b.attribute_definitions_ &&
         a.attributes_ == b.attributes_;
}

}  // namespace canforge::core
