// SPDX-License-Identifier: MIT
#ifndef CANFORGE_CORE_DATABASE_HPP
#define CANFORGE_CORE_DATABASE_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "canforge/core/Attribute.hpp"
#include "canforge/core/Message.hpp"
#include "canforge/core/Result.hpp"

namespace canforge::core {

struct Node {
  std::string name;
  std::string comment;
  AttributeMap attributes;

  friend bool operator==(const Node& a, const Node& b) {
    return a.name == b.name && a.comment == b.comment &&
           a.attributes == b.attributes;
  }
};

struct ValueTable {
  std::string name;
  std::vector<ValueDescription> values;

  friend bool operator==(const ValueTable& a, const ValueTable& b) {
    return a.name == b.name && a.values == b.values;
  }
};

/// Almost always written empty; preserved so a round trip does not drop it.
struct BitTiming {
  std::uint32_t baudrate = 0;
  std::uint32_t btr1 = 0;
  std::uint32_t btr2 = 0;

  friend bool operator==(const BitTiming& a, const BitTiming& b) {
    return a.baudrate == b.baudrate && a.btr1 == b.btr1 && a.btr2 == b.btr2;
  }
};

class Database {
 public:
  const std::string& version() const noexcept { return version_; }
  const std::string& comment() const noexcept { return comment_; }
  const std::vector<std::string>& new_symbols() const noexcept {
    return new_symbols_;
  }
  const std::optional<BitTiming>& bit_timing() const noexcept { return bit_timing_; }
  const std::vector<Node>& nodes() const noexcept { return nodes_; }
  const std::vector<Message>& messages() const noexcept { return messages_; }
  std::vector<Message>& messages() noexcept { return messages_; }
  const std::vector<ValueTable>& value_tables() const noexcept {
    return value_tables_;
  }
  const std::vector<AttributeDefinition>& attribute_definitions() const noexcept {
    return attribute_definitions_;
  }
  std::vector<AttributeDefinition>& attribute_definitions() noexcept {
    return attribute_definitions_;
  }
  const AttributeMap& attributes() const noexcept { return attributes_; }
  AttributeMap& attributes() noexcept { return attributes_; }

  void set_version(std::string v) { version_ = std::move(v); }
  void set_comment(std::string v) { comment_ = std::move(v); }
  void set_new_symbols(std::vector<std::string> v) { new_symbols_ = std::move(v); }
  void set_bit_timing(BitTiming v) { bit_timing_ = v; }
  void add_node(Node n) { nodes_.push_back(std::move(n)); }
  void add_message(Message m) { messages_.push_back(std::move(m)); }
  void add_value_table(ValueTable t) { value_tables_.push_back(std::move(t)); }
  void add_attribute_definition(AttributeDefinition d) {
    attribute_definitions_.push_back(std::move(d));
  }

  const Message* find_message(CanId id) const noexcept;
  Message* find_message(CanId id) noexcept;
  const Message* find_message(std::string_view name) const noexcept;
  Message* find_message(std::string_view name) noexcept;
  const Node* find_node(std::string_view name) const noexcept;
  Node* find_node(std::string_view name) noexcept;
  const ValueTable* find_value_table(std::string_view name) const noexcept;
  const AttributeDefinition* find_attribute_definition(
      std::string_view name) const noexcept;

  template <std::size_t Cap>
  Result<std::vector<DecodedSignal>> decode(const BasicFrame<Cap>& frame) const {
    const Message* m = find_message(frame.id());
    if (m == nullptr) {
      return Error(ErrorCode::CodecUnknownSignal,
                   "no message in this database has that identifier",
                   {frame.id().value(), 0});
    }
    return m->decode(frame);
  }

  Status validate() const noexcept;

  /// Every problem found, not just the first.
  std::vector<std::string> lint() const;

  friend bool operator==(const Database& a, const Database& b);
  friend bool operator!=(const Database& a, const Database& b) { return !(a == b); }

 private:
  std::string version_;
  std::string comment_;
  std::vector<std::string> new_symbols_;
  std::optional<BitTiming> bit_timing_;
  std::vector<Node> nodes_;
  std::vector<Message> messages_;
  std::vector<ValueTable> value_tables_;
  std::vector<AttributeDefinition> attribute_definitions_;
  AttributeMap attributes_;
};

}  // namespace canforge::core

#endif  // CANFORGE_CORE_DATABASE_HPP
