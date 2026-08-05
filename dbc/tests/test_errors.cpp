// SPDX-License-Identifier: MIT
//
// One deliberately malformed file per error class. Each case asserts not only
// that the parse fails but that it fails with the right code at the right
// place, and that the rendered diagnostic actually names the problem.

#include "canforge/dbc/Parser.hpp"

#include <gtest/gtest.h>

#include <string>

namespace canforge::dbc {
namespace {

std::string data_path(const char* name) {
  return std::string(CANFORGE_DBC_TEST_DATA) + "/" + name;
}

struct BadCase {
  const char* file;
  core::ErrorCode expected;
  const char* text_fragment;
};

const BadCase kBadCases[] = {
    {"bad_missing_pipe.dbc", core::ErrorCode::ParseUnexpectedToken, "'|'"},
    {"bad_bit_length.dbc", core::ErrorCode::CodecBadBitLength, "1 and 64"},
    {"bad_unterminated_string.dbc", core::ErrorCode::ParseUnterminatedString,
     "never closed"},
    {"bad_unknown_keyword.dbc", core::ErrorCode::ParseUnknownKeyword,
     "unknown section keyword"},
    {"bad_out_of_bounds.dbc", core::ErrorCode::CodecSignalOutOfBounds,
     "past the end"},
    {"bad_identifier.dbc", core::ErrorCode::FrameBadIdentifier, "out of range"},
    {"bad_missing_signedness.dbc", core::ErrorCode::ParseUnexpectedToken,
     "'+' or '-'"},
    {"bad_byte_order.dbc", core::ErrorCode::ParseSemantic, "0 (Motorola)"},
};

TEST(Errors, EachMalformedFileFailsWithItsOwnDiagnostic) {
  for (const BadCase& c : kBadCases) {
    auto parsed = parse_file(data_path(c.file));
    ASSERT_TRUE(parsed.has_value()) << "cannot read " << c.file;
    const ParseResult r = std::move(parsed).value();

    EXPECT_TRUE(r.diagnostics.has_errors()) << c.file << " should not parse";
    bool matched = false;
    for (const Diagnostic& d : r.diagnostics.items()) {
      if (d.severity == Severity::Error && d.code == c.expected) {
        matched = true;
        EXPECT_GT(d.where.line, 0u);
        EXPECT_GT(d.where.column, 0u);
        EXPECT_FALSE(d.message.empty());
      }
    }
    EXPECT_TRUE(matched) << c.file << " produced the wrong error code:\n"
                         << r.diagnostics.format(c.file);

    const std::string rendered = r.diagnostics.format(c.file);
    EXPECT_NE(rendered.find(c.text_fragment), std::string::npos)
        << c.file << " diagnostic does not mention '" << c.text_fragment
        << "':\n"
        << rendered;
    EXPECT_NE(rendered.find(c.file), std::string::npos)
        << "the diagnostic must name the file";
  }
}

TEST(Errors, DiagnosticsPointAtTheOffendingLine) {
  const std::string source =
      "VERSION \"1\"\n"
      "\n"
      "BU_: A\n"
      "\n"
      "BO_ 10 M: 8 A\n"
      " SG_ Broken : 0 8@1+ (1,0) [0|255] \"\" A\n";
  const ParseResult r = parse_string(source, "inline.dbc");
  ASSERT_TRUE(r.diagnostics.has_errors());
  const Diagnostic& d = r.diagnostics.items().front();
  EXPECT_EQ(d.where.line, 6u);
  EXPECT_EQ(d.source_line, " SG_ Broken : 0 8@1+ (1,0) [0|255] \"\" A");

  const std::string text = d.format("inline.dbc");
  EXPECT_NE(text.find("inline.dbc:6:"), std::string::npos) << text;
  EXPECT_NE(text.find(" SG_ Broken :"), std::string::npos) << text;
  EXPECT_NE(text.find('^'), std::string::npos) << text;
}

TEST(Errors, ParsingContinuesPastARecoverableError) {
  // The first message is broken; the second must still be recovered, so that
  // a single typo does not hide the rest of the file.
  const std::string source =
      "VERSION \"1\"\n"
      "BU_: A\n"
      "BO_ 10 Broken: 8 A\n"
      " SG_ Bad : 0 8@1+ (1,0) [0|255] \"\" A\n"
      "BO_ 11 Fine: 8 A\n"
      " SG_ Good : 0|8@1+ (1,0) [0|255] \"\" A\n";
  const ParseResult r = parse_string(source, "recover.dbc");
  EXPECT_TRUE(r.diagnostics.has_errors());
  EXPECT_NE(r.database.find_message("Fine"), nullptr)
      << "the parser gave up after the first error";
  ASSERT_NE(r.database.find_message("Fine")->find_signal("Good"), nullptr);
}

TEST(Errors, SeveralProblemsAreAllReported) {
  const std::string source =
      "VERSION \"1\"\n"
      "BU_: A\n"
      "BO_ 10 M: 8 A\n"
      " SG_ S1 : 0|99@1+ (1,0) [0|255] \"\" A\n"
      " SG_ S2 : 0|8@9+ (1,0) [0|255] \"\" A\n"
      "WOBBLE_ nonsense;\n";
  const ParseResult r = parse_string(source, "many.dbc");
  EXPECT_GE(r.diagnostics.error_count(), 3u)
      << r.diagnostics.format("many.dbc");
}

TEST(Errors, WarningsDoNotFailTheParse) {
  // A comment attached to a signal that does not exist is a warning: the rest
  // of the database is still perfectly usable.
  const std::string source =
      "VERSION \"1\"\n"
      "BU_: A\n"
      "BO_ 10 M: 8 A\n"
      " SG_ S : 0|8@1+ (1,0) [0|255] \"\" A\n"
      "CM_ SG_ 10 NotHere \"orphan\";\n";
  const ParseResult r = parse_string(source, "warn.dbc");
  EXPECT_FALSE(r.diagnostics.has_errors());
  EXPECT_TRUE(r.ok());
  EXPECT_GE(r.diagnostics.warning_count(), 1u);
  EXPECT_NE(r.database.find_message("M"), nullptr);
}

TEST(Errors, LoadReportsTheFirstErrorLocation) {
  const auto db = load(data_path("bad_missing_pipe.dbc"));
  ASSERT_FALSE(db.has_value());
  EXPECT_GT(db.error().detail().a, 0u) << "line number should be carried";
}

TEST(Errors, MissingFileIsAnIoError) {
  const auto r = parse_file(data_path("does_not_exist.dbc"));
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error().code(), core::ErrorCode::ParseIoError);
}

TEST(Errors, DuplicateMessageIdentifierIsCaughtByValidate) {
  const std::string source =
      "VERSION \"1\"\n"
      "BU_: A\n"
      "BO_ 10 First: 8 A\n"
      " SG_ S : 0|8@1+ (1,0) [0|255] \"\" A\n"
      "BO_ 10 Second: 8 A\n"
      " SG_ T : 0|8@1+ (1,0) [0|255] \"\" A\n";
  const ParseResult r = parse_string(source, "dup.dbc");
  const auto st = r.database.validate();
  ASSERT_FALSE(st.has_value());
  EXPECT_EQ(st.error().code(), core::ErrorCode::ParseDuplicateDefinition);
}

TEST(Errors, OverlappingSignalsAreLinted) {
  const std::string source =
      "VERSION \"1\"\n"
      "BU_: A\n"
      "BO_ 10 M: 8 A\n"
      " SG_ S1 : 0|16@1+ (1,0) [0|255] \"\" A\n"
      " SG_ S2 : 8|16@1+ (1,0) [0|255] \"\" A\n";
  const ParseResult r = parse_string(source, "overlap.dbc");
  ASSERT_FALSE(r.diagnostics.has_errors()) << r.diagnostics.format("overlap.dbc");
  const auto problems = r.database.lint();
  bool found = false;
  for (const std::string& p : problems) {
    if (p.find("overlap") != std::string::npos) {
      found = true;
    }
  }
  EXPECT_TRUE(found) << "an overlap between S1 and S2 should be reported";
}

TEST(Errors, ReservedBaseIdentifierIsLintedButAccepted) {
  const std::string source =
      "VERSION \"1\"\n"
      "BU_: A\n"
      "BO_ 2033 Reserved: 1 A\n"
      " SG_ S : 0|8@1+ (1,0) [0|255] \"\" A\n";
  const ParseResult r = parse_string(source, "reserved.dbc");
  EXPECT_FALSE(r.diagnostics.has_errors());
  const auto* m = r.database.find_message("Reserved");
  ASSERT_NE(m, nullptr);
  EXPECT_EQ(m->id().value(), 2033u);
  EXPECT_TRUE(m->id().violates_base_id_rule());
  bool mentioned = false;
  for (const std::string& p : r.database.lint()) {
    if (p.find("ISO 11898-1") != std::string::npos) {
      mentioned = true;
    }
  }
  EXPECT_TRUE(mentioned);
}

}  // namespace
}  // namespace canforge::dbc
