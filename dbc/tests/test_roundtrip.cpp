// SPDX-License-Identifier: MIT
//
// Round-trip: parse -> write -> parse must produce an identical Database.
//
// Comparing databases instead of text is on purpose: a byte diff would fail
// on whitespace and ordering that no writer should reproduce, and would still
// pass if the parser dropped a field the writer also never emits.

#include "canforge/dbc/Parser.hpp"
#include "canforge/dbc/Writer.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace canforge::dbc {
namespace {

std::string data_path(const char* name) {
  return std::string(CANFORGE_DBC_TEST_DATA) + "/" + name;
}

const char* const kCorpus[] = {"powertrain.dbc", "multiplexed.dbc", "valuetables.dbc",
                               "floats.dbc",     "attributes.dbc",  "quirks.dbc",
                               "bom.dbc"};

TEST(RoundTrip, ParseWriteParseIsIdentical) {
  for (const char* name : kCorpus) {
    auto first = parse_file(data_path(name));
    ASSERT_TRUE(first.has_value()) << name;
    ParseResult a = std::move(first).value();
    ASSERT_FALSE(a.diagnostics.has_errors()) << name << '\n'
                                             << a.diagnostics.format(name);

    const std::string text = write_string(a.database);
    ParseResult b = parse_string(text, std::string(name) + " (rewritten)");
    ASSERT_FALSE(b.diagnostics.has_errors())
        << "the writer produced something the parser rejects, for " << name << '\n'
        << b.diagnostics.format(name) << "\n---- output ----\n"
        << text;

    EXPECT_EQ(a.database, b.database) << "round trip lost information for " << name;
  }
}

TEST(RoundTrip, IsStableUnderRepetition) {
  // A second pass must be byte-identical to the first: the writer's output is
  // already in its own canonical form.
  for (const char* name : kCorpus) {
    ParseResult a = std::move(parse_file(data_path(name))).value();
    const std::string once = write_string(a.database);
    ParseResult b = parse_string(once, name);
    const std::string twice = write_string(b.database);
    EXPECT_EQ(once, twice) << "writer output is not a fixed point for " << name;
  }
}

TEST(RoundTrip, CrlfOutputParsesBackTheSame) {
  ParseResult a = std::move(parse_file(data_path("powertrain.dbc"))).value();
  WriteOptions opt;
  opt.crlf = true;
  const std::string text = write_string(a.database, opt);
  EXPECT_NE(text.find("\r\n"), std::string::npos);
  ParseResult b = parse_string(text, "crlf");
  EXPECT_TRUE(b.had_crlf);
  EXPECT_FALSE(b.diagnostics.has_errors());
  EXPECT_EQ(a.database, b.database);
}

TEST(RoundTrip, NumbersSurviveExactly) {
  // A factor of 0.1 is not representable in binary; the writer must emit a
  // decimal string that reads back as the same double, not a rounded one.
  const std::vector<double> awkward = {0.1,         0.125,
                                       1.0 / 256.0, 1e-9,
                                       -0.05,       3.3333333333333335,
                                       1e15,        2.220446049250313e-16,
                                       0.0,         -0.0};
  for (const double v : awkward) {
    const std::string text = format_number(v);
    EXPECT_DOUBLE_EQ(std::strtod(text.c_str(), nullptr), v)
        << "format_number(" << v << ") produced '" << text << "'";
  }
  EXPECT_EQ(format_number(1.0), "1");
  EXPECT_EQ(format_number(-3.0), "-3");
  EXPECT_EQ(format_number(0.125), "0.125");
  EXPECT_EQ(format_number(0.1), "0.1");
}

TEST(RoundTrip, FactorsAndOffsetsSurvive) {
  ParseResult a = std::move(parse_file(data_path("powertrain.dbc"))).value();
  ParseResult b = parse_string(write_string(a.database), "rewritten");
  const auto* wheel_a = a.database.find_message("WheelSpeeds");
  const auto* wheel_b = b.database.find_message("WheelSpeeds");
  ASSERT_NE(wheel_a, nullptr);
  ASSERT_NE(wheel_b, nullptr);
  const auto* s_a = wheel_a->find_signal("WheelBasedSpeed");
  const auto* s_b = wheel_b->find_signal("WheelBasedSpeed");
  ASSERT_NE(s_a, nullptr);
  ASSERT_NE(s_b, nullptr);
  EXPECT_DOUBLE_EQ(s_a->layout().factor, 0.00390625);
  EXPECT_DOUBLE_EQ(s_b->layout().factor, s_a->layout().factor);
}

TEST(RoundTrip, ExtendedIdentifiersKeepTheirFlag) {
  ParseResult a = std::move(parse_file(data_path("powertrain.dbc"))).value();
  const std::string text = write_string(a.database);
  // 0x18FEF100 | 0x80000000 = 2566844672
  EXPECT_NE(text.find("BO_ 2566844672 WheelSpeeds"), std::string::npos) << text;
  ParseResult b = parse_string(text, "rewritten");
  const auto* m = b.database.find_message("WheelSpeeds");
  ASSERT_NE(m, nullptr);
  EXPECT_TRUE(m->id().is_extended());
  EXPECT_EQ(m->id().value(), 0x18FEF100u);
}

TEST(RoundTrip, MultiplexIndicatorsAreRewrittenCorrectly) {
  ParseResult a = std::move(parse_file(data_path("multiplexed.dbc"))).value();
  const std::string text = write_string(a.database);
  EXPECT_NE(text.find(" SG_ ResponseKind M :"), std::string::npos) << text;
  EXPECT_NE(text.find(" SG_ VoltageA m1 :"), std::string::npos) << text;
  EXPECT_NE(text.find(" SG_ InnerMux m1M :"), std::string::npos) << text;
  EXPECT_NE(text.find("SG_MUL_VAL_ 1026 HighGroup Selector 10-20, 30-30;"),
            std::string::npos)
      << text;
}

TEST(RoundTrip, EnumAttributesUseNamesInDefaultsAndIndicesInValues) {
  ParseResult a = std::move(parse_file(data_path("attributes.dbc"))).value();
  const std::string text = write_string(a.database);
  EXPECT_NE(text.find("BA_DEF_DEF_ \"DbEnum\" \"Beta\";"), std::string::npos) << text;
  EXPECT_NE(text.find("BA_ \"DbEnum\" 2;"), std::string::npos) << text;
}

TEST(RoundTrip, FloatValueTypesAreReEmitted) {
  ParseResult a = std::move(parse_file(data_path("floats.dbc"))).value();
  const std::string text = write_string(a.database);
  EXPECT_NE(text.find("SIG_VALTYPE_ 300 PressureIntel : 1;"), std::string::npos)
      << text;
  EXPECT_NE(text.find("SIG_VALTYPE_ 301 PreciseValue : 2;"), std::string::npos) << text;
}

TEST(RoundTrip, EscapesSurviveTheTrip) {
  const std::string source =
      "VERSION \"1\"\n\nNS_ : \n\nBS_:\n\nBU_: A\n\n"
      "BO_ 10 M: 1 A\n SG_ S : 0|8@1+ (1,0) [0|255] \"\" A\n\n"
      "CM_ BO_ 10 \"quote \\\" and backslash \\\\ and path C:\\\\temp\";\n";
  ParseResult a = parse_string(source, "escapes");
  ASSERT_FALSE(a.diagnostics.has_errors()) << a.diagnostics.format("escapes");
  ParseResult b = parse_string(write_string(a.database), "rewritten");
  ASSERT_FALSE(b.diagnostics.has_errors());
  EXPECT_EQ(a.database.find_message("M")->comment(),
            b.database.find_message("M")->comment());
  EXPECT_NE(a.database.find_message("M")->comment().find('"'), std::string::npos);
}

TEST(RoundTrip, EmptyDatabaseIsWritable) {
  core::Database db;
  const std::string text = write_string(db);
  ParseResult r = parse_string(text, "empty");
  EXPECT_FALSE(r.diagnostics.has_errors()) << r.diagnostics.format("empty");
  EXPECT_TRUE(r.database.messages().empty());
  EXPECT_TRUE(r.database.nodes().empty());
}

}  // namespace
}  // namespace canforge::dbc
