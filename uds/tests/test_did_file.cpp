// SPDX-License-Identifier: MIT
//
// DID files: the JSON reader, the checks on what a file says, and the decoder.

#include "canforge/uds/DidFile.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace canforge::uds {
namespace {

const char* const kExample = R"json(
{
  "byte_order": "big",
  "dids": [
    { "did": "0100", "name": "Temperatures", "length": 6,
      "fields": [
        { "name": "coolant", "offset": 0, "size": 2, "type": "int",
          "factor": 0.1, "offset_value": 0, "unit": "degC", "invalid": 32767 },
        { "name": "mode", "offset": 2, "size": 1,
          "values": { "0": "INIT", "1": "NORMAL", "2": "SAFE" } },
        { "name": "pump", "offset": 3, "size": 1, "unit": "%" },
        { "name": "tag", "offset": 4, "size": 2, "type": "hex" } ] },
    { "did": "F189", "name": "Software version", "length": 8,
      "fields": [ { "name": "version", "offset": 0, "size": 8, "type": "ascii" } ] },
    { "did": "f1b0", "name": "Statistics", "length": 6,
      "fields": [
        { "name": "uptime", "offset": 0, "size": 4, "unit": "s" },
        { "name": "raw target", "offset": 4, "size": 1, "factor": 0.5, "unit": "degC" },
        { "name": "shifted", "offset": 5, "size": 1, "factor": 0.5, "offset_value": -40 } ] }
  ]
}
)json";

DidCatalog example() {
  auto parsed = DidCatalog::parse(kExample);
  EXPECT_TRUE(parsed.has_value())
      << (parsed.has_value() ? "" : std::string(parsed.error().message()));
  return parsed ? parsed.value() : DidCatalog{};
}

std::vector<DecodedField> decode(const DidCatalog& catalog, std::uint16_t did,
                                 const std::vector<std::uint8_t>& bytes) {
  const DidEntry* entry = catalog.find(did);
  EXPECT_NE(entry, nullptr);
  if (entry == nullptr) {
    return {};
  }
  auto decoded = catalog.decode(*entry, bytes.data(), bytes.size());
  EXPECT_TRUE(decoded.has_value());
  return decoded ? decoded.value() : std::vector<DecodedField>{};
}

core::Error error_of(const std::string& json) {
  const auto parsed = DidCatalog::parse(json);
  EXPECT_FALSE(parsed.has_value()) << json;
  return parsed ? core::Error{} : parsed.error();
}

// Reading the file

TEST(DidFile, ReadsEntriesAndFields) {
  const DidCatalog catalog = example();
  ASSERT_EQ(catalog.entries().size(), 3u);
  EXPECT_FALSE(catalog.little_endian());

  const DidEntry* temps = catalog.find(0x0100);
  ASSERT_NE(temps, nullptr);
  EXPECT_EQ(temps->name, "Temperatures");
  EXPECT_EQ(temps->length, 6u);
  ASSERT_EQ(temps->fields.size(), 4u);
  EXPECT_EQ(temps->fields[0].name, "coolant");
  EXPECT_EQ(temps->fields[0].type, DidFieldType::Signed);
  EXPECT_DOUBLE_EQ(temps->fields[0].factor, 0.1);
  EXPECT_EQ(temps->fields[0].unit, "degC");
  EXPECT_TRUE(temps->fields[0].has_invalid);
  EXPECT_EQ(temps->fields[0].invalid, 32767);
  EXPECT_EQ(temps->fields[1].type, DidFieldType::Unsigned) << "the default";
  EXPECT_EQ(temps->fields[1].values.at(2), "SAFE");

  EXPECT_NE(catalog.find(0xF1B0), nullptr) << "hex digits in either case";
  EXPECT_EQ(catalog.find(0x1234), nullptr);
}

