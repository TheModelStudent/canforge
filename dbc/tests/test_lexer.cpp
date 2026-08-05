// SPDX-License-Identifier: MIT
#include "canforge/dbc/Lexer.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace canforge::dbc {
namespace {

struct Lexed {
  Source source;
  DiagnosticSink sink;
  std::vector<Token> tokens;
};

Lexed lex(std::string text) {
  Lexed out;
  out.source = Source::normalise(std::move(text), "test.dbc");
  Lexer lexer(out.source, out.sink);
  out.tokens = lexer.tokenise();
  return out;
}

TEST(Source, StripsUtf8Bom) {
  const auto s = Source::normalise(
      "\xEF\xBB\xBF"
      "VERSION \"1\"\n",
      "x");
  EXPECT_TRUE(s.had_bom());
  EXPECT_EQ(s.text().substr(0, 7), "VERSION");
}

TEST(Source, NormalisesCrlfAndLoneCr) {
  const auto crlf = Source::normalise("a\r\nb\r\n", "x");
  EXPECT_TRUE(crlf.had_crlf());
  EXPECT_EQ(crlf.text(), "a\nb\n");
  EXPECT_EQ(crlf.line(1), "a");
  EXPECT_EQ(crlf.line(2), "b");

  const auto cr = Source::normalise("a\rb\r", "x");
  EXPECT_TRUE(cr.had_crlf());
  EXPECT_EQ(cr.text(), "a\nb\n");
}

TEST(Source, AddsAMissingFinalNewline) {
  const auto s = Source::normalise("VAL_ 1 X 0 \"a\" ;", "x");
  EXPECT_TRUE(s.had_no_final_newline());
  EXPECT_EQ(s.text().back(), '\n');
}

TEST(Source, TranscodesLatin1) {
  // 0xFC is 'u with diaeresis' in Latin-1 and an invalid UTF-8 lead byte.
  const std::string latin1 =
      "CM_ \"K\xFC"
      "hlung\";\n";
  const auto s = Source::normalise(latin1, "x");
  EXPECT_TRUE(s.had_latin1());
  EXPECT_NE(s.text().find("\xC3\xBC"), std::string::npos)
      << "0xFC should have become the two byte UTF-8 sequence C3 BC";
}

TEST(Source, LeavesValidUtf8Alone) {
  const std::string utf8 =
      "CM_ \"K\xC3\xBC"
      "hlung\";\n";
  const auto s = Source::normalise(utf8, "x");
  EXPECT_FALSE(s.had_latin1());
  EXPECT_EQ(s.text(), utf8);
}

TEST(Lexer, SignalLineTokenises) {
  const auto l = lex(" SG_ Speed : 0|16@1+ (0.125,-40) [0|8031.875] \"rpm\" A,B\n");
  ASSERT_GE(l.tokens.size(), 20u);
  EXPECT_TRUE(l.tokens[0].is_word("SG_"));
  EXPECT_TRUE(l.tokens[1].is_word("Speed"));
  EXPECT_EQ(l.tokens[2].kind, TokenKind::Colon);
  EXPECT_EQ(l.tokens[3].kind, TokenKind::Integer);
  EXPECT_EQ(l.tokens[3].integer, 0u);
  EXPECT_EQ(l.tokens[4].kind, TokenKind::Pipe);
  EXPECT_EQ(l.tokens[5].integer, 16u);
  EXPECT_EQ(l.tokens[6].kind, TokenKind::At);
  EXPECT_EQ(l.tokens[7].integer, 1u);
  EXPECT_EQ(l.tokens[8].kind, TokenKind::Plus);
  EXPECT_EQ(l.tokens[9].kind, TokenKind::LParen);
  EXPECT_EQ(l.tokens[10].kind, TokenKind::Real);
  EXPECT_DOUBLE_EQ(l.tokens[10].real, 0.125);
  EXPECT_EQ(l.tokens[11].kind, TokenKind::Comma);
  // The sign is a separate token, because '-' is also the signedness marker.
  EXPECT_EQ(l.tokens[12].kind, TokenKind::Minus);
  EXPECT_EQ(l.tokens[13].integer, 40u);
  EXPECT_FALSE(l.sink.has_errors());
}

TEST(Lexer, TracksLineAndColumn) {
  const auto l = lex("BO_ 1 A: 8 N\n SG_ S : 0|8@1+ (1,0) [0|1] \"\" N\n");
  EXPECT_EQ(l.tokens[0].where.line, 1u);
  EXPECT_EQ(l.tokens[0].where.column, 1u);
  const Token* sg = nullptr;
  for (const Token& t : l.tokens) {
    if (t.is_word("SG_")) {
      sg = &t;
      break;
    }
  }
  ASSERT_NE(sg, nullptr);
  EXPECT_EQ(sg->where.line, 2u);
  EXPECT_EQ(sg->where.column, 2u);
  EXPECT_TRUE(sg->first_on_line);
}

TEST(Lexer, Numbers) {
  const auto l = lex("1 -2 3.5 .5 1e3 1.5e-3 2E+2 0 4294967295\n");
  EXPECT_EQ(l.tokens[0].kind, TokenKind::Integer);
  EXPECT_EQ(l.tokens[1].kind, TokenKind::Minus);
  EXPECT_EQ(l.tokens[2].integer, 2u);
  EXPECT_EQ(l.tokens[3].kind, TokenKind::Real);
  EXPECT_DOUBLE_EQ(l.tokens[3].real, 3.5);
  EXPECT_DOUBLE_EQ(l.tokens[4].real, 0.5);
  EXPECT_DOUBLE_EQ(l.tokens[5].real, 1000.0);
  EXPECT_DOUBLE_EQ(l.tokens[6].real, 0.0015);
  EXPECT_DOUBLE_EQ(l.tokens[7].real, 200.0);
  EXPECT_EQ(l.tokens[8].integer, 0u);
  EXPECT_EQ(l.tokens[9].integer, 4294967295u);
  EXPECT_FALSE(l.sink.has_errors());
}

TEST(Lexer, IdentifierStartingWithADigit) {
  // Node names like `3PMS` are not legal per the documented grammar but do
  // occur; the lexer reclassifies instead of rejecting the file.
  const auto l = lex("BU_: 3PMS ECU_1 _private\n");
  EXPECT_TRUE(l.tokens[2].is_word("3PMS"));
  EXPECT_EQ(l.tokens[2].kind, TokenKind::Identifier);
  EXPECT_TRUE(l.tokens[3].is_word("ECU_1"));
  EXPECT_TRUE(l.tokens[4].is_word("_private"));
  EXPECT_FALSE(l.sink.has_errors());
}

TEST(Lexer, Strings) {
  const auto l = lex("\"plain\" \"with \\\"quote\\\"\" \"back\\\\slash\" \"\"\n");
  EXPECT_EQ(l.tokens[0].value, "plain");
  EXPECT_EQ(l.tokens[1].value, "with \"quote\"");
  EXPECT_EQ(l.tokens[2].value, "back\\slash");
  EXPECT_EQ(l.tokens[3].value, "");
  EXPECT_FALSE(l.sink.has_errors());
}

TEST(Lexer, DoubledQuoteEscape) {
  // Spreadsheet-derived exporters double the quote instead of escaping it.
  const auto l = lex("\"say \"\"hi\"\" now\"\n");
  EXPECT_EQ(l.tokens[0].value, "say \"hi\" now");
}

TEST(Lexer, MultiLineString) {
  const auto l = lex("CM_ \"first line\nsecond line\";\n");
  EXPECT_EQ(l.tokens[1].value, "first line\nsecond line");
  EXPECT_FALSE(l.sink.has_errors());
}

TEST(Lexer, UnterminatedStringIsReported) {
  const auto l = lex("CM_ \"never closed\n");
  EXPECT_TRUE(l.sink.has_errors());
  ASSERT_FALSE(l.sink.items().empty());
  EXPECT_EQ(l.sink.items().front().code, core::ErrorCode::ParseUnterminatedString);
}

TEST(Lexer, WindowsPathInsideAStringSurvives) {
  // A lone backslash that does not introduce a known escape is kept verbatim.
  const auto l = lex("\"C:\\temp\\file.dbc\"\n");
  EXPECT_EQ(l.tokens[0].value, "C:\\temp\\file.dbc");
}

TEST(Lexer, SkipsCppStyleComments) {
  const auto l = lex("// generated by a script\nVERSION \"1\"\n");
  EXPECT_TRUE(l.tokens[0].is_word("VERSION"));
}

TEST(Lexer, UnexpectedCharacterIsReported) {
  const auto l = lex("BO_ 1 A: 8 N $\n");
  ASSERT_TRUE(l.sink.has_errors());
  EXPECT_EQ(l.sink.items().front().code, core::ErrorCode::ParseUnexpectedToken);
  EXPECT_EQ(l.sink.items().front().token, "$");
}

TEST(Lexer, HashStartsAComment) {
  // The simulator config uses '#' for comments and shares this lexer. No DBC
  // file contains one outside a quoted string, which lex_string handles.
  const auto l = lex("# a comment\nBU_: A # trailing\n");
  EXPECT_FALSE(l.sink.has_errors());
  ASSERT_GE(l.tokens.size(), 4u);
  EXPECT_TRUE(l.tokens[0].is_word("BU_"));
  EXPECT_TRUE(l.tokens[2].is_word("A"));
  EXPECT_EQ(l.tokens[3].kind, TokenKind::End);
}

TEST(Lexer, HashInsideAStringIsNotAComment) {
  const auto l = lex("CM_ \"pressure # 3\";\n");
  EXPECT_FALSE(l.sink.has_errors());
  EXPECT_EQ(l.tokens[1].value, "pressure # 3");
}

TEST(Lexer, EndTokenIsAlwaysLast) {
  const auto l = lex("BU_: A\n");
  ASSERT_FALSE(l.tokens.empty());
  EXPECT_EQ(l.tokens.back().kind, TokenKind::End);
}

TEST(Diagnostic, RendersWithACaret) {
  Diagnostic d;
  d.severity = Severity::Error;
  d.code = core::ErrorCode::ParseUnexpectedToken;
  d.where = {12, 24};
  d.message = "expected '|' after the start bit";
  d.token = "8";
  d.source_line = " SG_ EngineSpeed : 24 8@1+ (0.125,0) [0|8031] \"rpm\" A";
  d.note = "this is what CANdb++ writes";
  const std::string text = d.format("powertrain.dbc");
  EXPECT_NE(text.find("powertrain.dbc:12:24: error:"), std::string::npos);
  EXPECT_NE(text.find("expected '|' after the start bit"), std::string::npos);
  EXPECT_NE(text.find("(found '8')"), std::string::npos);
  EXPECT_NE(text.find('^'), std::string::npos);
  EXPECT_NE(text.find("= note:"), std::string::npos);

  // The caret must land under column 24 of the quoted source line.
  const std::size_t caret = text.find('^');
  const std::size_t line_start = text.rfind('\n', caret) + 1;
  const std::size_t gutter = text.find("| ", line_start) + 2;
  EXPECT_EQ(caret - gutter, 23u) << "caret should sit at column 24";
}

}  // namespace
}  // namespace canforge::dbc
