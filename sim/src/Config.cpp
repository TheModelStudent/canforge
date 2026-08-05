// SPDX-License-Identifier: MIT
#include "canforge/sim/Config.hpp"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <sstream>

#include "canforge/text/Lexer.hpp"

namespace canforge::sim {
namespace {

using core::Error;
using core::ErrorCode;
using text::DiagnosticSink;
using text::Lexer;
using text::Severity;
using text::Source;
using text::Token;
using text::TokenKind;

/// A duration written as `10ms`, `1.5s`, `250us`. The lexer folds `10ms` into
/// a single identifier (digits followed by letters) but leaves `1.5s` as a
/// real followed by an identifier, because a real literal stops the
/// reclassification. Both spellings are accepted here rather than changing the
/// lexer, which the DBC parser also depends on.
bool duration_from(std::string_view text, std::uint64_t& out) noexcept {
  std::size_t i = 0;
  while (i < text.size() &&
         ((text[i] >= '0' && text[i] <= '9') || text[i] == '.')) {
    ++i;
  }
  if (i == 0) {
    return false;
  }
  const double value = std::strtod(std::string(text.substr(0, i)).c_str(), nullptr);
  const std::string_view unit = text.substr(i);
  double scale = 1e9;  // bare number means seconds
  if (unit == "ns") {
    scale = 1.0;
  } else if (unit == "us") {
    scale = 1e3;
  } else if (unit == "ms") {
    scale = 1e6;
  } else if (unit == "s" || unit.empty()) {
    scale = 1e9;
  } else if (unit == "m" || unit == "min") {
    scale = 60e9;
  } else {
    return false;
  }
  if (value < 0.0) {
    return false;
  }
  out = static_cast<std::uint64_t>(value * scale);
  return true;
}

/// A quantity like `1500kg` or `0.32m`; the unit is accepted and ignored,
/// since the field it lands in already fixes the unit.
bool number_from(std::string_view text, double& out) noexcept {
  std::size_t i = 0;
  if (i < text.size() && (text[i] == '-' || text[i] == '+')) {
    ++i;
  }
  const std::size_t start = i;
  while (i < text.size() &&
         ((text[i] >= '0' && text[i] <= '9') || text[i] == '.')) {
    ++i;
  }
  if (i == start) {
    return false;
  }
  out = std::strtod(std::string(text.substr(0, i)).c_str(), nullptr);
  return true;
}

class ConfigParser {
 public:
  ConfigParser(const Source& source, std::vector<Token> tokens, DiagnosticSink& sink)
      : source_(&source), tokens_(std::move(tokens)), sink_(&sink) {}

  SimConfig run();

