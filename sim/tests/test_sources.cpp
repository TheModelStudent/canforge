// SPDX-License-Identifier: MIT
#include "canforge/sim/SignalSource.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <vector>

namespace canforge::sim {
namespace {

constexpr std::uint64_t kS = 1000000000ULL;

TEST(Sources, Constant) {
  auto s = constant(42.5);
  EXPECT_DOUBLE_EQ(s->sample(0), 42.5);
  EXPECT_DOUBLE_EQ(s->sample(kS * 1000), 42.5);
}

TEST(Sources, RampClamps) {
  auto s = ramp(0.0, 100.0, 10 * kS, RampMode::Clamp);
  EXPECT_DOUBLE_EQ(s->sample(0), 0.0);
  EXPECT_DOUBLE_EQ(s->sample(5 * kS), 50.0);
  EXPECT_DOUBLE_EQ(s->sample(10 * kS), 100.0);
  EXPECT_DOUBLE_EQ(s->sample(50 * kS), 100.0) << "clamp holds the end value";
}

TEST(Sources, RampRepeatsAndPingPongs) {
  auto rep = ramp(0.0, 10.0, 10 * kS, RampMode::Repeat);
  EXPECT_DOUBLE_EQ(rep->sample(5 * kS), 5.0);
  EXPECT_DOUBLE_EQ(rep->sample(15 * kS), 5.0) << "second cycle repeats";

  auto pp = ramp(0.0, 10.0, 10 * kS, RampMode::PingPong);
  EXPECT_DOUBLE_EQ(pp->sample(5 * kS), 5.0);
  EXPECT_DOUBLE_EQ(pp->sample(15 * kS), 5.0) << "reversed half way back down";
  EXPECT_NEAR(pp->sample(11 * kS), 9.0, 1e-9);
}

TEST(Sources, Sine) {
  auto s = sine(10.0, 1.0, 100.0, 0.0);
  EXPECT_NEAR(s->sample(0), 100.0, 1e-9);
  EXPECT_NEAR(s->sample(kS / 4), 110.0, 1e-9);
  EXPECT_NEAR(s->sample(kS / 2), 100.0, 1e-6);
  EXPECT_NEAR(s->sample(3 * kS / 4), 90.0, 1e-9);
}

TEST(Sources, Square) {
  auto s = square(0.0, 5.0, kS, 0.25);
  EXPECT_DOUBLE_EQ(s->sample(0), 5.0);
  EXPECT_DOUBLE_EQ(s->sample(kS / 10), 5.0);
  EXPECT_DOUBLE_EQ(s->sample(kS / 2), 0.0);
  EXPECT_DOUBLE_EQ(s->sample(kS + kS / 10), 5.0) << "periodic";
}

TEST(Sources, RandomWalkIsSeededAndBounded) {
  auto a = random_walk(50.0, 5.0, 0.0, 100.0, 1234, kS / 10);
  auto b = random_walk(50.0, 5.0, 0.0, 100.0, 1234, kS / 10);
  for (std::uint64_t t = 0; t < 20 * kS; t += kS / 10) {
    const double va = a->sample(t);
    EXPECT_DOUBLE_EQ(va, b->sample(t)) << "same seed must give the same walk";
    EXPECT_GE(va, 0.0);
    EXPECT_LE(va, 100.0);
  }
  auto c = random_walk(50.0, 5.0, 0.0, 100.0, 9999, kS / 10);
  EXPECT_NE(a->sample(20 * kS), c->sample(20 * kS));
}

TEST(Sources, RandomWalkDoesNotDependOnStepSize) {
  // Sampling every 10 ms and every 1 s must agree at the shared instants,
  // because the walk advances on its own interval rather than per call.
  auto fine = random_walk(0.0, 1.0, -100.0, 100.0, 7, kS);
  auto coarse = random_walk(0.0, 1.0, -100.0, 100.0, 7, kS);
  for (std::uint64_t t = 0; t <= 10 * kS; t += kS / 100) {
    fine->sample(t);
  }
  EXPECT_DOUBLE_EQ(fine->sample(10 * kS), coarse->sample(10 * kS));
}

TEST(Sources, KeyframesInterpolate) {
  auto s = keyframes({{0, 0.0}, {10 * kS, 100.0}, {20 * kS, 50.0}}, true);
  EXPECT_DOUBLE_EQ(s->sample(0), 0.0);
  EXPECT_DOUBLE_EQ(s->sample(5 * kS), 50.0);
  EXPECT_DOUBLE_EQ(s->sample(10 * kS), 100.0);
  EXPECT_DOUBLE_EQ(s->sample(15 * kS), 75.0);
  EXPECT_DOUBLE_EQ(s->sample(99 * kS), 50.0) << "held past the last frame";
}

TEST(Sources, KeyframesAsSteps) {
  auto s = keyframes({{0, 0.0}, {10 * kS, 100.0}}, false);
  EXPECT_DOUBLE_EQ(s->sample(9 * kS), 0.0);
  EXPECT_DOUBLE_EQ(s->sample(10 * kS), 100.0);
}

TEST(Sources, Compose) {
  std::vector<SourcePtr> parts;
  parts.push_back(constant(10.0));
  parts.push_back(sine(1.0, 1.0, 0.0, 90.0));  // cos, so 1.0 at t=0
  auto s = sum(std::move(parts));
  EXPECT_NEAR(s->sample(0), 11.0, 1e-9);

  auto scaled = scale(constant(3.0), 2.0, 1.0);
  EXPECT_DOUBLE_EQ(scaled->sample(0), 7.0);

  auto clamped = clamp_to(ramp(0.0, 100.0, 10 * kS), 20.0, 80.0);
  EXPECT_DOUBLE_EQ(clamped->sample(0), 20.0);
  EXPECT_DOUBLE_EQ(clamped->sample(10 * kS), 80.0);
  EXPECT_DOUBLE_EQ(clamped->sample(5 * kS), 50.0);
}

TEST(Clock, VirtualClockAdvances) {
  VirtualClock clock;
  EXPECT_EQ(clock.now_ns(), 0u);
  clock.advance(kS);
  EXPECT_EQ(clock.now_ns(), kS);
  clock.set(42);
  EXPECT_EQ(clock.now_ns(), 42u);
}

TEST(Clock, SteadyClockStartsNearZeroAndMovesForward) {
  SteadyClock clock;
  const std::uint64_t a = clock.now_ns();
  EXPECT_LT(a, 1000000000ULL) << "the clock is rebased to its construction";
  const std::uint64_t b = clock.now_ns();
  EXPECT_GE(b, a);
}

}  // namespace
}  // namespace canforge::sim
