// SPDX-License-Identifier: MIT
#include "canforge/sim/SignalSource.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace canforge::sim {
namespace {

constexpr double kNs = 1e9;

class Constant final : public SignalSource {
 public:
  explicit Constant(double v) : v_(v) {}
  double sample(std::uint64_t) override { return v_; }

 private:
  double v_;
};

class Ramp final : public SignalSource {
 public:
  Ramp(double from, double to, std::uint64_t over_ns, RampMode mode)
      : from_(from), to_(to), over_(over_ns == 0 ? 1 : over_ns), mode_(mode) {}

  double sample(std::uint64_t t) override {
    const std::uint64_t cycle = t / over_;
    double phase = static_cast<double>(t % over_) / static_cast<double>(over_);
    switch (mode_) {
      case RampMode::Clamp:
        if (t >= over_) {
          return to_;
        }
        break;
      case RampMode::Repeat:
        break;
      case RampMode::PingPong:
        if ((cycle % 2u) == 1u) {
          phase = 1.0 - phase;
        }
        break;
    }
    return from_ + (to_ - from_) * phase;
  }

 private:
  double from_;
  double to_;
  std::uint64_t over_;
  RampMode mode_;
};

class Sine final : public SignalSource {
 public:
  Sine(double amplitude, double hz, double offset, double phase_deg)
      : amp_(amplitude),
        hz_(hz),
        offset_(offset),
        phase_(phase_deg * 3.14159265358979323846 / 180.0) {}

  double sample(std::uint64_t t) override {
    const double seconds = static_cast<double>(t) / kNs;
    return offset_ +
           amp_ * std::sin(2.0 * 3.14159265358979323846 * hz_ * seconds + phase_);
  }

 private:
  double amp_;
  double hz_;
  double offset_;
  double phase_;
};

class Square final : public SignalSource {
 public:
  Square(double low, double high, std::uint64_t period_ns, double duty)
      : low_(low),
        high_(high),
        period_(period_ns == 0 ? 1 : period_ns),
        duty_(std::clamp(duty, 0.0, 1.0)) {}

  double sample(std::uint64_t t) override {
    const double phase =
        static_cast<double>(t % period_) / static_cast<double>(period_);
    return phase < duty_ ? high_ : low_;
  }

 private:
  double low_;
  double high_;
  std::uint64_t period_;
  double duty_;
};

/// The walk is a deterministic function of the step index, not of how often
/// sample() happens to be called, so stepping the simulation at 1 ms and at
/// 10 ms produce the same trace.
class RandomWalk final : public SignalSource {
 public:
  RandomWalk(double start, double step, double low, double high, std::uint64_t seed,
             std::uint64_t interval_ns)
      : start_(start),
        step_(step),
        low_(low),
        high_(high),
        seed_(seed),
        interval_(interval_ns == 0 ? 1 : interval_ns) {
    reset();
  }

  void reset() override {
    value_ = start_;
    index_ = 0;
    rng_.seed(seed_);
  }

  double sample(std::uint64_t t) override {
    const std::uint64_t want = t / interval_;
    if (want < index_) {
      reset();
    }
    std::uniform_real_distribution<double> dist(-step_, step_);
    while (index_ < want) {
      value_ = std::clamp(value_ + dist(rng_), low_, high_);
      ++index_;
    }
    return value_;
  }

 private:
  double start_;
  double step_;
  double low_;
  double high_;
  std::uint64_t seed_;
  std::uint64_t interval_;
  double value_ = 0.0;
  std::uint64_t index_ = 0;
  std::mt19937_64 rng_{0};
};

class Keyframes final : public SignalSource {
 public:
  Keyframes(std::vector<Keyframe> frames, bool interpolate)
      : frames_(std::move(frames)), interpolate_(interpolate) {
    std::stable_sort(
        frames_.begin(), frames_.end(),
        [](const Keyframe& a, const Keyframe& b) { return a.at_ns < b.at_ns; });
  }

