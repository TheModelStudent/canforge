// SPDX-License-Identifier: MIT
#include "canforge/text/Json.hpp"

#include <cmath>
#include <cstdlib>

namespace canforge::text {

using core::Error;
using core::ErrorCode;
using core::Result;

namespace {

class JsonReader {
 public:
  explicit JsonReader(std::string_view text) : text_(text) {}

  Result<Json> read_document() {
    CANFORGE_TRY(Json value, read_value(0));
    skip_space();
    if (pos_ != text_.size()) {
      return fail(ErrorCode::ParseUnexpectedToken,
                  "text after the end of the JSON document");
    }
    return value;
  }

 private:
  // Deep enough for any sane document, shallow enough that a file of nothing
  // but '[' cannot overflow the stack.
  static constexpr int kMaxDepth = 32;

  Error fail(ErrorCode code, std::string_view message) const {
    return Error(code, message, {static_cast<std::uint32_t>(pos_), 0});
  }

  void skip_space() {
    while (pos_ < text_.size() && (text_[pos_] == ' ' || text_[pos_] == '\t' ||
                                   text_[pos_] == '\n' || text_[pos_] == '\r')) {
      ++pos_;
    }
  }

  bool consume(char c) {
    skip_space();
    if (pos_ < text_.size() && text_[pos_] == c) {
      ++pos_;
      return true;
    }
    return false;
  }

  bool consume_word(std::string_view word) {
    if (text_.substr(pos_, word.size()) == word) {
      pos_ += word.size();
      return true;
    }
    return false;
  }

  Result<Json> read_value(int depth) {
    if (depth > kMaxDepth) {
      return fail(ErrorCode::ParseSemantic, "JSON nested too deeply");
    }
    skip_space();
    if (pos_ >= text_.size()) {
      return fail(ErrorCode::ParseUnexpectedEof, "JSON ends where a value should be");
    }
    const char c = text_[pos_];
    if (c == '{') {
      return read_object(depth);
    }
    if (c == '[') {
      return read_array(depth);
    }
    if (c == '"') {
      CANFORGE_TRY(std::string s, read_string());
      Json value;
      value.type = Json::Type::String;
      value.string = std::move(s);
      return value;
    }
    Json value;
    if (consume_word("true")) {
      value.type = Json::Type::Bool;
      value.boolean = true;
      return value;
    }
    if (consume_word("false")) {
      value.type = Json::Type::Bool;
      return value;
    }
    if (consume_word("null")) {
      return value;
    }
    return read_number();
  }

  Result<Json> read_object(int depth) {
    Json value;
    value.type = Json::Type::Object;
    ++pos_;  // '{'
    if (consume('}')) {
      return value;
    }
    for (;;) {
      skip_space();
      if (pos_ >= text_.size() || text_[pos_] != '"') {
        return fail(ErrorCode::ParseUnexpectedToken,
                    "a JSON object member needs a name");
      }
      CANFORGE_TRY(std::string name, read_string());
      if (!consume(':')) {
        return fail(ErrorCode::ParseUnexpectedToken,
                    "expected ':' after a member name");
      }
      CANFORGE_TRY(Json member, read_value(depth + 1));
      value.object.emplace_back(std::move(name), std::move(member));
      if (consume(',')) {
        continue;
      }
      if (consume('}')) {
        return value;
      }
      return fail(ErrorCode::ParseUnexpectedToken,
                  "expected ',' or '}' in a JSON object");
    }
  }

  Result<Json> read_array(int depth) {
    Json value;
    value.type = Json::Type::Array;
    ++pos_;  // '['
    if (consume(']')) {
      return value;
    }
    for (;;) {
      CANFORGE_TRY(Json element, read_value(depth + 1));
      value.array.push_back(std::move(element));
      if (consume(',')) {
        continue;
      }
      if (consume(']')) {
        return value;
      }
      return fail(ErrorCode::ParseUnexpectedToken,
                  "expected ',' or ']' in a JSON array");
    }
  }