TEST(DidFile, UnknownMembersAreIgnored) {
  // A newer file with keys this version does not know must still load.
  const auto parsed = DidCatalog::parse(R"({
    "format": "canforge-did-1", "comment": ["anything", {"nested": true}, null, 1.5e3],
    "dids": [ { "did": "0001", "length": 1, "since": "2.0",
                "fields": [ { "name": "x", "offset": 0, "size": 1, "colour": "red" } ] } ] })");
  ASSERT_TRUE(parsed.has_value());
  EXPECT_EQ(parsed.value().entries().size(), 1u);
  EXPECT_TRUE(parsed.value().entries()[0].name.empty()) << "a name is optional";
}

TEST(DidFile, StringEscapesAreDecoded) {
  const auto parsed = DidCatalog::parse(R"({ "dids": [ { "did": "0001", "length": 1,
    "name": "a \"quoted\" \\ name\twith escapes",
    "fields": [ { "name": "t", "offset": 0, "size": 1, "unit": "°C" } ] } ] })");
  ASSERT_TRUE(parsed.has_value());
  EXPECT_EQ(parsed.value().entries()[0].name, "a \"quoted\" \\ name\twith escapes");
  EXPECT_EQ(parsed.value().entries()[0].fields[0].unit,
            "\xC2\xB0"
            "C");
}

TEST(DidFile, ADidMayBeANumber) {
  const auto parsed = DidCatalog::parse(R"({"dids":[{"did":256,"length":1}]})");
  ASSERT_TRUE(parsed.has_value());
  EXPECT_NE(parsed.value().find(0x0100), nullptr);
}

TEST(DidFile, AnEmptyCatalogIsValid) {
  const auto parsed = DidCatalog::parse(R"({"dids":[]})");
  ASSERT_TRUE(parsed.has_value());
  EXPECT_TRUE(parsed.value().entries().empty());
}

TEST(DidFile, AMissingFileIsAnIoError) {
  const auto loaded = DidCatalog::load("/nonexistent/dids.json");
  ASSERT_FALSE(loaded.has_value());
  EXPECT_EQ(loaded.error().code(), core::ErrorCode::ParseIoError);
}

// Syntax errors, with the place they were found

TEST(DidFile, SyntaxErrorsNameTheOffset) {
  EXPECT_EQ(error_of("").code(), core::ErrorCode::ParseUnexpectedEof);
  EXPECT_EQ(error_of(R"({"dids": [)").code(), core::ErrorCode::ParseUnexpectedEof);
  EXPECT_EQ(error_of(R"({"dids": "unterminated)").code(),
            core::ErrorCode::ParseUnterminatedString);
  EXPECT_EQ(error_of(R"({"dids": [1, 2,]})").code(), core::ErrorCode::ParseBadNumber);
  EXPECT_EQ(error_of(R"({"dids": [] } trailing)").code(),
            core::ErrorCode::ParseUnexpectedToken);
  EXPECT_EQ(error_of(R"({"dids" [] })").code(), core::ErrorCode::ParseUnexpectedToken);
  EXPECT_EQ(error_of(R"({dids: [] })").code(), core::ErrorCode::ParseUnexpectedToken);
  EXPECT_EQ(error_of(R"({"dids": [tru] })").code(), core::ErrorCode::ParseBadNumber);
  EXPECT_EQ(error_of(R"({"dids": ["\x"] })").code(), core::ErrorCode::ParseBadEncoding);
  EXPECT_EQ(error_of(R"({"dids": ["\u12G4"] })").code(),
            core::ErrorCode::ParseBadEncoding);
  EXPECT_EQ(error_of("{\"dids\": [\"a\nb\"] }").code(),
            core::ErrorCode::ParseBadEncoding);

  const core::Error e = error_of(R"({"dids": [1 2]})");
  EXPECT_EQ(e.code(), core::ErrorCode::ParseUnexpectedToken);
  EXPECT_EQ(e.detail().a, 12u) << "the byte where ',' or ']' was expected";
}

TEST(DidFile, DeepNestingIsRefusedNotCrashedOn) {
  const std::string deep(100000, '[');
  EXPECT_EQ(error_of(deep).code(), core::ErrorCode::ParseSemantic);
}

// Errors in what the file says

