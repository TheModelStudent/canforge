// SPDX-License-Identifier: MIT
#include "canforge/sim/Vehicle.hpp"

#include <algorithm>
#include <cmath>

namespace canforge::sim {
namespace {

constexpr double kRpmToRadPerSec = 2.0 * 3.14159265358979323846 / 60.0;
constexpr double kStepNs = 1000000.0;  // 1 ms internal step

/// A crude but well-behaved torque curve: full torque across the mid range,
/// tapering below idle and above the power peak. Enough shape that the RPM
/// trace is not a straight line.
double torque_shape(double rpm, const VehicleParams& p) {
  const double lo = p.idle_rpm * 0.6;
  const double peak = p.max_rpm * 0.55;
  if (rpm <= lo) {
    return 0.55;
  }
  if (rpm <= peak) {
    return 0.55 + 0.45 * (rpm - lo) / (peak - lo);
  }
  const double falloff = (rpm - peak) / std::max(1.0, p.max_rpm - peak);
  return std::max(0.15, 1.0 - 0.6 * falloff);
}

}  // namespace

void Vehicle::set_throttle(double percent) noexcept {
  state_.throttle_pct = std::clamp(percent, 0.0, 100.0);
}

void Vehicle::set_brake(double bar) noexcept {
  state_.brake_bar = std::clamp(bar, 0.0, 150.0);
}

void Vehicle::set_gear(int gear) noexcept {
  state_.gear = std::clamp(gear, 0, static_cast<int>(params_.gear_ratios.size()));
}

void Vehicle::step(std::uint64_t dt_ns) {
  leftover_ns_ += static_cast<double>(dt_ns);
  while (leftover_ns_ >= kStepNs) {
    integrate(kStepNs / 1e9);
    leftover_ns_ -= kStepNs;
  }
}

void Vehicle::integrate(double dt_s) {
  const VehicleParams& p = params_;
  VehicleState& s = state_;

  const bool in_gear = s.gear >= 1 && s.gear <= static_cast<int>(p.gear_ratios.size());
  const double ratio =
      in_gear ? p.gear_ratios[static_cast<std::size_t>(s.gear - 1)] * p.final_drive
              : 0.0;

  // Engine torque from throttle, shaped by speed. An idle governor adds
  // torque below idle so the engine does not stall at zero throttle.
  const double demand = s.throttle_pct / 100.0;
  double torque = p.peak_torque_nm * demand * torque_shape(s.engine_rpm, p);
  if (s.engine_rpm < p.idle_rpm) {
    torque += p.peak_torque_nm * 0.25 * (p.idle_rpm - s.engine_rpm) / p.idle_rpm;
  }
  // Internal friction and pumping losses, rising with speed.
  torque -= 8.0 + 0.004 * s.engine_rpm;
  s.engine_torque_nm = torque;

  // Road load at the wheels.
  const double drag = p.drag_area * s.speed_mps * std::fabs(s.speed_mps);
  const double roll =
      p.rolling_resistance * p.mass_kg * 9.81 * (s.speed_mps > 0.01 ? 1.0 : 0.0);
  const double brake_force = p.brake_torque_per_bar * s.brake_bar / p.wheel_radius_m;

  double drive_force = 0.0;
  if (in_gear) {
    drive_force = torque * ratio / p.wheel_radius_m;
  }

  double net = drive_force - drag - roll;
  // Braking opposes motion but must not push the car backwards.
  if (s.speed_mps > 0.0) {
    net -= brake_force;
  }
  double accel = net / p.mass_kg;
  if (s.speed_mps <= 0.0 && accel < 0.0) {
    accel = 0.0;
    s.speed_mps = 0.0;
  }
  s.acceleration_mps2 = accel;
  s.speed_mps = std::max(0.0, s.speed_mps + accel * dt_s);
  s.distance_m += s.speed_mps * dt_s;

  if (in_gear) {
    // Coupled: engine speed follows wheel speed through the gear. A real
    // driveline has a clutch and torque converter; this models a locked
    // driveline with a first-order pull toward the geared speed, which is
    // enough to make gear changes visible without a stiff ODE.
    const double geared_rpm = s.speed_mps / p.wheel_radius_m * ratio / kRpmToRadPerSec;
    const double pull = std::clamp(dt_s / 0.15, 0.0, 1.0);
    s.engine_rpm += (std::max(geared_rpm, p.idle_rpm) - s.engine_rpm) * pull;
  } else {
    // Free-revving: torque accelerates the engine against its own inertia.
    const double alpha = torque / p.engine_inertia;
    s.engine_rpm += alpha / kRpmToRadPerSec * dt_s;
  }
  s.engine_rpm = std::clamp(s.engine_rpm, 0.0, p.max_rpm);

  // Coolant warms toward a load-dependent target.
  const double load = std::clamp(demand, 0.0, 1.0);
  const double target = p.ambient_temp_c + 70.0 + 20.0 * load;
  const double k = dt_s / std::max(1.0, p.coolant_time_constant_s);
  s.coolant_temp_c += (target - s.coolant_temp_c) * k;

  // Fuel: idle burn plus a term proportional to torque and speed.
  s.fuel_rate_lph =
      0.6 + std::max(0.0, torque) * s.engine_rpm * 2.6e-5 / 60.0 * 60.0 * 0.001;
}

bool Vehicle::read(std::string_view field, double& out) const noexcept {
  const VehicleState& s = state_;
  if (field == "engine_rpm") {
    out = s.engine_rpm;
    return true;
  }
  if (field == "speed_mps") {
    out = s.speed_mps;
    return true;
  }
  if (field == "speed_kmh") {
    out = s.speed_mps * 3.6;
    return true;
  }
  if (field == "wheel_rpm") {
    out = s.speed_mps / params_.wheel_radius_m / kRpmToRadPerSec;
    return true;
  }
  if (field == "gear") {
    out = static_cast<double>(s.gear);
    return true;
  }
  if (field == "throttle_pct") {
    out = s.throttle_pct;
    return true;
  }
  if (field == "brake_bar") {
    out = s.brake_bar;
    return true;
  }
  if (field == "engine_torque_nm") {
    out = s.engine_torque_nm;
    return true;
  }
  if (field == "coolant_temp_c") {
    out = s.coolant_temp_c;
    return true;
  }
  if (field == "fuel_rate_lph") {
    out = s.fuel_rate_lph;
    return true;
  }
  if (field == "distance_m") {
    out = s.distance_m;
    return true;
  }
  if (field == "accel_mps2") {
    out = s.acceleration_mps2;
    return true;
  }
  return false;
}

const std::vector<std::string_view>& Vehicle::field_names() {
  static const std::vector<std::string_view> names = {
      "engine_rpm",     "speed_mps",     "speed_kmh",  "wheel_rpm",
      "gear",           "throttle_pct",  "brake_bar",  "engine_torque_nm",
      "coolant_temp_c", "fuel_rate_lph", "distance_m", "accel_mps2"};
  return names;
}

}  // namespace canforge::sim