  static void append_utf8(std::string& out, std::uint32_t cp) {
    if (cp < 0x80u) {
      out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800u) {
      out.push_back(static_cast<char>(0xC0u | (cp >> 6u)));
      out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    } else {
      out.push_back(static_cast<char>(0xE0u | (cp >> 12u)));
      out.push_back(static_cast<char>(0x80u | ((cp >> 6u) & 0x3Fu)));
      out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    }
  }

  Result<std::string> read_string() {
    std::string out;
    ++pos_;  // opening quote
    for (;;) {
      if (pos_ >= text_.size()) {
        return fail(ErrorCode::ParseUnterminatedString,
                    "a JSON string is never closed");
      }
      const char c = text_[pos_++];
      if (c == '"') {
        return out;
      }
      if (static_cast<unsigned char>(c) < 0x20u) {
        return fail(ErrorCode::ParseBadEncoding,
                    "a control character inside a JSON string");
      }
      if (c != '\\') {
        out.push_back(c);
        continue;
      }
      if (pos_ >= text_.size()) {
        return fail(ErrorCode::ParseUnterminatedString,
                    "a JSON string is never closed");
      }
      const char e = text_[pos_++];
      switch (e) {
        case '"':
          out.push_back('"');
          break;
        case '\\':
          out.push_back('\\');
          break;
        case '/':
          out.push_back('/');
          break;
        case 'b':
          out.push_back('\b');
          break;
        case 'f':
          out.push_back('\f');
          break;
        case 'n':
          out.push_back('\n');
          break;
        case 'r':
          out.push_back('\r');
          break;
        case 't':
          out.push_back('\t');
          break;
        case 'u': {
          if (pos_ + 4 > text_.size()) {
            return fail(ErrorCode::ParseBadEncoding,
                        "a \\u escape needs four hex digits");
          }
          std::uint32_t cp = 0;
          for (int i = 0; i < 4; ++i) {
            const char h = text_[pos_++];
            std::uint32_t digit = 0;
            if (h >= '0' && h <= '9') {
              digit = static_cast<std::uint32_t>(h - '0');
            } else if (h >= 'a' && h <= 'f') {
              digit = static_cast<std::uint32_t>(h - 'a') + 10u;
            } else if (h >= 'A' && h <= 'F') {
              digit = static_cast<std::uint32_t>(h - 'A') + 10u;
            } else {
              return fail(ErrorCode::ParseBadEncoding,
                          "a \\u escape needs four hex digits");
            }
            cp = (cp << 4u) | digit;
          }
          // Surrogate pairs would need two escapes read together; names and
          // units of diagnostic data have no use for them.
          append_utf8(out, cp);
          break;
        }
        default:
          return fail(ErrorCode::ParseBadEncoding,
                      "an unknown escape in a JSON string");
      }
    }
  }

  Result<Json> read_number() {
    const std::size_t start = pos_;
    if (pos_ < text_.size() && text_[pos_] == '-') {
      ++pos_;
    }
    while (pos_ < text_.size() &&
           ((text_[pos_] >= '0' && text_[pos_] <= '9') || text_[pos_] == '.' ||
            text_[pos_] == 'e' || text_[pos_] == 'E' || text_[pos_] == '+' ||
            text_[pos_] == '-')) {
      ++pos_;
    }
    const std::string token(text_.substr(start, pos_ - start));
    char* end = nullptr;
    const double number = std::strtod(token.c_str(), &end);
    if (token.empty() || token == "-" || end != token.c_str() + token.size() ||
        !std::isfinite(number)) {
      pos_ = start;
      return fail(ErrorCode::ParseBadNumber, "not a JSON value");
    }
    Json value;
    value.type = Json::Type::Number;
    value.number = number;
    return value;
  }

  std::string_view text_;
  std::size_t pos_ = 0;
};

}  // namespace

Result<Json> parse_json(std::string_view text) {
  JsonReader reader(text);
  return reader.read_document();
}

}  // namespace canforge::text
