// SPDX-License-Identifier: MIT
#ifndef CANFORGE_SIM_SIGNALSOURCE_HPP
#define CANFORGE_SIM_SIGNALSOURCE_HPP

/// Value generators for simulated signals, and the clock they run against.
///
/// Every source is a pure function of simulated time except the random walk,
/// which carries state but is seeded, so a whole simulation replays
/// identically from the same configuration. Sources compose: `sum`, `product`,
/// `scale` and `clamp` take other sources, so "a sine plus a slow drift,
/// clamped to the signal's range" is three lines of configuration.

#include <cstdint>
#include <memory>
#include <random>
#include <string>
#include <vector>

namespace canforge::sim {

/// Simulated time. Nothing in sim/ reads a wall clock or sleeps: everything is
/// driven by an explicit timestamp, which keeps the whole simulator
/// reproducible and its tests instant.
class IClock {
 public:
  IClock() = default;
  virtual ~IClock() = default;
  IClock(const IClock&) = delete;
  IClock& operator=(const IClock&) = delete;
  virtual std::uint64_t now_ns() const noexcept = 0;
};

class SteadyClock final : public IClock {
 public:
  SteadyClock();
  std::uint64_t now_ns() const noexcept override;

 private:
  std::uint64_t origin_ns_ = 0;
};

class VirtualClock final : public IClock {
 public:
  std::uint64_t now_ns() const noexcept override { return now_; }
  void advance(std::uint64_t by_ns) noexcept { now_ += by_ns; }
  void set(std::uint64_t at_ns) noexcept { now_ = at_ns; }

 private:
  std::uint64_t now_ = 0;
};

class SignalSource {
 public:
  SignalSource() = default;
  virtual ~SignalSource() = default;
  SignalSource(const SignalSource&) = delete;
  SignalSource& operator=(const SignalSource&) = delete;

  /// Value at simulated time `t_ns`. Not const: a random walk advances.
  virtual double sample(std::uint64_t t_ns) = 0;
  virtual void reset() {}
};

using SourcePtr = std::unique_ptr<SignalSource>;

enum class RampMode : std::uint8_t {
  Clamp,     ///< Hold the end value once the ramp completes.
  Repeat,    ///< Jump back to the start and run again.
  PingPong,  ///< Reverse direction at each end.
};

struct Keyframe {
  std::uint64_t at_ns = 0;
  double value = 0.0;
};

SourcePtr constant(double value);
SourcePtr ramp(double from, double to, std::uint64_t over_ns,
               RampMode mode = RampMode::Clamp);
SourcePtr sine(double amplitude, double frequency_hz, double offset = 0.0,
               double phase_degrees = 0.0);
SourcePtr square(double low, double high, std::uint64_t period_ns, double duty = 0.5);
/// A bounded random walk. `interval_ns` is how often it takes a step, so the
/// result does not depend on how finely the simulation is stepped.
SourcePtr random_walk(double start, double step, double low, double high,
                      std::uint64_t seed, std::uint64_t interval_ns = 100000000ULL);
/// Keyframes sorted by time; either linearly interpolated or held as steps.
SourcePtr keyframes(std::vector<Keyframe> frames, bool interpolate = true);

SourcePtr sum(std::vector<SourcePtr> parts);
SourcePtr product(std::vector<SourcePtr> parts);
SourcePtr scale(SourcePtr inner, double factor, double offset = 0.0);
SourcePtr clamp_to(SourcePtr inner, double low, double high);

}  // namespace canforge::sim

#endif  // CANFORGE_SIM_SIGNALSOURCE_HPP
