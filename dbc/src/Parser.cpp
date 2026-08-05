// SPDX-License-Identifier: MIT
#include "canforge/dbc/Parser.hpp"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <utility>

#include "canforge/dbc/Lexer.hpp"

namespace canforge::dbc {
namespace {

using core::AttributeDefinition;
using core::AttributeMap;
using core::AttributeObject;
using core::AttributeType;
using core::AttributeValue;
using core::ByteOrder;
using core::CanId;
using core::Database;
using core::ErrorCode;
using core::Message;
using core::MultiplexRole;
using core::MuxRange;
using core::Node;
using core::Signal;
using core::SignalLayout;
using core::Signedness;
using core::ValueDescription;
using core::ValueTable;
using core::ValueType;

/// DBC encodes an extended identifier by setting bit 31 of the message id.
/// This is not in any published grammar; it is simply what Vector's tools do,
/// and every database with 29-bit identifiers relies on it.
constexpr std::uint32_t kDbcExtendedFlag = 0x80000000u;

class Parser {
 public:
  Parser(const Source& source, std::vector<Token> tokens, DiagnosticSink& sink)
      : source_(&source), tokens_(std::move(tokens)), sink_(&sink) {}

  Database run();

 private:
  const Token& peek(std::size_t ahead = 0) const {
    const std::size_t i = std::min(pos_ + ahead, tokens_.size() - 1);
    return tokens_[i];
  }
  const Token& current() const { return peek(0); }
  const Token& advance() {
    const Token& t = tokens_[std::min(pos_, tokens_.size() - 1)];
    if (pos_ + 1 < tokens_.size()) {
      ++pos_;
    }
    return t;
  }
  bool at_end() const { return current().kind == TokenKind::End; }
  bool match(TokenKind k) {
    if (current().kind == k) {
      advance();
      return true;
    }
    return false;
  }
  bool match_word(std::string_view w) {
    if (current().is_word(w)) {
      advance();
      return true;
    }
    return false;
  }

  void report(Severity sev, ErrorCode code, const Token& t, std::string message,
              std::string note = {}) {
    Diagnostic d;
    d.severity = sev;
    d.code = code;
    d.where = t.where;
    d.message = std::move(message);
    d.token = t.kind == TokenKind::End ? "<end of file>" : std::string(t.text);
    d.source_line = std::string(source_->line(t.where.line));
    d.note = std::move(note);
    sink_->add(std::move(d));
  }
  void error(ErrorCode code, const Token& t, std::string message,
             std::string note = {}) {
    report(Severity::Error, code, t, std::move(message), std::move(note));
  }
  void warn(ErrorCode code, const Token& t, std::string message,
            std::string note = {}) {
    report(Severity::Warning, code, t, std::move(message), std::move(note));
  }

  bool expect(TokenKind k, std::string_view what) {
    if (current().kind == k) {
      advance();
      return true;
    }
    std::ostringstream os;
    os << "expected " << to_string(k) << ' ' << what << ", found "
       << to_string(current().kind);
    error(ErrorCode::ParseUnexpectedToken, current(), os.str());
    return false;
  }

  /// Error recovery: skip to just past the next `;`, or to the next token that
  /// begins a line and looks like a top-level keyword.
  void resynchronise() {
    while (!at_end()) {
      if (current().kind == TokenKind::Semicolon) {
        advance();
        return;
      }
      if (current().first_on_line && current().kind == TokenKind::Identifier &&
          is_top_level_keyword(current().text)) {
        return;
      }
      advance();
    }
  }

  static bool is_top_level_keyword(std::string_view w) {
    static const std::string_view kWords[] = {
        "VERSION",    "NS_",         "BS_",        "BU_",       "BO_",
        "CM_",        "BA_DEF_",     "BA_DEF_DEF_", "BA_",      "VAL_",
        "VAL_TABLE_", "SIG_VALTYPE_", "SG_MUL_VAL_", "BO_TX_BU_", "EV_",
        "BA_DEF_REL_", "BA_REL_",    "BA_DEF_DEF_REL_", "SIG_GROUP_",
        "SGTYPE_",    "ENVVAR_DATA_", "BU_SG_REL_", "BU_BO_REL_",
        "BU_EV_REL_", "CAT_DEF_",    "CAT_",       "FILTER",    "SIG_TYPE_REF_"};
    for (const std::string_view k : kWords) {
      if (w == k) {
        return true;
      }
    }
    return false;
  }

  /// Optional leading sign followed by a number. DBC keeps the sign as a
  /// separate token because `-` is also the signedness marker in `@1-`.
  bool parse_number(double& out) {
    double sign = 1.0;
    if (match(TokenKind::Minus)) {
      sign = -1.0;
    } else {
      match(TokenKind::Plus);
    }
    if (current().kind != TokenKind::Integer && current().kind != TokenKind::Real) {
      error(ErrorCode::ParseBadNumber, current(), "expected a number");
      return false;
    }
    out = sign * advance().real;
    return true;
  }

