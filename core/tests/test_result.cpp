// SPDX-License-Identifier: MIT
#include "canforge/core/Result.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <utility>

namespace canforge::core {
namespace {

constexpr Error kBoom{ErrorCode::InvalidArgument, "boom", {7, 9}};

TEST(Result, HoldsAValue) {
  Result<int> r{42};
  EXPECT_TRUE(r.has_value());
  EXPECT_TRUE(static_cast<bool>(r));
  EXPECT_EQ(r.value(), 42);
  EXPECT_EQ(*r, 42);
}

TEST(Result, HoldsAnError) {
  Result<int> r{kBoom};
  EXPECT_FALSE(r.has_value());
  EXPECT_FALSE(static_cast<bool>(r));
  EXPECT_EQ(r.error().code(), ErrorCode::InvalidArgument);
  EXPECT_EQ(r.error().message(), "boom");
  EXPECT_EQ(r.error().detail().a, 7u);
  EXPECT_EQ(r.error().detail().b, 9u);
}

TEST(Result, ValueOrFallsBack) {
  EXPECT_EQ(Result<int>{5}.value_or(99), 5);
  EXPECT_EQ(Result<int>{kBoom}.value_or(99), 99);
}

TEST(Result, ErrorIsTriviallyCopyableAndSmall) {
  // These two properties are what let a Result cross the real-time boundary.
  EXPECT_TRUE(std::is_trivially_copyable_v<Error>);
  EXPECT_LE(sizeof(Error), 32u);
}

TEST(Result, ManagesNonTrivialValues) {
  Result<std::string> r{std::string(200, 'x')};  // definitely heap allocated
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(r.value().size(), 200u);

  Result<std::string> copy = r;
  ASSERT_TRUE(copy.has_value());
  EXPECT_EQ(copy.value().size(), 200u);

  Result<std::string> moved = std::move(r);
  ASSERT_TRUE(moved.has_value());
  EXPECT_EQ(moved.value().size(), 200u);

  moved = Result<std::string>{kBoom};
  EXPECT_FALSE(moved.has_value());
}

TEST(Result, MoveOnlyValues) {
  Result<std::unique_ptr<int>> r{std::make_unique<int>(3)};
  ASSERT_TRUE(r.has_value());
  auto p = std::move(r).value();
  EXPECT_EQ(*p, 3);
}

TEST(Result, AssignmentSwitchesState) {
  Result<std::string> r{std::string("hello")};
  r = Result<std::string>{kBoom};
  EXPECT_FALSE(r.has_value());
  r = Result<std::string>{std::string("world")};
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(r.value(), "world");
}

TEST(Result, VoidSpecialisation) {
  Status good = ok();
  EXPECT_TRUE(good.has_value());
  Status bad{kBoom};
  EXPECT_FALSE(bad.has_value());
  EXPECT_EQ(bad.error().code(), ErrorCode::InvalidArgument);
}

TEST(Result, MapTransformsOnlySuccess) {
  auto twice = [](int v) { return v * 2; };
  EXPECT_EQ(Result<int>{21}.map(twice).value(), 42);
  EXPECT_FALSE(Result<int>{kBoom}.map(twice).has_value());
}

TEST(Result, AndThenChains) {
  auto half = [](int v) -> Result<int> {
    if (v % 2 != 0) {
      return Error{ErrorCode::InvalidArgument, "odd"};
    }
    return v / 2;
  };
  EXPECT_EQ(Result<int>{10}.and_then(half).value(), 5);
  EXPECT_EQ(Result<int>{11}.and_then(half).error().message(), "odd");
  EXPECT_EQ(Result<int>{kBoom}.and_then(half).error().code(),
            ErrorCode::InvalidArgument);
}

Result<int> parse_positive(int v) {
  if (v <= 0) {
    return Error{
        ErrorCode::OutOfRange, "not positive", {static_cast<std::uint32_t>(-v), 0}};
  }
  return v;
}

Result<int> doubled_positive(int v) {
  CANFORGE_TRY(const auto n, parse_positive(v));
  return n * 2;
}

Status check_positive(int v) {
  CANFORGE_TRY(const auto n, parse_positive(v));
  static_cast<void>(n);
  return ok();
}

TEST(Result, TryMacroPropagates) {
  EXPECT_EQ(doubled_positive(4).value(), 8);
  EXPECT_EQ(doubled_positive(-1).error().code(), ErrorCode::OutOfRange);
  EXPECT_TRUE(check_positive(1).has_value());
  EXPECT_FALSE(check_positive(0).has_value());
}

Status always_fails() {
  return Error{ErrorCode::Unsupported, "nope"};
}

Status forwards() {
  CANFORGE_CHECK(always_fails());
  return ok();
}

TEST(Result, CheckMacroPropagates) {
  EXPECT_EQ(forwards().error().code(), ErrorCode::Unsupported);
}

TEST(Result, ErrorCodesAllHaveText) {
  // Every enumerator must have a string; the switch in to_string is
  // -Werror=switch protected, this catches a missing `case` that returns the
  // fallback by accident.
  for (std::uint16_t i = 0; i <= static_cast<std::uint16_t>(ErrorCode::LogEndOfFile);
       ++i) {
    EXPECT_STRNE(to_string(static_cast<ErrorCode>(i)), "unknown error")
        << "missing text for error code " << i;
  }
}

TEST(ResultDeathTest, ValueOnErrorAborts) {
  Result<int> r{kBoom};
  EXPECT_DEATH(static_cast<void>(r.value()), "error result");
}

TEST(ResultDeathTest, ErrorOnValueAborts) {
  Result<int> r{1};
  EXPECT_DEATH(static_cast<void>(r.error()), "success result");
}

}  // namespace
}  // namespace canforge::core
