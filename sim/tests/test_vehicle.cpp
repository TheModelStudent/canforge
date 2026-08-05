// SPDX-License-Identifier: MIT
#include "canforge/sim/Vehicle.hpp"

#include <gtest/gtest.h>

namespace canforge::sim {
namespace {

constexpr std::uint64_t kMs = 1000000ULL;

Vehicle make() { return Vehicle(VehicleParams{}); }

void run(Vehicle& v, double seconds) {
  for (int i = 0; i < static_cast<int>(seconds * 100); ++i) {
    v.step(10 * kMs);
  }
}

TEST(Vehicle, IdlesWithoutStalling) {
  Vehicle v = make();
  v.set_gear(0);
  run(v, 5.0);
  EXPECT_NEAR(v.state().engine_rpm, v.params().idle_rpm, 250.0);
  EXPECT_DOUBLE_EQ(v.state().speed_mps, 0.0);
}

TEST(Vehicle, ThrottleAccelerates) {
  Vehicle v = make();
  v.set_gear(1);
  v.set_throttle(80.0);
  run(v, 5.0);
  EXPECT_GT(v.state().speed_mps, 1.0) << "the car should be moving";
  EXPECT_GT(v.state().engine_rpm, v.params().idle_rpm);
  EXPECT_GT(v.state().distance_m, 0.0);
}

TEST(Vehicle, BrakeDecelerates) {
  Vehicle v = make();
  v.set_gear(1);
  v.set_throttle(80.0);
  run(v, 8.0);
  const double moving = v.state().speed_mps;
  ASSERT_GT(moving, 1.0);

  v.set_throttle(0.0);
  v.set_brake(40.0);
  run(v, 5.0);
  EXPECT_LT(v.state().speed_mps, moving);
  EXPECT_GE(v.state().speed_mps, 0.0) << "braking must not reverse the car";
}

TEST(Vehicle, ComesToACompleteStop) {
  Vehicle v = make();
  v.set_gear(1);
  v.set_throttle(60.0);
  run(v, 5.0);
  v.set_throttle(0.0);
  v.set_brake(120.0);
  run(v, 20.0);
  EXPECT_DOUBLE_EQ(v.state().speed_mps, 0.0);
}

TEST(Vehicle, HigherGearGivesLowerRpmAtTheSameSpeed) {
  Vehicle low = make();
  low.set_gear(1);
  low.set_throttle(50.0);
  run(low, 10.0);

  Vehicle high = make();
  high.set_gear(4);
  high.set_throttle(50.0);
  run(high, 10.0);

  // Same road speed band, but the taller gear turns the engine slower per
  // unit of road speed.
  const double low_ratio = low.state().engine_rpm / std::max(0.1, low.state().speed_mps);
  const double high_ratio =
      high.state().engine_rpm / std::max(0.1, high.state().speed_mps);
  EXPECT_LT(high_ratio, low_ratio);
}

TEST(Vehicle, DragLimitsTopSpeed) {
  Vehicle v = make();
  v.set_gear(5);
  v.set_throttle(100.0);
  run(v, 120.0);
  const double first = v.state().speed_mps;
  run(v, 30.0);
  EXPECT_NEAR(v.state().speed_mps, first, 1.5) << "speed should have settled";
  EXPECT_LT(v.state().speed_mps, 120.0) << "and be physically plausible";
}

TEST(Vehicle, CoolantWarmsUp) {
  Vehicle v = make();
  const double cold = v.state().coolant_temp_c;
  v.set_gear(1);
  v.set_throttle(50.0);
  run(v, 200.0);
  EXPECT_GT(v.state().coolant_temp_c, cold + 40.0);
  EXPECT_LT(v.state().coolant_temp_c, 130.0);
}

TEST(Vehicle, SteppingCoarselyMatchesSteppingFinely) {
  // The internal fixed step means the trajectory does not depend on how the
  // caller slices time.
  Vehicle fine = make();
  Vehicle coarse = make();
  for (Vehicle* v : {&fine, &coarse}) {
    v->set_gear(2);
    v->set_throttle(70.0);
  }
  for (int i = 0; i < 1000; ++i) {
    fine.step(1 * kMs);
  }
  for (int i = 0; i < 10; ++i) {
    coarse.step(100 * kMs);
  }
  EXPECT_NEAR(fine.state().speed_mps, coarse.state().speed_mps, 1e-6);
  EXPECT_NEAR(fine.state().engine_rpm, coarse.state().engine_rpm, 1e-6);
}

TEST(Vehicle, NamedFieldAccess) {
  Vehicle v = make();
  v.set_gear(2);
  v.set_throttle(30.0);
  run(v, 2.0);

  double out = 0.0;
  EXPECT_TRUE(v.read("engine_rpm", out));
  EXPECT_DOUBLE_EQ(out, v.state().engine_rpm);
  EXPECT_TRUE(v.read("speed_kmh", out));
  EXPECT_NEAR(out, v.state().speed_mps * 3.6, 1e-9);
  EXPECT_TRUE(v.read("gear", out));
  EXPECT_DOUBLE_EQ(out, 2.0);
  EXPECT_TRUE(v.read("throttle_pct", out));
  EXPECT_DOUBLE_EQ(out, 30.0);
  EXPECT_FALSE(v.read("no_such_field", out));
  EXPECT_FALSE(Vehicle::field_names().empty());
}

}  // namespace
}  // namespace canforge::sim