  bool parse_unsigned(std::uint64_t& out) {
    if (current().kind != TokenKind::Integer) {
      error(ErrorCode::ParseBadNumber, current(),
            "expected a non-negative integer");
      return false;
    }
    out = advance().integer;
    return true;
  }

  bool parse_string(std::string& out) {
    if (current().kind != TokenKind::String) {
      error(ErrorCode::ParseUnexpectedToken, current(),
            "expected a quoted string");
      return false;
    }
    out = advance().value;
    return true;
  }

  bool parse_name(std::string& out) {
    if (current().kind != TokenKind::Identifier) {
      error(ErrorCode::ParseUnexpectedToken, current(), "expected a name");
      return false;
    }
    out = std::string(advance().text);
    return true;
  }

  /// A comma-separated list of names, tolerating a trailing comma and the
  /// `Vector__XXX` placeholder, and accepting an empty list.
  std::vector<std::string> parse_name_list() {
    std::vector<std::string> names;
    while (current().kind == TokenKind::Identifier) {
      const std::string name(advance().text);
      if (name != core::kNoNode) {
        names.push_back(name);
      }
      if (!match(TokenKind::Comma)) {
        break;
      }
    }
    return names;
  }

  CanId parse_message_id(std::uint64_t raw, const Token& where) {
    const auto packed = static_cast<std::uint32_t>(raw);
    const bool extended = (packed & kDbcExtendedFlag) != 0u;
    const std::uint32_t id = packed & ~kDbcExtendedFlag;
    auto built = extended ? CanId::extended(id) : CanId::standard(id);
    if (!built) {
      // A 29-bit identifier written without the flag is a common export bug;
      // accept it as extended rather than rejecting the whole file.
      if (!extended && id <= core::kExtendedIdMax) {
        warn(ErrorCode::ParseSemantic, where,
             "identifier does not fit in 11 bits and the extended flag (bit 31) "
             "is not set; treating it as a 29-bit identifier",
             "some exporters omit the 0x80000000 flag that Vector tools set");
        return CanId::extended(id).value();
      }
      error(ErrorCode::FrameBadIdentifier, where,
            "message identifier is out of range for both frame formats");
      return CanId{};
    }
    return built.value();
  }

  void parse_version();
  void parse_new_symbols();
  void parse_bit_timing();
  void parse_nodes();
  void parse_value_table();
  void parse_message();
  void parse_signal(Message& message);
  void parse_message_transmitters();
  void parse_comment();
  void parse_attribute_definition(bool relation);
  void parse_attribute_default(bool relation);
  void parse_attribute_value(bool relation);
  void parse_value_descriptions();
  void parse_signal_value_type();
  void parse_extended_multiplexing();
  void skip_unsupported_section(std::string_view keyword, std::string_view why);

  AttributeValue parse_attribute_literal(const AttributeDefinition* def);
  void assign_attribute(const std::string& name, AttributeValue value,
                        AttributeObject object, const Token& where);

