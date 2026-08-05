// SPDX-License-Identifier: MIT
#ifndef CANFORGE_TEXT_LEXER_HPP
#define CANFORGE_TEXT_LEXER_HPP

/// Hand-written lexer for the DBC and simulator config languages.
///
/// Hand written because column-accurate diagnostics were a requirement, and
/// because no authoritative DBC grammar exists -- the format is whatever
/// Vector's tools emit, so every real-world tolerance is a deviation from the
/// documented grammar. Recursive descent puts each tolerance next to the rule
/// it bends, with a comment naming the tool that needs it.
///
/// The lexer does not know keywords: BO_ and SG_ are just identifiers. That is
/// what makes NS_ -- a list whose entries are spelled exactly like the keywords
/// they name -- tractable.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "canforge/core/Result.hpp"
#include "canforge/text/Diagnostic.hpp"

namespace canforge::text {

class Source {
 public:
  /// Normalises `bytes` in place:
  ///   * strips a UTF-8 byte order mark
  ///   * converts CRLF and lone CR to LF
  ///   * transcodes Latin-1 to UTF-8 when the input is not valid UTF-8
  ///   * appends a trailing newline if the file lacks one
  static Source normalise(std::string bytes, std::string name);

  const std::string& text() const noexcept { return text_; }
  const std::string& name() const noexcept { return name_; }

  /// The full text of a 1-based line, without its newline.
  std::string_view line(std::uint32_t number) const;

  bool had_bom() const noexcept { return had_bom_; }
  bool had_crlf() const noexcept { return had_crlf_; }
  bool had_latin1() const noexcept { return had_latin1_; }
  bool had_no_final_newline() const noexcept { return had_no_final_newline_; }

 private:
  std::string text_;
  std::string name_;
  std::vector<std::size_t> line_starts_;
  bool had_bom_ = false;
  bool had_crlf_ = false;
  bool had_latin1_ = false;
  bool had_no_final_newline_ = false;
};

enum class TokenKind : std::uint8_t {
  End,
  Identifier,
  Integer,
  Real,
  String,
  Colon,
  Semicolon,
  Comma,
  Pipe,
  At,
  Plus,
  Minus,
  LParen,
  RParen,
  LBracket,
  RBracket,
  Equals,
  Dot,
  Slash,
  Unknown,
};

const char* to_string(TokenKind k) noexcept;

struct Token {
  TokenKind kind = TokenKind::End;
  std::string_view text;  ///< Slice of the normalised source.
  SourceLocation where;
  std::uint64_t integer = 0;  ///< Valid for Integer.
  double real = 0.0;          ///< Valid for Integer and Real.
  std::string value;          ///< Unescaped contents, valid for String.
  bool first_on_line = false;

  bool is(TokenKind k) const noexcept { return kind == k; }
  bool is_word(std::string_view w) const noexcept {
    return kind == TokenKind::Identifier && text == w;
  }
};

/// The whole token stream is produced up front. DBC files are a few megabytes
/// at most, and having the vector makes lookahead and resynchronisation trivial.
class Lexer {
 public:
  Lexer(const Source& source, DiagnosticSink& diagnostics)
      : source_(&source), diagnostics_(&diagnostics) {}

  std::vector<Token> tokenise();

 private:
  char peek(std::size_t ahead = 0) const noexcept;
  bool at_end() const noexcept;
  char advance() noexcept;
  void skip_trivia() noexcept;
  SourceLocation here() const noexcept { return {line_, column_}; }
  void error(core::ErrorCode code, SourceLocation where, std::string message,
             std::string token);

  Token lex_number();
  Token lex_identifier();
  Token lex_string();

  const Source* source_;
  DiagnosticSink* diagnostics_;
  std::size_t pos_ = 0;
  std::uint32_t line_ = 1;
  std::uint32_t column_ = 1;
  bool at_line_start_ = true;
};

}  // namespace canforge::text

#endif  // CANFORGE_TEXT_LEXER_HPP