 private:
  const Token& peek(std::size_t ahead = 0) const {
    return tokens_[std::min(pos_ + ahead, tokens_.size() - 1)];
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
  bool match_word(std::string_view w) {
    if (current().is_word(w)) {
      advance();
      return true;
    }
    return false;
  }

  void report(Severity sev, std::string message, std::string note = {}) {
    text::Diagnostic d;
    d.severity = sev;
    d.code = ErrorCode::ParseSemantic;
    d.where = current().where;
    d.message = std::move(message);
    d.token = current().kind == TokenKind::End ? "<end of file>"
                                               : std::string(current().text);
    d.source_line = std::string(source_->line(current().where.line));
    d.note = std::move(note);
    sink_->add(std::move(d));
  }
  void error(std::string message, std::string note = {}) {
    report(Severity::Error, std::move(message), std::move(note));
  }
  void warn(std::string message, std::string note = {}) {
    report(Severity::Warning, std::move(message), std::move(note));
  }

  /// Skip to the start of the next line that begins a new statement.
  void skip_line() {
    const std::uint32_t line = current().where.line;
    while (!at_end() && current().where.line == line) {
      advance();
    }
  }

  bool name(std::string& out) {
    if (current().kind != TokenKind::Identifier &&
        current().kind != TokenKind::String) {
      error("expected a name");
      return false;
    }
    const Token& t = advance();
    out = t.kind == TokenKind::String ? t.value : std::string(t.text);
    return true;
  }

  bool number(double& out) {
    double sign = 1.0;
    if (current().kind == TokenKind::Minus) {
      sign = -1.0;
      advance();
    } else if (current().kind == TokenKind::Plus) {
      advance();
    }
    if (current().kind == TokenKind::Integer || current().kind == TokenKind::Real) {
      out = sign * advance().real;
      return true;
    }
    // `1500kg` arrives as one identifier.
    if (current().kind == TokenKind::Identifier &&
        number_from(current().text, out)) {
      out *= sign;
      advance();
      return true;
    }
    error("expected a number");
    return false;
  }

  bool duration(std::uint64_t& out) {
    if (current().kind == TokenKind::Identifier &&
        duration_from(current().text, out)) {
      advance();
      return true;
    }
    if (current().kind == TokenKind::Integer || current().kind == TokenKind::Real) {
      const double value = advance().real;
      std::uint64_t scale_ns = 1000000000ULL;
      if (current().kind == TokenKind::Identifier) {
        std::uint64_t probe = 0;
        const std::string unit = "1" + std::string(current().text);
        if (duration_from(unit, probe)) {
          scale_ns = probe;
          advance();
        }
      }
      out = static_cast<std::uint64_t>(value * static_cast<double>(scale_ns));
      return true;
    }
    error("expected a duration such as 10ms, 250us or 1.5s");
    return false;
  }

  void parse_bus();
  void parse_database();
  void parse_node();
  void parse_tx(NodeSpec& node);
  void parse_plant();
  void parse_drive();
  void parse_fault();

  /// Collect the rest of the line verbatim; source expressions are re-parsed
  /// later by build_source(), which keeps the two concerns apart.
  std::string rest_of_line();

  const Source* source_;
  std::vector<Token> tokens_;
  DiagnosticSink* sink_;
  std::size_t pos_ = 0;
  SimConfig config_;
};

std::string ConfigParser::rest_of_line() {
  const std::uint32_t line = current().where.line;
  const std::string_view text = source_->line(line);
  const std::uint32_t from = current().where.column;
  std::string out;
  if (from >= 1 && from <= text.size()) {
    out = std::string(text.substr(from - 1));
  }
  while (!at_end() && current().where.line == line) {
    advance();
  }
  const std::size_t begin = out.find_first_not_of(" \t\r");
  const std::size_t end = out.find_last_not_of(" \t\r");
  return begin == std::string::npos ? std::string{}
                                    : out.substr(begin, end - begin + 1);
}

void ConfigParser::parse_bus() {
  advance();  // bus
  if (!name(config_.bus)) {
    skip_line();
    return;
  }
  double v = 0.0;
  if (current().kind != TokenKind::End && current().where.line == peek(0).where.line &&
      (current().kind == TokenKind::Integer || current().kind == TokenKind::Real ||
       current().kind == TokenKind::Identifier)) {
    if (number(v)) {
      config_.nominal_bitrate = static_cast<std::uint32_t>(v);
    }
  }
  if (current().kind == TokenKind::Integer || current().kind == TokenKind::Real) {
    if (number(v)) {
      config_.data_bitrate = static_cast<std::uint32_t>(v);
    }
  }
}

void ConfigParser::parse_database() {
  advance();  // database
  // A path may contain dots and slashes, which the lexer splits into separate
  // tokens, so the rest of the line is taken verbatim. A quoted string works
  // too, for a path containing spaces.
  if (current().kind == TokenKind::String) {
    config_.database_path = advance().value;
    return;
  }
  config_.database_path = rest_of_line();
  if (config_.database_path.empty()) {
    error("database needs a file path");
  }
}

void ConfigParser::parse_tx(NodeSpec& node) {
  advance();  // tx
  TxSpec tx;
  if (!name(tx.message)) {
    skip_line();
    return;
  }
  while (!at_end() && current().kind == TokenKind::Identifier &&
         !current().first_on_line) {
    if (match_word("cycle")) {
      if (!duration(tx.cycle_ns)) {
        break;
      }
    } else if (match_word("jitter")) {
      if (!duration(tx.jitter_ns)) {
        break;
      }
    } else if (match_word("phase")) {
      if (!duration(tx.phase_ns)) {
        break;
      }
    } else if (match_word("onchange")) {
      tx.mode = TxMode::OnChange;
    } else if (match_word("event")) {
      tx.mode = TxMode::Event;
    } else {
      error("unknown tx option",
            "expected one of: cycle, jitter, phase, onchange, event");
      skip_line();
      break;
    }
  }

  // Indented body: signal / counter / checksum lines, until the next
  // statement that is not one of those.
  for (;;) {
    if (at_end()) {
      break;
    }
    if (current().is_word("signal")) {
      advance();
      TxSpec::Binding binding;
      if (!name(binding.signal)) {
        skip_line();
        continue;
      }
      // The '=' is not a token the lexer knows, so it is optional and simply
      // skipped when present; everything after the signal name is the source
      // expression.
      const std::string expression = rest_of_line();
      std::string trimmed = expression;
      if (!trimmed.empty() && trimmed.front() == '=') {
        trimmed.erase(0, 1);
        const std::size_t begin = trimmed.find_first_not_of(" \t");
        trimmed = begin == std::string::npos ? std::string{} : trimmed.substr(begin);
      }
      if (trimmed.rfind("plant.", 0) == 0) {
        binding.plant_field = trimmed.substr(6);
      } else {
        binding.expression = trimmed;
      }
      tx.bindings.push_back(std::move(binding));
      continue;
    }
    if (current().is_word("counter")) {
      advance();
      if (!name(tx.counter_signal)) {
        skip_line();
      }
      continue;
    }
    if (current().is_word("checksum")) {
      advance();
      if (!name(tx.checksum_signal)) {
        skip_line();
        continue;
      }
      tx.checksum = ChecksumKind::Crc8Sae;
      if (current().kind == TokenKind::Identifier && !current().first_on_line) {
        const std::string kind(advance().text);
        if (kind == "crc8" || kind == "crc8sae") {
          tx.checksum = ChecksumKind::Crc8Sae;
        } else if (kind == "xor" || kind == "xor8") {
          tx.checksum = ChecksumKind::Xor8;
        } else {
          warn("unknown checksum kind '" + kind + "'; using crc8");
        }
      }
      continue;
    }
    break;
  }
  node.transmits.push_back(std::move(tx));
}

void ConfigParser::parse_node() {
  advance();  // node
  NodeSpec node;
  if (!name(node.name)) {
    skip_line();
    return;
  }
  while (!at_end() && current().is_word("tx")) {
    parse_tx(node);
  }
  config_.nodes.push_back(std::move(node));
}

void ConfigParser::parse_plant() {
  advance();  // plant
  config_.has_plant = true;
  VehicleParams& p = config_.plant;
  // An optional name after `plant` is accepted and ignored; only one plant is
  // modelled, but naming it reads better in a config.
  if (current().kind == TokenKind::Identifier && !current().first_on_line) {
    advance();
  }
  for (;;) {
    if (at_end() || current().kind != TokenKind::Identifier) {
      break;
    }
    const std::string_view key = current().text;
    double v = 0.0;
    if (key == "mass") {
      advance();
      if (number(v)) { p.mass_kg = v; }
    } else if (key == "wheel_radius") {
      advance();
      if (number(v)) { p.wheel_radius_m = v; }
    } else if (key == "drag_area") {
      advance();
      if (number(v)) { p.drag_area = v; }
    } else if (key == "rolling_resistance") {
      advance();
      if (number(v)) { p.rolling_resistance = v; }
    } else if (key == "final_drive") {
      advance();
      if (number(v)) { p.final_drive = v; }
    } else if (key == "idle_rpm") {
      advance();
      if (number(v)) { p.idle_rpm = v; }
    } else if (key == "max_rpm") {
      advance();
      if (number(v)) { p.max_rpm = v; }
    } else if (key == "peak_torque") {
      advance();
      if (number(v)) { p.peak_torque_nm = v; }
    } else if (key == "ambient_temp") {
      advance();
      if (number(v)) { p.ambient_temp_c = v; }
    } else if (key == "gear_ratios") {
      advance();
      std::vector<double> ratios;
      while ((current().kind == TokenKind::Integer ||
              current().kind == TokenKind::Real) &&
             !current().first_on_line) {
        ratios.push_back(advance().real);
      }
      if (ratios.empty()) {
        error("gear_ratios needs at least one ratio");
      } else {
        p.gear_ratios = std::move(ratios);
      }
    } else {
      break;  // not a plant key; hand back to the top level
    }
  }
}

void ConfigParser::parse_drive() {
  advance();  // drive
  while (!at_end() && current().is_word("at")) {
    advance();
    DriveEvent event;
    if (!duration(event.at_ns)) {
      skip_line();
      continue;
    }
    while (!at_end() && current().kind == TokenKind::Identifier &&
           !current().first_on_line) {
      double v = 0.0;
      if (match_word("throttle")) {
        if (number(v)) {
          event.has_throttle = true;
          event.throttle_pct = v;
        }
      } else if (match_word("brake")) {
        if (number(v)) {
          event.has_brake = true;
          event.brake_bar = v;
        }
      } else if (match_word("gear")) {
        if (number(v)) {
          event.has_gear = true;
          event.gear = static_cast<int>(v);
        }
      } else {
        error("unknown drive input",
              "expected one of: throttle, brake, gear");
        skip_line();
        break;
      }
    }
    config_.drive.push_back(event);
  }
}

void ConfigParser::parse_fault() {
  advance();  // fault
  Fault fault;
  if (!name(fault.name)) {
    skip_line();
    return;
  }
  if (current().kind != TokenKind::Identifier) {
    error("expected a fault kind");
    skip_line();
    return;
  }
  const std::string kind(advance().text);
  double v = 0.0;
  if (kind == "drop") {
    fault.kind = FaultKind::DropFrames;
    if (number(v)) {
      fault.probability = v;
    }
  } else if (kind == "bitflip") {
    fault.kind = FaultKind::BitFlip;
    if (number(v)) {
      fault.probability = v;
    }
  } else if (kind == "delay") {
    fault.kind = FaultKind::DelayMessage;
    if (!duration(fault.delay_ns)) {
      skip_line();
      return;
    }
  } else if (kind == "freeze") {
    fault.kind = FaultKind::FreezeSignal;
    if (!name(fault.signal)) {
      skip_line();
      return;
    }
  } else if (kind == "range" || kind == "out_of_range") {
    fault.kind = FaultKind::OutOfRange;
    if (!name(fault.signal)) {
      skip_line();
      return;
    }
    if (number(v)) {
      fault.value = v;
    }
  } else if (kind == "stop") {
    fault.kind = FaultKind::StopNode;
  } else {
    error("unknown fault kind '" + kind + "'",
          "expected one of: drop, bitflip, delay, freeze, range, stop");
    skip_line();
    return;
  }

  while (!at_end() && current().kind == TokenKind::Identifier &&
         !current().first_on_line) {
    if (match_word("node")) {
      if (!name(fault.node)) {
        break;
      }
    } else if (match_word("message") || match_word("id")) {
      double raw = 0.0;
      if (!number(raw)) {
        break;
      }
      const auto packed = static_cast<std::uint32_t>(raw);
      auto id = packed > core::kStandardIdMax ? core::CanId::extended(packed)
                                              : core::CanId::standard(packed);
      if (!id) {
        error("message identifier is out of range");
        break;
      }
      fault.id = id.value();
      fault.has_id = true;
    } else if (match_word("active")) {
      fault.active = true;
    } else {
      error("unknown fault option",
            "expected one of: node, message, active");
      skip_line();
      break;
    }
  }
  config_.faults.push_back(std::move(fault));
}

SimConfig ConfigParser::run() {
  while (!at_end()) {
    if (current().kind != TokenKind::Identifier) {
      error("expected a section keyword");
      skip_line();
      continue;
    }
    const std::string_view word = current().text;
    if (word == "bus") {
      parse_bus();
    } else if (word == "database") {
      parse_database();
    } else if (word == "node") {
      parse_node();
    } else if (word == "plant") {
      parse_plant();
    } else if (word == "drive") {
      parse_drive();
    } else if (word == "fault") {
      parse_fault();
    } else {
      error("unknown section '" + std::string(word) + "'",
            "expected one of: bus, database, node, plant, drive, fault");
      skip_line();
    }
  }
  return std::move(config_);
}

std::vector<std::string> split_words(const std::string& text) {
  std::vector<std::string> out;
  std::istringstream in(text);
  std::string word;
  while (in >> word) {
    out.push_back(word);
  }
  return out;
}

core::Result<SourcePtr> build_one(const std::vector<std::string>& words,
                                  std::size_t& i);

/// `key value` lookup inside an expression's word list, consuming as it goes.
bool take_number(const std::vector<std::string>& words, std::size_t& i, double& out) {
  if (i >= words.size()) {
    return false;
  }
  return number_from(words[i++], out);
}

bool take_duration(const std::vector<std::string>& words, std::size_t& i,
                   std::uint64_t& out) {
  if (i >= words.size()) {
    return false;
  }
  return duration_from(words[i++], out);
}

core::Result<SourcePtr> build_one(const std::vector<std::string>& words,
                                  std::size_t& i) {
  if (i >= words.size()) {
    return Error(ErrorCode::ParseUnexpectedEof, "empty signal source expression");
  }
  const std::string kind = words[i++];

  if (kind == "const" || kind == "constant") {
    double v = 0.0;
    if (!take_number(words, i, v)) {
      return Error(ErrorCode::ParseBadNumber, "const needs a value");
    }
    return constant(v);
  }
  if (kind == "ramp") {
    double from = 0.0;
    double to = 0.0;
    std::uint64_t over = 1000000000ULL;
    if (!take_number(words, i, from)) {
      return Error(ErrorCode::ParseBadNumber, "ramp needs a start value");
    }
    if (i < words.size() && words[i] == "to") {
      ++i;
    }
    if (!take_number(words, i, to)) {
      return Error(ErrorCode::ParseBadNumber, "ramp needs an end value");
    }
    if (i < words.size() && words[i] == "over") {
      ++i;
      if (!take_duration(words, i, over)) {
        return Error(ErrorCode::ParseBadNumber, "ramp needs a duration after 'over'");
      }
    }
    RampMode mode = RampMode::Clamp;
    if (i < words.size() && words[i] == "repeat") {
      mode = RampMode::Repeat;
      ++i;
    } else if (i < words.size() && words[i] == "pingpong") {
      mode = RampMode::PingPong;
      ++i;
    }
    return ramp(from, to, over, mode);
  }
  if (kind == "sine") {
    double amp = 1.0;
    double freq = 1.0;
    double offset = 0.0;
    double phase = 0.0;
    while (i < words.size()) {
      if (words[i] == "amp" && i + 1 < words.size()) {
        ++i;
        take_number(words, i, amp);
      } else if (words[i] == "freq" && i + 1 < words.size()) {
        ++i;
        take_number(words, i, freq);
      } else if (words[i] == "offset" && i + 1 < words.size()) {
        ++i;
        take_number(words, i, offset);
      } else if (words[i] == "phase" && i + 1 < words.size()) {
        ++i;
        take_number(words, i, phase);
      } else {
        break;
      }
    }
    return sine(amp, freq, offset, phase);
  }
  if (kind == "square") {
    double low = 0.0;
    double high = 1.0;
    std::uint64_t period = 1000000000ULL;
    double duty = 0.5;
    while (i < words.size()) {
      if (words[i] == "low" && i + 1 < words.size()) {
        ++i;
        take_number(words, i, low);
      } else if (words[i] == "high" && i + 1 < words.size()) {
        ++i;
        take_number(words, i, high);
      } else if (words[i] == "period" && i + 1 < words.size()) {
        ++i;
        take_duration(words, i, period);
      } else if (words[i] == "duty" && i + 1 < words.size()) {
        ++i;
        take_number(words, i, duty);
      } else {
        break;
      }
    }
    return square(low, high, period, duty);
  }
  if (kind == "walk" || kind == "random_walk") {
    double start = 0.0;
    double step = 1.0;
    double low = 0.0;
    double high = 100.0;
    double seed = 1.0;
    std::uint64_t interval = 100000000ULL;
    while (i < words.size()) {
      if (words[i] == "start" && i + 1 < words.size()) {
        ++i;
        take_number(words, i, start);
      } else if (words[i] == "step" && i + 1 < words.size()) {
        ++i;
        take_number(words, i, step);
      } else if (words[i] == "min" && i + 1 < words.size()) {
        ++i;
        take_number(words, i, low);
      } else if (words[i] == "max" && i + 1 < words.size()) {
        ++i;
        take_number(words, i, high);
      } else if (words[i] == "seed" && i + 1 < words.size()) {
        ++i;
        take_number(words, i, seed);
      } else if (words[i] == "every" && i + 1 < words.size()) {
        ++i;
        take_duration(words, i, interval);
      } else {
        break;
      }
    }
    return random_walk(start, step, low, high,
                       static_cast<std::uint64_t>(seed), interval);
  }
  if (kind == "keyframes") {
    std::vector<Keyframe> frames;
    while (i < words.size()) {
      const std::string& word = words[i];
      const std::size_t eq = word.find('=');
      if (eq == std::string::npos) {
        break;
      }
      Keyframe frame;
      if (!duration_from(word.substr(0, eq), frame.at_ns)) {
        break;
      }
      if (!number_from(word.substr(eq + 1), frame.value)) {
        break;
      }
      frames.push_back(frame);
      ++i;
    }
    if (frames.empty()) {
      return Error(ErrorCode::ParseSemantic,
                   "keyframes needs at least one <time>=<value> pair");
    }
    return keyframes(std::move(frames), true);
  }
  // A bare number is a constant.
  double literal = 0.0;
  if (number_from(kind, literal)) {
    return constant(literal);
  }
  return Error(ErrorCode::ParseUnknownKeyword, "unknown signal source");
}

}  // namespace

core::Result<SourcePtr> build_source(const std::string& expression) {
  const std::vector<std::string> words = split_words(expression);
  std::size_t i = 0;
  CANFORGE_TRY(auto first, build_one(words, i));
  SourcePtr result = std::move(first);

  // Composition: `<source> + <source>` sums, `<source> * <number>` scales.
  while (i < words.size()) {
    if (words[i] == "+") {
      ++i;
      CANFORGE_TRY(auto next, build_one(words, i));
      std::vector<SourcePtr> parts;
      parts.push_back(std::move(result));
      parts.push_back(std::move(next));
      result = sum(std::move(parts));
      continue;
    }
    if (words[i] == "*") {
      ++i;
      double factor = 1.0;
      if (!take_number(words, i, factor)) {
        return Error(ErrorCode::ParseBadNumber, "'*' needs a number");
      }
      result = scale(std::move(result), factor);
      continue;
    }
    if (words[i] == "clamp" && i + 2 < words.size()) {
      ++i;
      double low = 0.0;
      double high = 0.0;
      if (!take_number(words, i, low) || !take_number(words, i, high)) {
        return Error(ErrorCode::ParseBadNumber, "clamp needs a low and a high value");
      }
      result = clamp_to(std::move(result), low, high);
      continue;
    }
    return Error(ErrorCode::ParseUnexpectedToken,
                 "trailing text after the signal source expression");
  }
  return result;
}

ConfigParseResult parse_config_string(std::string_view text,
                                      std::string_view filename) {
  ConfigParseResult result;
  const Source source = Source::normalise(std::string(text), std::string(filename));
  Lexer lexer(source, result.diagnostics);
  ConfigParser parser(source, lexer.tokenise(), result.diagnostics);
  result.config = parser.run();

  // Validate every source expression now, so a typo is reported with the rest
  // of the parse rather than at the first simulation step.
  for (const NodeSpec& node : result.config.nodes) {
    for (const TxSpec& tx : node.transmits) {
      for (const TxSpec::Binding& b : tx.bindings) {
        if (!b.plant_field.empty()) {
          double probe = 0.0;
          const Vehicle blank;
          if (!blank.read(b.plant_field, probe)) {
            text::Diagnostic d;
            d.severity = Severity::Error;
            d.code = ErrorCode::ParseUndefinedReference;
            d.message = "unknown plant field 'plant." + b.plant_field + "'";
            d.note = "valid fields: ";
            for (const std::string_view f : Vehicle::field_names()) {
              d.note += std::string(f) + " ";
            }
            result.diagnostics.add(std::move(d));
          }
          continue;
        }
        auto built = build_source(b.expression);
        if (!built) {
          text::Diagnostic d;
          d.severity = Severity::Error;
          d.code = built.error().code();
          d.message = "signal '" + b.signal + "': " +
                      std::string(built.error().message());
          d.token = b.expression;
          result.diagnostics.add(std::move(d));
        }
      }
    }
  }
  return result;
}

core::Result<ConfigParseResult> parse_config_file(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return Error(ErrorCode::ParseIoError, "cannot open the simulator config");
  }
  std::ostringstream buffer;
  buffer << in.rdbuf();
  return parse_config_string(buffer.str(), path);
}

}  // namespace canforge::sim
