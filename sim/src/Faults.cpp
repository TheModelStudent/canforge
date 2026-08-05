// SPDX-License-Identifier: MIT
#include "canforge/sim/Faults.hpp"

#include <algorithm>

namespace canforge::sim {

void FaultInjector::add(Fault fault) {
  faults_.push_back(std::move(fault));
}

const Fault* FaultInjector::find(std::string_view name) const noexcept {
  for (const Fault& f : faults_) {
    if (f.name == name) {
      return &f;
    }
  }
  return nullptr;
}

bool FaultInjector::set_active(std::string_view name, bool on) noexcept {
  for (Fault& f : faults_) {
    if (f.name == name) {
      f.active = on;
      return true;
    }
  }
  return false;
}

bool FaultInjector::toggle(std::string_view name) noexcept {
  for (Fault& f : faults_) {
    if (f.name == name) {
      f.active = !f.active;
      return true;
    }
  }
  return false;
}

bool FaultInjector::node_stopped(std::string_view node) const noexcept {
  for (const Fault& f : faults_) {
    if (f.active && f.kind == FaultKind::StopNode &&
        (f.node.empty() || f.node == node)) {
      return true;
    }
  }
  return false;
}

FaultInjector::FrozenValue& FaultInjector::frozen_slot(std::uint32_t id,
                                                       const std::string& signal) {
  for (FrozenValue& v : frozen_) {
    if (v.id == id && v.signal == signal) {
      return v;
    }
  }
  FrozenValue v;
  v.id = id;
  v.signal = signal;
  frozen_.push_back(std::move(v));
  return frozen_.back();
}

std::vector<ScheduledFrame> FaultInjector::process(std::string_view node,
                                                   const core::Frame& frame,
                                                   std::uint64_t now_ns,
                                                   const core::Database& database) {
  std::vector<ScheduledFrame> out;
  if (node_stopped(node)) {
    ++dropped_;
    return out;
  }

  core::Frame working = frame;
  std::uint64_t when = now_ns;
  const core::Message* message = database.find_message(frame.id());

  for (Fault& f : faults_) {
    if (!f.active) {
      continue;
    }
    if (!f.node.empty() && f.node != node) {
      continue;
    }
    if (f.has_id && f.id != frame.id()) {
      continue;
    }

    switch (f.kind) {
      case FaultKind::StopNode:
        break;  // handled above

      case FaultKind::DropFrames: {
        std::uniform_real_distribution<double> dist(0.0, 1.0);
        if (dist(rng_) < f.probability) {
          ++dropped_;
          return {};
        }
        break;
      }

      case FaultKind::DelayMessage:
        when += f.delay_ns;
        ++delayed_;
        break;

      case FaultKind::FreezeSignal: {
        if (message == nullptr) {
          break;
        }
        const core::Signal* s = message->find_signal(f.signal);
        if (s == nullptr || !s->layout().fits(working.size())) {
          break;
        }
        FrozenValue& slot = frozen_slot(frame.id().value(), f.signal);
        if (!slot.captured) {
          slot.value = s->decode(working);
          slot.captured = true;
        }
        static_cast<void>(s->encode(slot.value, working));
        break;
      }

      case FaultKind::OutOfRange: {
        if (message == nullptr) {
          break;
        }
        const core::Signal* s = message->find_signal(f.signal);
        if (s == nullptr || !s->layout().fits(working.size())) {
          break;
        }
        // The codec saturates to the declared range by design, so an
        // out-of-range fault has to go in as a raw value; that is exactly what
        // a misbehaving ECU puts on the wire.
        core::SignalLayout raw = s->layout();
        raw.minimum = 0.0;
        raw.maximum = 0.0;
        static_cast<void>(raw.encode(f.value, working.data()));
        break;
      }

      case FaultKind::BitFlip: {
        std::uniform_real_distribution<double> dist(0.0, 1.0);
        if (dist(rng_) < f.probability && working.size() > 0) {
          std::uniform_int_distribution<std::size_t> which(0, working.size() * 8u - 1u);
          const std::size_t bit = which(rng_);
          working.data()[bit / 8u] =
              static_cast<std::uint8_t>(working.data()[bit / 8u] ^ (1u << (bit % 8u)));
          ++corrupted_;
        }
        break;
      }
    }
  }

  out.push_back(ScheduledFrame{when, working});
  return out;
}

}  // namespace canforge::sim
