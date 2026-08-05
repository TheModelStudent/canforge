// SPDX-License-Identifier: MIT
#ifndef CANFORGE_SIM_CONFIG_HPP
#define CANFORGE_SIM_CONFIG_HPP

/// The simulator's configuration language.
///
/// Deliberately not YAML or JSON: both would be a dependency, and neither
/// reads well for this. The format is line oriented, scanned with the same
/// `text::Lexer` the DBC parser uses, and parsed by recursive descent with the
/// same compiler-style diagnostics -- so a typo in a config file reports a
/// line, a column and a caret exactly as a broken DBC does.
///
///     bus vcan0 500000
///     database powertrain.dbc
///
///     node ECM
///       tx EngineData cycle 10ms jitter 1ms
///         signal EngineSpeed       = plant.engine_rpm
///         signal EngineCoolantTemp = ramp 20 to 90 over 60s
///         signal ThrottlePosition  = plant.throttle_pct
///
///     plant
///       mass 1500kg
///       gear_ratios 3.5 2.1 1.4 1.0 0.8
///
///     drive
///       at 0s  throttle 0
///       at 2s  throttle 60
///
///     fault dropsome drop 0.1 node ECM
///
/// Sources: `const V`, `ramp A to B over T [repeat|pingpong]`,
/// `sine amp A freq F [offset O] [phase P]`, `square low L high H period T
/// [duty D]`, `walk start S step D min L max H [seed N]`,
/// `keyframes (T=V)...`, and `plant.<field>`. Any source may be followed by
/// `+ <source>` to sum, or `* <number>` to scale.

#include <cstdint>
#include <string>
#include <vector>

#include "canforge/core/Result.hpp"
#include "canforge/sim/Ecu.hpp"
#include "canforge/sim/Faults.hpp"
#include "canforge/sim/Vehicle.hpp"
#include "canforge/text/Diagnostic.hpp"

namespace canforge::sim {

/// One entry of the driver script: at this time, apply these pedal inputs.
struct DriveEvent {
  std::uint64_t at_ns = 0;
  bool has_throttle = false;
  double throttle_pct = 0.0;
  bool has_brake = false;
  double brake_bar = 0.0;
  bool has_gear = false;
  int gear = 1;
};

/// The parsed form of a `tx` block, before it is bound to a Database.
struct TxSpec {
  std::string message;
  std::uint64_t cycle_ns = 100000000ULL;
  std::uint64_t jitter_ns = 0;
  std::uint64_t phase_ns = 0;
  TxMode mode = TxMode::Cyclic;
  std::string counter_signal;
  std::string checksum_signal;
  ChecksumKind checksum = ChecksumKind::None;
  /// Bindings are kept as source text and rebuilt per Ecu, because a
  /// SourcePtr is move-only and a spec may be instantiated more than once.
  struct Binding {
    std::string signal;
    std::string plant_field;
    std::string expression;  ///< Empty when plant_field is set.
  };
  std::vector<Binding> bindings;
};

struct NodeSpec {
  std::string name;
  std::vector<TxSpec> transmits;
};

struct SimConfig {
  std::string bus = "vcan0";
  std::uint32_t nominal_bitrate = 500000;
  std::uint32_t data_bitrate = 2000000;
  std::string database_path;
  bool has_plant = false;
  VehicleParams plant;
  std::vector<DriveEvent> drive;
  std::vector<NodeSpec> nodes;
  std::vector<Fault> faults;
};

struct ConfigParseResult {
  SimConfig config;
  text::DiagnosticSink diagnostics;
  bool ok() const noexcept { return !diagnostics.has_errors(); }
};

ConfigParseResult parse_config_string(std::string_view text,
                                      std::string_view filename = "<memory>");
core::Result<ConfigParseResult> parse_config_file(const std::string& path);

/// Build a signal source from one binding expression. Exposed so the
/// simulator can rebuild sources when it resets, and so it is directly
/// testable.
core::Result<SourcePtr> build_source(const std::string& expression);

}  // namespace canforge::sim

#endif  // CANFORGE_SIM_CONFIG_HPP