TEST(DidFile, SemanticErrorsNameTheDid) {
  EXPECT_EQ(error_of(R"([])").code(), core::ErrorCode::ParseSemantic);
  EXPECT_EQ(error_of(R"({})").code(), core::ErrorCode::ParseSemantic);
  EXPECT_EQ(error_of(R"({"dids": {}})").code(), core::ErrorCode::ParseSemantic);
  EXPECT_EQ(error_of(R"({"byte_order": "middle", "dids": []})").code(),
            core::ErrorCode::ParseSemantic);
  EXPECT_EQ(error_of(R"({"dids": [{"length": 1}]})").code(),
            core::ErrorCode::ParseSemantic);
  EXPECT_EQ(error_of(R"({"dids": [{"did": "F18G", "length": 1}]})").code(),
            core::ErrorCode::ParseSemantic);
  EXPECT_EQ(error_of(R"({"dids": [{"did": "12345", "length": 1}]})").code(),
            core::ErrorCode::ParseSemantic);

  const auto field_error = [](const std::string& field) {
    return error_of(R"({"dids": [{"did": "0200", "length": 4, "fields": [)" + field +
                    "]}]}");
  };
  const core::Error past_end = field_error(R"({"name": "x", "offset": 3, "size": 2})");
  EXPECT_EQ(past_end.code(), core::ErrorCode::ParseSemantic);
  EXPECT_EQ(past_end.detail().a, 0x0200u);
  EXPECT_EQ(field_error(R"({"offset": 0, "size": 1})").code(),
            core::ErrorCode::ParseSemantic);
  EXPECT_EQ(field_error(R"({"name": "x", "offset": 0})").code(),
            core::ErrorCode::ParseSemantic);
  EXPECT_EQ(field_error(R"({"name": "x", "offset": 0, "size": 0})").code(),
            core::ErrorCode::ParseSemantic);
  EXPECT_EQ(field_error(R"({"name": "x", "offset": 0.5, "size": 1})").code(),
            core::ErrorCode::ParseSemantic);
  EXPECT_EQ(
      field_error(R"({"name": "x", "offset": 0, "size": 1, "type": "float"})").code(),
      core::ErrorCode::ParseSemantic);
  EXPECT_EQ(field_error(R"({"name": "x", "offset": 0, "size": 1, "factor": 0})").code(),
            core::ErrorCode::ParseSemantic);
  EXPECT_EQ(
      field_error(R"({"name": "x", "offset": 0, "size": 1, "values": {"a": "b"}})")
          .code(),
      core::ErrorCode::ParseSemantic);
  EXPECT_EQ(error_of(R"({"dids": [{"did": "0001", "length": 9,
              "fields": [{"name": "x", "offset": 0, "size": 9}]}]})")
                .code(),
            core::ErrorCode::ParseSemantic)
      << "a nine-byte number";
}

TEST(DidFile, TheSameDidTwiceIsRefused) {
  const core::Error e = error_of(
      R"({"dids": [{"did": "F190", "length": 1}, {"did": "f190", "length": 2}]})");
  EXPECT_EQ(e.code(), core::ErrorCode::ParseDuplicateDefinition);
  EXPECT_EQ(e.detail().a, 0xF190u);
}

// Decoding

TEST(DidFile, DecodesScaledSignedValues) {
  const DidCatalog catalog = example();
  auto fields = decode(catalog, 0x0100, {0x01, 0x11, 0x02, 0x4D, 0xAB, 0xCD});
  ASSERT_EQ(fields.size(), 4u);
  EXPECT_EQ(fields[0].name, "coolant");
  EXPECT_EQ(fields[0].text, "27.3 degC");  // 0x0111 = 273
  EXPECT_TRUE(fields[0].numeric);
  EXPECT_DOUBLE_EQ(fields[0].value, 27.3);
  EXPECT_EQ(fields[0].raw, 273);
  EXPECT_EQ(fields[1].text, "SAFE");
  EXPECT_EQ(fields[2].text, "77 %");
  EXPECT_EQ(fields[3].text, "AB CD");
  EXPECT_FALSE(fields[3].numeric);

  fields = decode(catalog, 0x0100, {0xFF, 0x83, 0x07, 0x00, 0x00, 0x00});
  EXPECT_EQ(fields[0].text, "-12.5 degC");  // 0xFF83 = -125
  EXPECT_EQ(fields[1].text, "7") << "a value without a label is shown as the number";
}

