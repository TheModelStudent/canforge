// SPDX-License-Identifier: MIT
#ifndef CANFORGE_SIM_VEHICLE_HPP
#define CANFORGE_SIM_VEHICLE_HPP

/// A longitudinal vehicle model.
///
/// This is demo-grade physics, not validated vehicle dynamics: it exists so
/// that the signals on the bus tell one coherent story instead of drifting
/// independently. Throttle raises engine speed against inertia and load, the
/// gear ratio couples engine speed to wheel speed, aerodynamic drag and
/// rolling resistance oppose motion, and brake pressure decelerates. A
/// first-order coolant model warms the engine up under load, because a
/// flat 90 degrees from the first frame looks obviously fake in a trace.
///
/// Integration is semi-implicit Euler at a fixed 1 ms internal step, with
/// sub-stepping so the caller can advance by any interval and get the same
/// trajectory.

#include <cstdint>
#include <string_view>
#include <vector>

namespace canforge::sim {

struct VehicleParams {
  double mass_kg = 1500.0;
  double wheel_radius_m = 0.32;
  double drag_area = 0.70;  ///< 0.5 * rho * Cd * A, in kg/m
  double rolling_resistance = 0.013;
  std::vector<double> gear_ratios = {3.5, 2.1, 1.4, 1.0, 0.8};
  double final_drive = 3.9;
  double idle_rpm = 800.0;
  double max_rpm = 6500.0;
  double peak_torque_nm = 250.0;
  double engine_inertia = 0.25;  ///< kg m^2, engine plus flywheel
  double brake_torque_per_bar = 220.0;
  double ambient_temp_c = 20.0;
  double coolant_time_constant_s = 90.0;
};

struct VehicleState {
  double engine_rpm = 800.0;
  double speed_mps = 0.0;
  double distance_m = 0.0;
  double acceleration_mps2 = 0.0;
  double engine_torque_nm = 0.0;
  double coolant_temp_c = 20.0;
  double fuel_rate_lph = 0.0;
  int gear = 1;  ///< 0 is neutral
  double throttle_pct = 0.0;
  double brake_bar = 0.0;
};

class Vehicle {
 public:
  Vehicle() = default;
  explicit Vehicle(VehicleParams params) : params_(std::move(params)) {
    state_.engine_rpm = params_.idle_rpm;
    state_.coolant_temp_c = params_.ambient_temp_c;
  }

  void set_throttle(double percent) noexcept;
  void set_brake(double bar) noexcept;
  void set_gear(int gear) noexcept;

  void step(std::uint64_t dt_ns);

  const VehicleState& state() const noexcept { return state_; }
  const VehicleParams& params() const noexcept { return params_; }

  /// Named read-only access, so the config file can bind a DBC signal to
  /// `plant.engine_rpm` without the parser knowing the struct layout.
  /// Returns false for an unknown name, which the config parser turns into a
  /// diagnostic listing the valid ones.
  bool read(std::string_view field, double& out) const noexcept;
  static const std::vector<std::string_view>& field_names();

 private:
  void integrate(double dt_s);

  VehicleParams params_;
  VehicleState state_;
  double leftover_ns_ = 0.0;
};

}  // namespace canforge::sim

#endif  // CANFORGE_SIM_VEHICLE_HPP
