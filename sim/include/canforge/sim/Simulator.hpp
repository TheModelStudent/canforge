// SPDX-License-Identifier: MIT
#ifndef CANFORGE_SIM_SIMULATOR_HPP
#define CANFORGE_SIM_SIMULATOR_HPP

/// Ties the plant, the ECUs and the fault injector together.
///
/// Like everything else in sim/, the simulator neither sleeps nor does I/O:
/// `step(now_ns)` returns the frames that should be on the bus by that time
/// and the caller decides where they go. That is what lets one code path serve
/// both a deterministic test on the virtual bus and a real run against vcan0.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "canforge/core/Database.hpp"
#include "canforge/sim/Config.hpp"
#include "canforge/sim/Ecu.hpp"
#include "canforge/sim/Faults.hpp"
#include "canforge/sim/Vehicle.hpp"

namespace canforge::sim {

class Simulator {
 public:
  /// Binds a parsed config to a database. Fails when the config names a
  /// message or signal the database does not contain -- caught here rather
  /// than silently producing an empty bus.
  static core::Result<std::unique_ptr<Simulator>> build(SimConfig config,
                                                        const core::Database& db);

  /// Advance to `now_ns` and return everything that should be transmitted.
  /// Frames held back by a delay fault surface in a later step.
  std::vector<core::Frame> step(std::uint64_t now_ns);

  Vehicle& vehicle() noexcept { return vehicle_; }
  const Vehicle& vehicle() const noexcept { return vehicle_; }
  FaultInjector& faults() noexcept { return faults_; }
  std::vector<Ecu>& nodes() noexcept { return nodes_; }
  const std::vector<Ecu>& nodes() const noexcept { return nodes_; }
  const SimConfig& config() const noexcept { return config_; }

  Ecu* find_node(std::string_view name) noexcept;
  std::uint64_t frames_produced() const noexcept { return produced_; }

  void reset();

 private:
  Simulator(SimConfig config, const core::Database& db)
      : config_(std::move(config)), database_(&db) {}

  void apply_drive(std::uint64_t now_ns);

  SimConfig config_;
  const core::Database* database_;
  Vehicle vehicle_;
  std::vector<Ecu> nodes_;
  FaultInjector faults_;
  std::vector<ScheduledFrame> pending_;
  std::uint64_t last_step_ns_ = 0;
  std::size_t next_drive_ = 0;
  std::uint64_t produced_ = 0;
  bool started_ = false;
};

}  // namespace canforge::sim

#endif  // CANFORGE_SIM_SIMULATOR_HPP