TEST(DidFile, TheInvalidMarkerIsShownAsSuch) {
  const auto fields = decode(example(), 0x0100, {0x7F, 0xFF, 0x00, 0x00, 0x00, 0x00});
  EXPECT_EQ(fields[0].text, "invalid");
  EXPECT_FALSE(fields[0].valid);
  EXPECT_EQ(fields[1].text, "INIT");
}

TEST(DidFile, DecodesAsciiWithoutItsPadding) {
  auto fields = decode(example(), 0xF189, {'1', '.', '2', '.', '3', 0, 0, 0});
  ASSERT_EQ(fields.size(), 1u);
  EXPECT_EQ(fields[0].text, "1.2.3");
  fields = decode(example(), 0xF189, {'A', 0x01, 'B', 0xFF, 'C', 'D', 'E', 'F'});
  EXPECT_EQ(fields[0].text, "A.B.CDEF") << "unprintable bytes become dots";
}

TEST(DidFile, DecodesWideValuesFactorsAndOffsets) {
  const auto fields = decode(example(), 0xF1B0, {0x00, 0x01, 0x51, 0x80, 45, 100});
  ASSERT_EQ(fields.size(), 3u);
  EXPECT_EQ(fields[0].text, "86400 s");  // 0x00015180
  EXPECT_EQ(fields[1].text, "22.5 degC");
  EXPECT_EQ(fields[2].text, "10.0") << "100 * 0.5 - 40, no unit";
}

TEST(DidFile, LittleEndianWhenTheFileSaysSo) {
  const auto parsed = DidCatalog::parse(R"({"byte_order": "little", "dids": [
    {"did": "0001", "length": 4, "fields": [
      {"name": "u", "offset": 0, "size": 2},
      {"name": "s", "offset": 2, "size": 2, "type": "int"}]}]})");
  ASSERT_TRUE(parsed.has_value());
  EXPECT_TRUE(parsed.value().little_endian());
  const auto fields = decode(parsed.value(), 0x0001, {0x34, 0x12, 0xFE, 0xFF});
  EXPECT_EQ(fields[0].text, "4660");  // 0x1234
  EXPECT_EQ(fields[1].text, "-2");
}

TEST(DidFile, EightByteValuesKeepEveryBit) {
  const auto parsed = DidCatalog::parse(R"({"dids": [
    {"did": "0001", "length": 16, "fields": [
      {"name": "u", "offset": 0, "size": 8},
      {"name": "s", "offset": 8, "size": 8, "type": "int"}]}]})");
  ASSERT_TRUE(parsed.has_value());
  const auto fields = decode(
      parsed.value(), 0x0001,
      {0, 0, 0, 0, 0, 0, 0x01, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFE});
  EXPECT_EQ(fields[0].raw, 256);
  EXPECT_EQ(fields[1].raw, -2);
}

TEST(DidFile, AShortResponseIsAnError) {
  const DidCatalog catalog = example();
  const DidEntry* entry = catalog.find(0x0100);
  ASSERT_NE(entry, nullptr);
  const std::uint8_t five[5] = {};
  const auto decoded = catalog.decode(*entry, five, sizeof(five));
  ASSERT_FALSE(decoded.has_value());
  EXPECT_EQ(decoded.error().code(), core::ErrorCode::OutOfRange);
  EXPECT_EQ(decoded.error().detail().a, 5u);
  EXPECT_EQ(decoded.error().detail().b, 6u);

  const std::uint8_t seven[7] = {};
  EXPECT_TRUE(catalog.decode(*entry, seven, sizeof(seven)).has_value())
      << "extra bytes after the described part are not this decoder's concern";
}

}  // namespace
}  // namespace canforge::uds