  double sample(std::uint64_t t) override {
    if (frames_.empty()) {
      return 0.0;
    }
    if (t <= frames_.front().at_ns) {
      return frames_.front().value;
    }
    if (t >= frames_.back().at_ns) {
      return frames_.back().value;
    }
    std::size_t i = 0;
    while (i + 1 < frames_.size() && frames_[i + 1].at_ns <= t) {
      ++i;
    }
    const Keyframe& a = frames_[i];
    const Keyframe& b = frames_[i + 1];
    if (!interpolate_ || b.at_ns == a.at_ns) {
      return a.value;
    }
    const double span = static_cast<double>(b.at_ns - a.at_ns);
    const double into = static_cast<double>(t - a.at_ns);
    return a.value + (b.value - a.value) * (into / span);
  }

 private:
  std::vector<Keyframe> frames_;
  bool interpolate_;
};

class Combine final : public SignalSource {
 public:
  Combine(std::vector<SourcePtr> parts, bool multiply)
      : parts_(std::move(parts)), multiply_(multiply) {}

  double sample(std::uint64_t t) override {
    double acc = multiply_ ? 1.0 : 0.0;
    for (SourcePtr& p : parts_) {
      const double v = p->sample(t);
      acc = multiply_ ? acc * v : acc + v;
    }
    return acc;
  }
  void reset() override {
    for (SourcePtr& p : parts_) {
      p->reset();
    }
  }

 private:
  std::vector<SourcePtr> parts_;
  bool multiply_;
};

class Affine final : public SignalSource {
 public:
  Affine(SourcePtr inner, double factor, double offset)
      : inner_(std::move(inner)), factor_(factor), offset_(offset) {}
  double sample(std::uint64_t t) override {
    return inner_->sample(t) * factor_ + offset_;
  }
  void reset() override { inner_->reset(); }

 private:
  SourcePtr inner_;
  double factor_;
  double offset_;
};

class Clamped final : public SignalSource {
 public:
  Clamped(SourcePtr inner, double low, double high)
      : inner_(std::move(inner)), low_(low), high_(high) {}
  double sample(std::uint64_t t) override {
    return std::clamp(inner_->sample(t), low_, high_);
  }
  void reset() override { inner_->reset(); }

 private:
  SourcePtr inner_;
  double low_;
  double high_;
};

}  // namespace

SteadyClock::SteadyClock()
    : origin_ns_(static_cast<std::uint64_t>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(
              std::chrono::steady_clock::now().time_since_epoch())
              .count())) {}

std::uint64_t SteadyClock::now_ns() const noexcept {
  const auto now = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
  return now - origin_ns_;
}

SourcePtr constant(double value) {
  return std::make_unique<Constant>(value);
}

SourcePtr ramp(double from, double to, std::uint64_t over_ns, RampMode mode) {
  return std::make_unique<Ramp>(from, to, over_ns, mode);
}

SourcePtr sine(double amplitude, double frequency_hz, double offset,
               double phase_degrees) {
  return std::make_unique<Sine>(amplitude, frequency_hz, offset, phase_degrees);
}

SourcePtr square(double low, double high, std::uint64_t period_ns, double duty) {
  return std::make_unique<Square>(low, high, period_ns, duty);
}

SourcePtr random_walk(double start, double step, double low, double high,
                      std::uint64_t seed, std::uint64_t interval_ns) {
  return std::make_unique<RandomWalk>(start, step, low, high, seed, interval_ns);
}

SourcePtr keyframes(std::vector<Keyframe> frames, bool interpolate) {
  return std::make_unique<Keyframes>(std::move(frames), interpolate);
}

SourcePtr sum(std::vector<SourcePtr> parts) {
  return std::make_unique<Combine>(std::move(parts), false);
}

SourcePtr product(std::vector<SourcePtr> parts) {
  return std::make_unique<Combine>(std::move(parts), true);
}

SourcePtr scale(SourcePtr inner, double factor, double offset) {
  return std::make_unique<Affine>(std::move(inner), factor, offset);
}

SourcePtr clamp_to(SourcePtr inner, double low, double high) {
  return std::make_unique<Clamped>(std::move(inner), low, high);
}

}  // namespace canforge::sim