  const Source* source_;
  std::vector<Token> tokens_;
  DiagnosticSink* sink_;
  std::size_t pos_ = 0;
  Database db_;
  // Pending assignment target, filled by parse_attribute_value.
  Message* attr_message_ = nullptr;
  Signal* attr_signal_ = nullptr;
  Node* attr_node_ = nullptr;
};

Database Parser::run() {
  while (!at_end()) {
    const Token& t = current();
    if (t.kind != TokenKind::Identifier) {
      error(ErrorCode::ParseUnexpectedToken, t,
            "expected a section keyword at the start of a statement");
      resynchronise();
      continue;
    }

    if (t.is_word("VERSION")) {
      parse_version();
    } else if (t.is_word("NS_")) {
      parse_new_symbols();
    } else if (t.is_word("BS_")) {
      parse_bit_timing();
    } else if (t.is_word("BU_")) {
      parse_nodes();
    } else if (t.is_word("VAL_TABLE_")) {
      parse_value_table();
    } else if (t.is_word("BO_")) {
      parse_message();
    } else if (t.is_word("BO_TX_BU_")) {
      parse_message_transmitters();
    } else if (t.is_word("CM_")) {
      parse_comment();
    } else if (t.is_word("BA_DEF_")) {
      parse_attribute_definition(false);
    } else if (t.is_word("BA_DEF_REL_")) {
      parse_attribute_definition(true);
    } else if (t.is_word("BA_DEF_DEF_")) {
      parse_attribute_default(false);
    } else if (t.is_word("BA_DEF_DEF_REL_")) {
      parse_attribute_default(true);
    } else if (t.is_word("BA_")) {
      parse_attribute_value(false);
    } else if (t.is_word("BA_REL_")) {
      parse_attribute_value(true);
    } else if (t.is_word("VAL_")) {
      parse_value_descriptions();
    } else if (t.is_word("SIG_VALTYPE_")) {
      parse_signal_value_type();
    } else if (t.is_word("SG_MUL_VAL_")) {
      parse_extended_multiplexing();
    } else if (t.is_word("EV_")) {
      skip_unsupported_section("EV_", "environment variables are preserved by "
                                      "neither canforge nor most CAN stacks");
    } else if (t.is_word("SIG_GROUP_") || t.is_word("SGTYPE_") ||
               t.is_word("ENVVAR_DATA_") || t.is_word("BU_SG_REL_") ||
               t.is_word("BU_BO_REL_") || t.is_word("BU_EV_REL_") ||
               t.is_word("CAT_DEF_") || t.is_word("CAT_") ||
               t.is_word("FILTER") || t.is_word("SIG_TYPE_REF_")) {
      skip_unsupported_section(t.text, "not required for encoding or decoding");
    } else if (t.is_word("SG_")) {
      error(ErrorCode::ParseSemantic, t,
            "a signal definition must follow a BO_ message definition");
      resynchronise();
    } else {
      error(ErrorCode::ParseUnknownKeyword, t, "unknown section keyword");
      resynchronise();
    }
  }
  return std::move(db_);
}

void Parser::parse_version() {
  advance();  // VERSION
  std::string v;
  if (parse_string(v)) {
    db_.set_version(std::move(v));
  } else {
    resynchronise();
  }
}

void Parser::parse_new_symbols() {
  advance();  // NS_
  match(TokenKind::Colon);
  // The entries of NS_ are spelled exactly like the section keywords they
  // announce, so they cannot be told apart by name. What does distinguish them
  // is that an NS_ entry is a bare identifier on its own line, whereas a real
  // section is always followed by punctuation or an operand. The list is
  // terminated by BS_, which is mandatory in every file that has an NS_.
  std::vector<std::string> symbols;
  while (current().kind == TokenKind::Identifier && !current().is_word("BS_")) {
    const Token& next = peek(1);
    const bool entry_stands_alone =
        next.first_on_line || next.kind == TokenKind::End;
    if (!entry_stands_alone) {
      break;
    }
    symbols.push_back(std::string(advance().text));
  }
  db_.set_new_symbols(std::move(symbols));
}

void Parser::parse_bit_timing() {
  advance();  // BS_
  expect(TokenKind::Colon, "after BS_");
  // Written empty in practically every file. When present the form is
  // `BS_: <baudrate> : <btr1>, <btr2>`.
  if (current().kind == TokenKind::Integer) {
    core::BitTiming bt;
    std::uint64_t v = 0;
    if (parse_unsigned(v)) {
      bt.baudrate = static_cast<std::uint32_t>(v);
    }
    if (match(TokenKind::Colon)) {
      if (parse_unsigned(v)) {
        bt.btr1 = static_cast<std::uint32_t>(v);
      }
      expect(TokenKind::Comma, "between the two bit timing registers");
      if (parse_unsigned(v)) {
        bt.btr2 = static_cast<std::uint32_t>(v);
      }
    }
    db_.set_bit_timing(bt);
  }
}

void Parser::parse_nodes() {
  advance();  // BU_
  expect(TokenKind::Colon, "after BU_");
  // The node list runs to the end of the line. Nodes are whitespace separated,
  // not comma separated, but files exported from spreadsheets use commas, so
  // both are accepted.
  while (current().kind == TokenKind::Identifier && !current().first_on_line) {
    Node n;
    n.name = std::string(advance().text);
    if (n.name != core::kNoNode && db_.find_node(n.name) == nullptr) {
      db_.add_node(std::move(n));
    }
    match(TokenKind::Comma);
  }
}

void Parser::parse_value_table() {
  advance();  // VAL_TABLE_
  ValueTable table;
  if (!parse_name(table.name)) {
    resynchronise();
    return;
  }
  while (current().kind == TokenKind::Integer ||
         current().kind == TokenKind::Minus) {
    double v = 0.0;
    if (!parse_number(v)) {
      break;
    }
    ValueDescription vd;
    vd.value = static_cast<std::int64_t>(v);
    if (!parse_string(vd.text)) {
      break;
    }
    table.values.push_back(std::move(vd));
  }
  match(TokenKind::Semicolon);
  db_.add_value_table(std::move(table));
}

void Parser::parse_message() {
  const Token& keyword = advance();  // BO_
  std::uint64_t raw_id = 0;
  const Token& id_token = current();
  if (!parse_unsigned(raw_id)) {
    resynchronise();
    return;
  }
  Message message;
  message.set_id(parse_message_id(raw_id, id_token));

  std::string name;
  if (!parse_name(name)) {
    resynchronise();
    return;
  }
  message.set_name(std::move(name));
  expect(TokenKind::Colon, "after the message name");

  std::uint64_t dlc = 0;
  const Token& dlc_token = current();
  if (!parse_unsigned(dlc)) {
    resynchronise();
    return;
  }
  if (dlc > 64u) {
    error(ErrorCode::FrameBadDlc, dlc_token,
          "message length exceeds the 64 bytes a CAN FD frame can carry");
    dlc = 64;
  }
  message.set_dlc(static_cast<std::uint8_t>(dlc));
  // A message longer than eight bytes can only be CAN FD. The BA_ attribute
  // `VFrameFormat` says so explicitly when present; the length alone is a
  // reliable fallback and is applied here so that validate() does not reject
  // a perfectly good FD database that omits the attribute.
  message.set_fd(dlc > 8u);

  std::string transmitter;
  if (current().kind == TokenKind::Identifier) {
    transmitter = std::string(advance().text);
    if (transmitter == core::kNoNode) {
      transmitter.clear();
    }
  } else {
    warn(ErrorCode::ParseSemantic, current(),
         "message has no transmitting node",
         "the grammar requires one; Vector writes Vector__XXX when there is none");
  }
  message.set_transmitter(std::move(transmitter));
  static_cast<void>(keyword);

  while (current().is_word("SG_")) {
    parse_signal(message);
  }
  db_.add_message(std::move(message));
}

void Parser::parse_signal(Message& message) {
  advance();  // SG_
  Signal signal;
  std::string name;
  if (!parse_name(name)) {
    resynchronise();
    return;
  }
  signal.set_name(name);

  // Multiplexer indicator: M, m<n>, or m<n>M for a signal that is both
  // multiplexed and itself a multiplexor (extended multiplexing).
  if (current().kind == TokenKind::Identifier && !current().is_word("SG_")) {
    const std::string_view ind = current().text;
    if (ind == "M") {
      signal.set_multiplex_role(MultiplexRole::Multiplexor);
      advance();
    } else if (ind.size() >= 2 && ind[0] == 'm') {
      std::size_t digits = 1;
      while (digits < ind.size() && ind[digits] >= '0' && ind[digits] <= '9') {
        ++digits;
      }
      const bool also_multiplexor = (digits < ind.size() && ind[digits] == 'M');
      if (digits > 1 && (digits == ind.size() || also_multiplexor)) {
        signal.set_multiplex_role(MultiplexRole::Multiplexed);
        signal.set_multiplex_value(static_cast<std::uint32_t>(
            std::strtoul(std::string(ind.substr(1, digits - 1)).c_str(), nullptr, 10)));
        if (also_multiplexor) {
          // `m<n>M`: switched in by an outer multiplexor and itself switching
          // an inner group.
          signal.set_multiplex_role(MultiplexRole::Both);
        }
        advance();
      }
    }
  }

  expect(TokenKind::Colon, "after the signal name");

  SignalLayout layout;
  std::uint64_t start = 0;
  const Token& start_token = current();
  if (!parse_unsigned(start)) {
    resynchronise();
    return;
  }
  if (start > 511u) {
    error(ErrorCode::CodecSignalOutOfBounds, start_token,
          "start bit is beyond the largest possible payload");
    start = 0;
  }
  layout.start_bit = static_cast<std::uint16_t>(start);
  expect(TokenKind::Pipe, "between the start bit and the bit length");

  std::uint64_t length = 0;
  const Token& len_token = current();
  if (!parse_unsigned(length)) {
    resynchronise();
    return;
  }
  if (length < 1u || length > 64u) {
    error(ErrorCode::CodecBadBitLength, len_token,
          "signal length must be between 1 and 64 bits");
    length = std::clamp<std::uint64_t>(length, 1u, 64u);
  }
  layout.bit_length = static_cast<std::uint8_t>(length);

  expect(TokenKind::At, "before the byte order");
  const Token& order_token = current();
  std::uint64_t order = 1;
  if (!parse_unsigned(order)) {
    resynchronise();
    return;
  }
  if (order > 1u) {
    error(ErrorCode::ParseSemantic, order_token,
          "byte order must be 0 (Motorola) or 1 (Intel)");
    order = 1;
  }
  layout.byte_order = order == 1u ? ByteOrder::Intel : ByteOrder::Motorola;

  if (match(TokenKind::Plus)) {
    layout.signedness = Signedness::Unsigned;
  } else if (match(TokenKind::Minus)) {
    layout.signedness = Signedness::Signed;
  } else {
    error(ErrorCode::ParseUnexpectedToken, current(),
          "expected '+' or '-' for the signal's signedness");
  }

  expect(TokenKind::LParen, "before the factor and offset");
  if (!parse_number(layout.factor)) {
    resynchronise();
    return;
  }
  expect(TokenKind::Comma, "between the factor and the offset");
  if (!parse_number(layout.offset)) {
    resynchronise();
    return;
  }
  expect(TokenKind::RParen, "after the factor and offset");

  expect(TokenKind::LBracket, "before the minimum and maximum");
  if (!parse_number(layout.minimum)) {
    resynchronise();
    return;
  }
  expect(TokenKind::Pipe, "between the minimum and the maximum");
  if (!parse_number(layout.maximum)) {
    resynchronise();
    return;
  }
  expect(TokenKind::RBracket, "after the minimum and maximum");

  std::string unit;
  if (!parse_string(unit)) {
    resynchronise();
    return;
  }
  signal.set_unit(std::move(unit));
  signal.set_layout(layout);
  signal.set_receivers(parse_name_list());

  if (layout.factor == 0.0) {
    warn(ErrorCode::CodecBadFactor, start_token,
         "signal '" + name + "' has a scale factor of zero, which makes it "
         "impossible to encode; substituting 1",
         "occasionally emitted by converters for constant signals");
    SignalLayout fixed = layout;
    fixed.factor = 1.0;
    signal.set_layout(fixed);
  }

  // The signal must fit the message it was declared in. This is a semantic
  // check rather than a grammatical one, but doing it here rather than in
  // Database::validate() is what lets the diagnostic carry a line and column.
  if (!signal.layout().fits(message.dlc())) {
    error(ErrorCode::CodecSignalOutOfBounds, start_token,
          "signal '" + name + "' extends past the end of message '" +
              message.name() + "', which holds " +
              std::to_string(message.dlc()) + " bytes");
  }

  message.add_signal(std::move(signal));
}

void Parser::parse_message_transmitters() {
  advance();  // BO_TX_BU_
  std::uint64_t raw_id = 0;
  const Token& id_token = current();
  if (!parse_unsigned(raw_id)) {
    resynchronise();
    return;
  }
  expect(TokenKind::Colon, "after the message identifier");
  const CanId id = parse_message_id(raw_id, id_token);
  std::vector<std::string> transmitters = parse_name_list();
  match(TokenKind::Semicolon);
  if (Message* m = db_.find_message(id)) {
    m->set_additional_transmitters(std::move(transmitters));
  } else {
    warn(ErrorCode::ParseUndefinedReference, id_token,
         "BO_TX_BU_ refers to a message that is not defined");
  }
}

void Parser::parse_comment() {
  advance();  // CM_
  const Token& kind = current();

  auto finish = [this](std::string& target) {
    std::string text;
    if (parse_string(text)) {
      target = std::move(text);
    }
    // The trailing semicolon is mandatory in the grammar but is missing from
    // files produced by several converters, so it is optional here.
    match(TokenKind::Semicolon);
  };

  if (kind.is_word("BU_")) {
    advance();
    std::string node;
    if (!parse_name(node)) {
      resynchronise();
      return;
    }
    if (Node* n = db_.find_node(node)) {
      finish(n->comment);
    } else {
      warn(ErrorCode::ParseUndefinedReference, kind,
           "comment refers to node '" + node + "', which is not declared in BU_");
      std::string ignored;
      finish(ignored);
    }
    return;
  }
  if (kind.is_word("BO_")) {
    advance();
    std::uint64_t raw_id = 0;
    const Token& id_token = current();
    if (!parse_unsigned(raw_id)) {
      resynchronise();
      return;
    }
    Message* m = db_.find_message(parse_message_id(raw_id, id_token));
    if (m != nullptr) {
      finish(m->comment_ref());
    } else {
      warn(ErrorCode::ParseUndefinedReference, id_token,
           "comment refers to a message that is not defined");
      std::string ignored;
      finish(ignored);
    }
    return;
  }
  if (kind.is_word("SG_")) {
    advance();
    std::uint64_t raw_id = 0;
    const Token& id_token = current();
    if (!parse_unsigned(raw_id)) {
      resynchronise();
      return;
    }
    std::string signal_name;
    if (!parse_name(signal_name)) {
      resynchronise();
      return;
    }
    Message* m = db_.find_message(parse_message_id(raw_id, id_token));
    Signal* s = m != nullptr ? m->find_signal(signal_name) : nullptr;
    if (s != nullptr) {
      std::string text;
      if (parse_string(text)) {
        s->set_comment(std::move(text));
      }
      match(TokenKind::Semicolon);
    } else {
      warn(ErrorCode::ParseUndefinedReference, id_token,
           "comment refers to signal '" + signal_name + "', which is not defined");
      std::string ignored;
      finish(ignored);
    }
    return;
  }
  if (kind.is_word("EV_")) {
    advance();
    std::string ignored_name;
    parse_name(ignored_name);
    std::string ignored;
    finish(ignored);
    return;
  }
  // No object keyword: the comment belongs to the database itself.
  std::string text;
  if (parse_string(text)) {
    db_.set_comment(std::move(text));
  }
  match(TokenKind::Semicolon);
}

void Parser::skip_unsupported_section(std::string_view keyword,
                                      std::string_view why) {
  const Token& t = current();
  warn(ErrorCode::Unsupported, t,
       "skipping unsupported section " + std::string(keyword),
       std::string(why) +
           "; the statement is discarded, so it will not survive a rewrite");
  advance();
  resynchronise();
}

void Parser::parse_attribute_definition(bool relation) {
  const Token& keyword = advance();  // BA_DEF_ / BA_DEF_REL_
  AttributeDefinition def;

  // The object keyword is optional; when absent the attribute applies to the
  // database. Note that some writers put two spaces after BA_DEF_ and before
  // the object keyword, which the lexer already absorbs.
  if (current().kind == TokenKind::Identifier) {
    const std::string_view w = current().text;
    if (w == "BU_") {
      def.object = AttributeObject::Node;
      advance();
    } else if (w == "BO_") {
      def.object = AttributeObject::Message;
      advance();
    } else if (w == "SG_") {
      def.object = AttributeObject::Signal;
      advance();
    } else if (w == "EV_") {
      def.object = AttributeObject::EnvVar;
      advance();
    } else if (relation) {
      // BU_SG_REL_ and friends. Recorded as a node attribute so the name is
      // still resolvable; canforge does not model relations.
      def.object = AttributeObject::Node;
      advance();
    }
  }

  if (!parse_string(def.name)) {
    resynchronise();
    return;
  }

  if (current().kind != TokenKind::Identifier) {
    error(ErrorCode::ParseUnexpectedToken, current(),
          "expected an attribute type: INT, HEX, FLOAT, STRING or ENUM");
    resynchronise();
    return;
  }
  const std::string type_name(advance().text);
  if (type_name == "INT" || type_name == "HEX") {
    def.type = type_name == "INT" ? AttributeType::Int : AttributeType::Hex;
    if (!parse_number(def.minimum) || !parse_number(def.maximum)) {
      resynchronise();
      return;
    }
  } else if (type_name == "FLOAT") {
    def.type = AttributeType::Float;
    if (!parse_number(def.minimum) || !parse_number(def.maximum)) {
      resynchronise();
      return;
    }
  } else if (type_name == "STRING") {
    def.type = AttributeType::String;
  } else if (type_name == "ENUM") {
    def.type = AttributeType::Enum;
    while (current().kind == TokenKind::String) {
      def.enum_values.push_back(advance().value);
      if (!match(TokenKind::Comma)) {
        break;
      }
    }
  } else {
    error(ErrorCode::ParseSemantic, keyword,
          "unknown attribute type '" + type_name + "'");
    resynchronise();
    return;
  }
  match(TokenKind::Semicolon);

  if (db_.find_attribute_definition(def.name) != nullptr) {
    warn(ErrorCode::ParseDuplicateDefinition, keyword,
         "attribute '" + def.name + "' is defined more than once; the first "
         "definition is kept");
    return;
  }
  db_.add_attribute_definition(std::move(def));
}

AttributeValue Parser::parse_attribute_literal(const AttributeDefinition* def) {
  if (current().kind == TokenKind::String) {
    const std::string text = advance().value;
    if (def != nullptr && def->type == AttributeType::Enum) {
      const auto it =
          std::find(def->enum_values.begin(), def->enum_values.end(), text);
      const auto index = it == def->enum_values.end()
                             ? std::int64_t{-1}
                             : static_cast<std::int64_t>(
                                   std::distance(def->enum_values.begin(), it));
      return AttributeValue::enumeration(index, text);
    }
    return AttributeValue::string(text);
  }
  double v = 0.0;
  if (!parse_number(v)) {
    return {};
  }
  if (def == nullptr) {
    return AttributeValue::floating(v);
  }
  switch (def->type) {
    case AttributeType::Int:
      return AttributeValue::integer(static_cast<std::int64_t>(v), false);
    case AttributeType::Hex:
      return AttributeValue::integer(static_cast<std::int64_t>(v), true);
    case AttributeType::Float:
      return AttributeValue::floating(v);
    case AttributeType::Enum: {
      // In BA_ an enum value is written as its zero-based index.
      const auto index = static_cast<std::int64_t>(v);
      const std::string name =
          index >= 0 && static_cast<std::size_t>(index) < def->enum_values.size()
              ? def->enum_values[static_cast<std::size_t>(index)]
              : std::string{};
      return AttributeValue::enumeration(index, name);
    }
    case AttributeType::String:
    default:
      return AttributeValue::string(std::to_string(v));
  }
}

void Parser::parse_attribute_default(bool relation) {
  advance();  // BA_DEF_DEF_ / BA_DEF_DEF_REL_
  static_cast<void>(relation);
  std::string name;
  if (!parse_string(name)) {
    resynchronise();
    return;
  }
  const AttributeDefinition* def = db_.find_attribute_definition(name);
  if (def == nullptr) {
    warn(ErrorCode::ParseUndefinedReference, current(),
         "default given for attribute '" + name + "', which has no BA_DEF_");
  }
  AttributeValue value = parse_attribute_literal(def);
  match(TokenKind::Semicolon);
  for (AttributeDefinition& d : db_.attribute_definitions()) {
    if (d.name == name) {
      d.default_value = std::move(value);
      d.has_default = true;
      break;
    }
  }
}

void Parser::assign_attribute(const std::string& name, AttributeValue value,
                              AttributeObject object, const Token& where) {
  switch (object) {
    case AttributeObject::Database:
      db_.attributes()[name] = std::move(value);
      return;
    case AttributeObject::Node:
      if (attr_node_ != nullptr) {
        attr_node_->attributes[name] = std::move(value);
        return;
      }
      break;
    case AttributeObject::Message:
      if (attr_message_ != nullptr) {
        attr_message_->attributes()[name] = std::move(value);
        return;
      }
      break;
    case AttributeObject::Signal:
      if (attr_signal_ != nullptr) {
        attr_signal_->attributes()[name] = std::move(value);
        return;
      }
      break;
    case AttributeObject::EnvVar:
      return;  // environment variables are not modelled
  }
  warn(ErrorCode::ParseUndefinedReference, where,
       "attribute '" + name + "' is assigned to a " +
           core::to_string(object) + " that is not defined");
}

void Parser::parse_attribute_value(bool relation) {
  const Token& keyword = advance();  // BA_ / BA_REL_
  static_cast<void>(relation);
  attr_message_ = nullptr;
  attr_signal_ = nullptr;
  attr_node_ = nullptr;

  std::string name;
  if (!parse_string(name)) {
    resynchronise();
    return;
  }
  const AttributeDefinition* def = db_.find_attribute_definition(name);
  AttributeObject object = AttributeObject::Database;

  if (current().kind == TokenKind::Identifier) {
    const std::string_view w = current().text;
    if (w == "BU_") {
      advance();
      object = AttributeObject::Node;
      std::string node;
      if (!parse_name(node)) {
        resynchronise();
        return;
      }
      attr_node_ = db_.find_node(node);
    } else if (w == "BO_") {
      advance();
      object = AttributeObject::Message;
      std::uint64_t raw_id = 0;
      const Token& id_token = current();
      if (!parse_unsigned(raw_id)) {
        resynchronise();
        return;
      }
      attr_message_ = db_.find_message(parse_message_id(raw_id, id_token));
    } else if (w == "SG_") {
      advance();
      object = AttributeObject::Signal;
      std::uint64_t raw_id = 0;
      const Token& id_token = current();
      if (!parse_unsigned(raw_id)) {
        resynchronise();
        return;
      }
      std::string signal_name;
      if (!parse_name(signal_name)) {
        resynchronise();
        return;
      }
      Message* m = db_.find_message(parse_message_id(raw_id, id_token));
      attr_message_ = m;
      attr_signal_ = m != nullptr ? m->find_signal(signal_name) : nullptr;
    } else if (w == "EV_") {
      advance();
      object = AttributeObject::EnvVar;
      std::string ignored;
      parse_name(ignored);
    }
  }

  AttributeValue value = parse_attribute_literal(def);
  match(TokenKind::Semicolon);
  assign_attribute(name, std::move(value), object, keyword);
}

void Parser::parse_value_descriptions() {
  advance();  // VAL_
  std::uint64_t raw_id = 0;
  const Token& id_token = current();
  if (!parse_unsigned(raw_id)) {
    // VAL_ with a name instead of an id addresses an environment variable.
    resynchronise();
    return;
  }
  std::string signal_name;
  if (!parse_name(signal_name)) {
    resynchronise();
    return;
  }
  std::vector<ValueDescription> values;
  while (current().kind == TokenKind::Integer ||
         current().kind == TokenKind::Minus) {
    double v = 0.0;
    if (!parse_number(v)) {
      break;
    }
    ValueDescription vd;
    vd.value = static_cast<std::int64_t>(v);
    if (!parse_string(vd.text)) {
      break;
    }
    values.push_back(std::move(vd));
  }
  match(TokenKind::Semicolon);

  Message* m = db_.find_message(parse_message_id(raw_id, id_token));
  Signal* s = m != nullptr ? m->find_signal(signal_name) : nullptr;
  if (s == nullptr) {
    warn(ErrorCode::ParseUndefinedReference, id_token,
         "VAL_ refers to signal '" + signal_name + "', which is not defined");
    return;
  }
  s->set_value_descriptions(std::move(values));
}

void Parser::parse_signal_value_type() {
  advance();  // SIG_VALTYPE_
  std::uint64_t raw_id = 0;
  const Token& id_token = current();
  if (!parse_unsigned(raw_id)) {
    resynchronise();
    return;
  }
  std::string signal_name;
  if (!parse_name(signal_name)) {
    resynchronise();
    return;
  }
  // The colon is in the grammar but is omitted by some exporters.
  match(TokenKind::Colon);
  std::uint64_t kind = 0;
  const Token& kind_token = current();
  if (!parse_unsigned(kind)) {
    resynchronise();
    return;
  }
  match(TokenKind::Semicolon);

  Message* m = db_.find_message(parse_message_id(raw_id, id_token));
  Signal* s = m != nullptr ? m->find_signal(signal_name) : nullptr;
  if (s == nullptr) {
    warn(ErrorCode::ParseUndefinedReference, id_token,
         "SIG_VALTYPE_ refers to signal '" + signal_name + "', which is not defined");
    return;
  }
  SignalLayout layout = s->layout();
  switch (kind) {
    case 0: layout.value_type = ValueType::Integer; break;
    case 1: layout.value_type = ValueType::Float32; break;
    case 2: layout.value_type = ValueType::Float64; break;
    default:
      error(ErrorCode::ParseSemantic, kind_token,
            "signal value type must be 0 (integer), 1 (float) or 2 (double)");
      return;
  }
  if (layout.value_type == ValueType::Float32 && layout.bit_length != 32u) {
    error(ErrorCode::CodecBadBitLength, kind_token,
          "a signal declared as IEEE float must be 32 bits wide");
    return;
  }
  if (layout.value_type == ValueType::Float64 && layout.bit_length != 64u) {
    error(ErrorCode::CodecBadBitLength, kind_token,
          "a signal declared as IEEE double must be 64 bits wide");
    return;
  }
  s->set_layout(layout);
}

void Parser::parse_extended_multiplexing() {
  advance();  // SG_MUL_VAL_
  std::uint64_t raw_id = 0;
  const Token& id_token = current();
  if (!parse_unsigned(raw_id)) {
    resynchronise();
    return;
  }
  std::string multiplexed;
  std::string multiplexor;
  if (!parse_name(multiplexed) || !parse_name(multiplexor)) {
    resynchronise();
    return;
  }
  std::vector<MuxRange> ranges;
  for (;;) {
    if (current().kind != TokenKind::Integer) {
      break;
    }
    MuxRange r;
    r.low = static_cast<std::uint32_t>(advance().integer);
    if (match(TokenKind::Minus)) {
      if (current().kind != TokenKind::Integer) {
        error(ErrorCode::ParseBadNumber, current(),
              "expected the upper bound of a multiplex range");
        break;
      }
      r.high = static_cast<std::uint32_t>(advance().integer);
    } else {
      r.high = r.low;
    }
    if (r.high < r.low) {
      warn(ErrorCode::ParseSemantic, id_token,
           "multiplex range is written high to low; swapping the bounds");
      std::swap(r.low, r.high);
    }
    ranges.push_back(r);
    if (!match(TokenKind::Comma)) {
      break;
    }
  }
  match(TokenKind::Semicolon);

  Message* m = db_.find_message(parse_message_id(raw_id, id_token));
  Signal* s = m != nullptr ? m->find_signal(multiplexed) : nullptr;
  if (s == nullptr) {
    warn(ErrorCode::ParseUndefinedReference, id_token,
         "SG_MUL_VAL_ refers to signal '" + multiplexed + "', which is not defined");
    return;
  }
  if (m != nullptr && m->find_signal(multiplexor) == nullptr) {
    warn(ErrorCode::ParseUndefinedReference, id_token,
         "SG_MUL_VAL_ names multiplexor '" + multiplexor +
             "', which is not a signal of this message");
  }
  // A signal already marked `M` in its SG_ line becomes `Both` once
  // SG_MUL_VAL_ says which multiplexor values switch it in.
  s->set_multiplex_role(s->multiplex_role() == MultiplexRole::Multiplexor ||
                                s->multiplex_role() == MultiplexRole::Both
                            ? MultiplexRole::Both
                            : MultiplexRole::Multiplexed);
  s->set_multiplexor_name(multiplexor);
  s->set_multiplex_ranges(std::move(ranges));
}

}  // namespace

ParseResult parse_string(std::string_view text, std::string_view filename) {
  ParseResult result;
  const Source source =
      Source::normalise(std::string(text), std::string(filename));
  result.had_bom = source.had_bom();
  result.had_crlf = source.had_crlf();
  result.had_latin1 = source.had_latin1();
  result.had_no_final_newline = source.had_no_final_newline();

  Lexer lexer(source, result.diagnostics);
  Parser parser(source, lexer.tokenise(), result.diagnostics);
  result.database = parser.run();
  return result;
}

core::Result<ParseResult> parse_file(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return core::Error(core::ErrorCode::ParseIoError, "cannot open the file");
  }
  std::ostringstream buffer;
  buffer << in.rdbuf();
  return parse_string(buffer.str(), path);
}

core::Result<core::Database> load(const std::string& path) {
  CANFORGE_TRY(auto parsed, parse_file(path));
  if (!parsed.ok()) {
    const Diagnostic* first = nullptr;
    for (const Diagnostic& d : parsed.diagnostics.items()) {
      if (d.severity == Severity::Error) {
        first = &d;
        break;
      }
    }
    return core::Error(first != nullptr ? first->code
                                        : core::ErrorCode::ParseSemantic,
                       "the DBC file contains errors; see the diagnostics",
                       {first != nullptr ? first->where.line : 0,
                        first != nullptr ? first->where.column : 0});
  }
  return std::move(parsed.database);
}

}  // namespace canforge::dbc
