// SPDX-License-Identifier: MIT
#include "canforge/sim/Simulator.hpp"

#include <algorithm>

namespace canforge::sim {

core::Result<std::unique_ptr<Simulator>> Simulator::build(SimConfig config,
                                                          const core::Database& db) {
  auto sim = std::unique_ptr<Simulator>(new Simulator(std::move(config), db));

  if (sim->config_.has_plant) {
    sim->vehicle_ = Vehicle(sim->config_.plant);
  }

  for (const NodeSpec& node : sim->config_.nodes) {
    Ecu ecu(node.name);
    for (const TxSpec& spec : node.transmits) {
      const core::Message* message = db.find_message(spec.message);
      if (message == nullptr) {
        return core::Error(core::ErrorCode::ParseUndefinedReference,
                           "the config transmits a message the database does "
                           "not define");
      }
      TxMessage tx;
      tx.message = message;
      tx.cycle_ns = spec.cycle_ns;
      tx.jitter_ns = spec.jitter_ns;
      tx.phase_ns = spec.phase_ns;
      tx.mode = spec.mode;
      tx.counter_signal = spec.counter_signal;
      tx.checksum_signal = spec.checksum_signal;
      tx.checksum = spec.checksum;

      for (const std::string& named : {spec.counter_signal, spec.checksum_signal}) {
        if (!named.empty() && message->find_signal(named) == nullptr) {
          return core::Error(core::ErrorCode::ParseUndefinedReference,
                             "counter or checksum names a signal the message "
                             "does not have");
        }
      }

      for (const TxSpec::Binding& binding : spec.bindings) {
        if (message->find_signal(binding.signal) == nullptr) {
          return core::Error(core::ErrorCode::ParseUndefinedReference,
                             "the config binds a signal the message does not have");
        }
        SignalBinding out;
        out.signal = binding.signal;
        if (!binding.plant_field.empty()) {
          out.plant_field = binding.plant_field;
        } else {
          auto built = build_source(binding.expression);
          if (!built) {
            return built.error();
          }
          out.source = std::move(built).value();
        }
        tx.bindings.push_back(std::move(out));
      }
      ecu.add(std::move(tx));
    }
    sim->nodes_.push_back(std::move(ecu));
  }

  for (const Fault& fault : sim->config_.faults) {
    sim->faults_.add(fault);
  }
  std::stable_sort(sim->config_.drive.begin(), sim->config_.drive.end(),
                   [](const DriveEvent& a, const DriveEvent& b) {
                     return a.at_ns < b.at_ns;
                   });
  return sim;
}

Ecu* Simulator::find_node(std::string_view name) noexcept {
  for (Ecu& e : nodes_) {
    if (e.name() == name) {
      return &e;
    }
  }
  return nullptr;
}

void Simulator::reset() {
  pending_.clear();
  next_drive_ = 0;
  produced_ = 0;
  last_step_ns_ = 0;
  started_ = false;
  faults_.reset();
  if (config_.has_plant) {
    vehicle_ = Vehicle(config_.plant);
  }
  for (Ecu& e : nodes_) {
    e.reset(0);
    e.set_running(true);
  }
}

void Simulator::apply_drive(std::uint64_t now_ns) {
  while (next_drive_ < config_.drive.size() &&
         config_.drive[next_drive_].at_ns <= now_ns) {
    const DriveEvent& event = config_.drive[next_drive_];
    if (event.has_throttle) {
      vehicle_.set_throttle(event.throttle_pct);
    }
    if (event.has_brake) {
      vehicle_.set_brake(event.brake_bar);
    }
    if (event.has_gear) {
      vehicle_.set_gear(event.gear);
    }
    ++next_drive_;
  }
}

std::vector<core::Frame> Simulator::step(std::uint64_t now_ns) {
  if (!started_) {
    last_step_ns_ = now_ns;
    started_ = true;
  }
  const std::uint64_t dt = now_ns > last_step_ns_ ? now_ns - last_step_ns_ : 0;
  last_step_ns_ = now_ns;

  apply_drive(now_ns);
  if (config_.has_plant && dt != 0) {
    vehicle_.step(dt);
  }

  const Vehicle* plant = config_.has_plant ? &vehicle_ : nullptr;
  for (Ecu& ecu : nodes_) {
    for (const core::Frame& frame : ecu.step(now_ns, plant)) {
      ++produced_;
      for (ScheduledFrame& scheduled :
           faults_.process(ecu.name(), frame, now_ns, *database_)) {
        pending_.push_back(std::move(scheduled));
      }
    }
  }

  // Release everything whose scheduled time has arrived, in time order, so a
  // delay fault reorders the bus exactly as a slow ECU would.
  std::stable_sort(pending_.begin(), pending_.end(),
                   [](const ScheduledFrame& a, const ScheduledFrame& b) {
                     return a.at_ns < b.at_ns;
                   });
  std::vector<core::Frame> out;
  auto it = pending_.begin();
  while (it != pending_.end() && it->at_ns <= now_ns) {
    core::Frame frame = it->frame;
    frame.set_timestamp_ns(now_ns);
    out.push_back(frame);
    ++it;
  }
  pending_.erase(pending_.begin(), it);
  return out;
}

}  // namespace canforge::sim
