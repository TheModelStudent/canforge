// SPDX-License-Identifier: MIT
#ifndef CANFORGE_SIM_FAULTS_HPP
#define CANFORGE_SIM_FAULTS_HPP

/// Runtime fault injection on the simulator's transmit path.
///
/// Every fault is named so it can be toggled at runtime, and every stochastic
/// fault is seeded, so "drop 10% of frames" drops exactly the same frames on
/// every run and a test can assert the count.

#include <cstdint>
#include <random>
#include <string>
#include <vector>

#include "canforge/core/Database.hpp"
#include "canforge/core/Frame.hpp"

namespace canforge::sim {

enum class FaultKind : std::uint8_t {
  DropFrames,    ///< Randomly discard a fraction of this node's frames.
  DelayMessage,  ///< Hold one message id back by a fixed time.
  FreezeSignal,  ///< Keep reporting the value a signal had when frozen.
  OutOfRange,    ///< Force a signal to a value outside its declared range.
  StopNode,      ///< Silence a node entirely.
  BitFlip,       ///< Corrupt a random payload bit, as a bad transceiver would.
};

struct Fault {
  FaultKind kind = FaultKind::DropFrames;
  std::string name;      ///< Unique handle used to toggle it.
  std::string node;      ///< Empty means every node.
  core::CanId id;        ///< For message- and signal-scoped faults.
  bool has_id = false;
  std::string signal;
  double value = 0.0;        ///< OutOfRange target.
  double probability = 0.0;  ///< DropFrames, BitFlip.
  std::uint64_t delay_ns = 0;
  bool active = false;
};

/// A frame with the time it should actually reach the bus. Delayed faults push
/// this into the future; everything else keeps `at_ns` equal to `now`.
struct ScheduledFrame {
  std::uint64_t at_ns = 0;
  core::Frame frame;
};

class FaultInjector {
 public:
  explicit FaultInjector(std::uint64_t seed = 0xFA0117ULL) : rng_(seed), seed_(seed) {}

  void add(Fault fault);
  bool set_active(std::string_view name, bool on) noexcept;
  bool toggle(std::string_view name) noexcept;
  const std::vector<Fault>& faults() const noexcept { return faults_; }
  const Fault* find(std::string_view name) const noexcept;

  /// True when a StopNode fault is silencing this node.
  bool node_stopped(std::string_view node) const noexcept;

  /// Run one frame through the active faults. Returns the frames that should
  /// actually be transmitted -- empty when the frame was dropped.
  std::vector<ScheduledFrame> process(std::string_view node,
                                      const core::Frame& frame,
                                      std::uint64_t now_ns,
                                      const core::Database& database);

  void reset() {
    rng_.seed(seed_);
    frozen_.clear();
  }

  std::uint64_t dropped() const noexcept { return dropped_; }
  std::uint64_t delayed() const noexcept { return delayed_; }
  std::uint64_t corrupted() const noexcept { return corrupted_; }

 private:
  struct FrozenValue {
    std::uint32_t id = 0;
    std::string signal;
    double value = 0.0;
    bool captured = false;
  };
  FrozenValue& frozen_slot(std::uint32_t id, const std::string& signal);

  std::vector<Fault> faults_;
  std::vector<FrozenValue> frozen_;
  std::mt19937_64 rng_;
  std::uint64_t seed_;
  std::uint64_t dropped_ = 0;
  std::uint64_t delayed_ = 0;
  std::uint64_t corrupted_ = 0;
};

}  // namespace canforge::sim

#endif  // CANFORGE_SIM_FAULTS_HPP
