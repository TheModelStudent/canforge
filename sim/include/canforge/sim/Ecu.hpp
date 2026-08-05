// SPDX-License-Identifier: MIT
#ifndef CANFORGE_SIM_ECU_HPP
#define CANFORGE_SIM_ECU_HPP

/// A simulated node: a set of messages it transmits on a schedule, each with
/// its signals bound either to a generator or to a field of the plant model.
///
/// Rolling counters and checksums are first-class, not an afterthought,
/// because nearly every real powertrain message carries them and a trace
/// without them looks wrong to anyone who has stared at one.

#include <cstdint>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "canforge/core/Database.hpp"
#include "canforge/sim/SignalSource.hpp"
#include "canforge/sim/Vehicle.hpp"

namespace canforge::sim {

enum class TxMode : std::uint8_t {
  Cyclic,    ///< Every cycle_ns, unconditionally.
  OnChange,  ///< Only when a signal value changed since the last transmission.
  Event,     ///< Only when explicitly triggered.
};

enum class ChecksumKind : std::uint8_t {
  None,
  Xor8,    ///< XOR of every other byte. Common on older buses.
  Crc8Sae, ///< SAE J1850: polynomial 0x1D, init 0xFF, final XOR 0xFF.
};

/// One signal of one message, driven by exactly one of: a generator, a plant
/// field, or (for counters and checksums) the framework itself.
struct SignalBinding {
  std::string signal;
  SourcePtr source;       ///< Null when `plant_field` is set.
  std::string plant_field;
};

struct TxMessage {
  const core::Message* message = nullptr;
  std::uint64_t cycle_ns = 100000000ULL;
  std::uint64_t jitter_ns = 0;   ///< Uniform +/- jitter on each transmission.
  std::uint64_t phase_ns = 0;    ///< Initial offset, so nodes do not all fire at t=0.
  TxMode mode = TxMode::Cyclic;
  std::vector<SignalBinding> bindings;
  std::string counter_signal;    ///< Incremented modulo the signal's width.
  std::string checksum_signal;
  ChecksumKind checksum = ChecksumKind::None;
};

std::uint8_t crc8_sae_j1850(const std::uint8_t* data, std::size_t n) noexcept;
std::uint8_t xor8(const std::uint8_t* data, std::size_t n, std::size_t skip) noexcept;

class Ecu {
 public:
  explicit Ecu(std::string name) : name_(std::move(name)) {}

  const std::string& name() const noexcept { return name_; }
  void add(TxMessage message);
  const std::vector<TxMessage>& messages() const noexcept { return messages_; }

  /// A stopped node transmits nothing, which is how the "node dropped off the
  /// bus" fault is expressed.
  void set_running(bool on) noexcept { running_ = on; }
  bool running() const noexcept { return running_; }

  bool trigger(std::string_view message_name) noexcept;

  /// Produce whatever is due at `now_ns`. Never sleeps, never does I/O.
  std::vector<core::Frame> step(std::uint64_t now_ns, const Vehicle* plant);

  void reset(std::uint64_t now_ns);

 private:
  struct Runtime {
    std::uint64_t next_due_ns = 0;
    std::uint32_t counter = 0;
    core::Frame last;
    bool has_last = false;
    bool triggered = false;
  };

  core::Frame build(TxMessage& tx, Runtime& rt, const Vehicle* plant,
                    std::uint64_t now_ns);

  std::string name_;
  std::vector<TxMessage> messages_;
  std::vector<Runtime> runtime_;
  static constexpr std::uint64_t kJitterSeed = 0x5147C0DEULL;
  std::mt19937_64 rng_{kJitterSeed};
  bool running_ = true;
};

}  // namespace canforge::sim

#endif  // CANFORGE_SIM_ECU_HPP
