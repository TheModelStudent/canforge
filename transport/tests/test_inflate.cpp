// SPDX-License-Identifier: MIT
#include "../src/Inflate.hpp"

#include <gtest/gtest.h>

#include <fstream>
#include <string>
#include <vector>

namespace canforge::transport::detail {
namespace {

std::vector<std::uint8_t> read_file(const std::string& name) {
  const std::string path = std::string(CANFORGE_TRANSPORT_TEST_DATA) + "/" + name;
  std::ifstream in(path, std::ios::binary);
  EXPECT_TRUE(in.good()) << "missing fixture " << path;
  return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(in)),
                                   std::istreambuf_iterator<char>());
}

// The fixtures were produced with Python's zlib at compression levels 0, 1, 6
// and 9, which between them exercise stored blocks, fixed Huffman blocks and
// dynamic Huffman blocks.
const char* const kCases[] = {"stored", "fixed", "dynamic", "text"};

TEST(Inflate, RoundTripsEveryBlockType) {
  for (const char* name : kCases) {
    const auto compressed = read_file(std::string("inflate/") + name + ".z");
    const auto expected = read_file(std::string("inflate/") + name + ".raw");
    ASSERT_FALSE(compressed.empty()) << name;

    auto out = inflate_zlib(compressed.data(), compressed.size(), expected.size());
    ASSERT_TRUE(out.has_value())
        << name << ": " << out.error().message();
    EXPECT_EQ(out.value(), expected) << name;
  }
}

TEST(Inflate, WorksWithoutASizeHint) {
  const auto compressed = read_file("inflate/text.z");
  const auto expected = read_file("inflate/text.raw");
  auto out = inflate_zlib(compressed.data(), compressed.size());
  ASSERT_TRUE(out.has_value());
  EXPECT_EQ(out.value(), expected);
}

TEST(Inflate, RejectsATruncatedStream) {
  auto compressed = read_file("inflate/dynamic.z");
  compressed.resize(compressed.size() / 2);
  const auto out = inflate_zlib(compressed.data(), compressed.size());
  EXPECT_FALSE(out.has_value());
}

TEST(Inflate, RejectsABadHeader) {
  auto compressed = read_file("inflate/text.z");
  compressed[0] = 0x77;  // compression method is no longer deflate
  const auto out = inflate_zlib(compressed.data(), compressed.size());
  ASSERT_FALSE(out.has_value());
  EXPECT_EQ(out.error().code(), core::ErrorCode::LogBadFormat);
}

TEST(Inflate, DetectsACorruptedPayload) {
  auto compressed = read_file("inflate/text.z");
  ASSERT_GT(compressed.size(), 20u);
  compressed[compressed.size() / 2] ^= 0xFFu;
  const auto out = inflate_zlib(compressed.data(), compressed.size());
  // Either the bit stream itself fails, or it decodes to something whose
  // Adler-32 does not match. Both are caught; neither may read out of bounds,
  // which is what the sanitiser build checks.
  EXPECT_FALSE(out.has_value());
}

TEST(Inflate, RejectsAnEmptyInput) {
  const std::uint8_t nothing = 0;
  EXPECT_FALSE(inflate_zlib(&nothing, 0).has_value());
}

TEST(Inflate, Adler32MatchesTheKnownVector) {
  // RFC 1950's worked example.
  const std::string data = "Wikipedia";
  EXPECT_EQ(adler32(reinterpret_cast<const std::uint8_t*>(data.data()),
                    data.size()),
            0x11E60398u);
  EXPECT_EQ(adler32(nullptr, 0), 1u);
}

}  // namespace
}  // namespace canforge::transport::detail
