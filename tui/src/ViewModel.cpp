// SPDX-License-Identifier: MIT
#include "canforge/tui/ViewModel.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace canforge::tui {
namespace {

std::string hex_id(core::CanId id) {
  char buffer[16];
  std::snprintf(buffer, sizeof(buffer), id.is_extended() ? "%08X" : "%03X",
                id.value());
  return buffer;
}

std::string lowered(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}

}  // namespace

void SignalHistory::push(double value) {
  if (!std::isfinite(value)) {
    return;
  }
  samples_.push_back(value);
  while (samples_.size() > kCapacity) {
    samples_.pop_front();
  }
  // The bounds are recomputed rather than tracked incrementally, because a
  // dropped sample can be the one that held the extreme and an incremental
  // minimum would then be wrong for the rest of the session.
  const auto [lo, hi] = std::minmax_element(samples_.begin(), samples_.end());
  min_ = *lo;
  max_ = *hi;
}

void SignalHistory::clear() {
  samples_.clear();
  min_ = 0.0;
  max_ = 0.0;
}

std::vector<int> SignalHistory::levels(std::size_t rows) const {
  std::vector<int> out;
  if (samples_.empty() || rows == 0) {
    return out;
  }
  out.reserve(samples_.size());
  const double span = max_ - min_;
  for (const double v : samples_) {
    if (span <= 0.0) {
      // A constant signal: draw it down the middle. Mapping it to zero would
      // make a healthy steady value look like a dead one.
      out.push_back(static_cast<int>(rows / 2));
      continue;
    }
    const double scaled = (v - min_) / span * static_cast<double>(rows - 1);
    out.push_back(static_cast<int>(std::lround(scaled)));
  }
  return out;
}

void ViewModel::ingest(const core::FdFrame& frame) {
  const std::lock_guard<std::mutex> lock(mutex_);
  ++total_;
  now_ns_ = frame.timestamp_ns();

  const core::Message* message =
      database_ != nullptr ? database_->find_message(frame.id()) : nullptr;

  TraceEntry entry;
  entry.timestamp_ns = frame.timestamp_ns();
  entry.id = frame.id();
  entry.dlc = frame.dlc();
  entry.size = frame.size();
  std::copy(frame.data(), frame.data() + frame.size(), entry.data.begin());
  entry.is_error = frame.is_error_frame();
  if (message != nullptr) {
    entry.message_name = message->name();
    entry.node = message->transmitter();
    const auto decoded = message->decode(frame);
    if (decoded) {
      entry.decoded = true;
      for (const core::DecodedSignal& signal : decoded.value()) {
        entry.signals.emplace_back(std::string(signal.name()), signal.value);
        entry.units.push_back(signal.signal != nullptr ? signal.signal->unit()
                                                       : std::string{});
      }
    }
  }

  const transport::FrameTiming timing = transport::frame_timing(frame, timing_);
  ++bus_.frames_received;
  bus_.bytes_received += frame.size();
  bus_.wire_bits += timing.total_bits();
  if (bus_.first_timestamp_ns == 0) {
    bus_.first_timestamp_ns = frame.timestamp_ns();
  }
  bus_.last_timestamp_ns = frame.timestamp_ns();
  if (frame.is_error_frame()) {
    ++bus_.error_frames;
  }

  const std::string node = entry.node.empty() ? std::string("<unknown>") : entry.node;
  auto node_it = std::find_if(nodes_.begin(), nodes_.end(),
                              [&node](const NodeStats& n) { return n.name == node; });
  if (node_it == nodes_.end()) {
    nodes_.push_back(NodeStats{node, 0, 0, 0});
    node_it = std::prev(nodes_.end());
  }
  ++node_it->frames;
  node_it->bytes += frame.size();
  node_it->wire_bits += timing.total_bits();

  recent_ns_.push_back(frame.timestamp_ns());
  while (!recent_ns_.empty() &&
         frame.timestamp_ns() - recent_ns_.front() > 1000000000ULL) {
    recent_ns_.pop_front();
  }

  auto stats_it = std::find_if(messages_.begin(), messages_.end(),
                               [&frame](const MessageStats& m) {
                                 return m.id == frame.id();
                               });
  if (stats_it == messages_.end()) {
    MessageStats fresh;
    fresh.id = frame.id();
    fresh.name = entry.message_name.empty() ? ("id " + hex_id(frame.id()))
                                            : entry.message_name;
    fresh.node = node;
    fresh.first_ns = frame.timestamp_ns();
    if (message != nullptr) {
      const auto attribute = message->attributes().find("GenMsgCycleTime");
      if (attribute != message->attributes().end()) {
        fresh.expected_cycle_ms =
            static_cast<std::uint32_t>(attribute->second.as_int());
      }
    }
    messages_.push_back(std::move(fresh));
    stats_it = std::prev(messages_.end());
    std::sort(messages_.begin(), messages_.end(),
              [](const MessageStats& a, const MessageStats& b) {
                return a.id.value() < b.id.value();
              });
    stats_it = std::find_if(messages_.begin(), messages_.end(),
                            [&frame](const MessageStats& m) {
                              return m.id == frame.id();
                            });
  }

  if (stats_it->count > 0) {
    const double period_ms =
        static_cast<double>(frame.timestamp_ns() - stats_it->last_ns) / 1e6;
    // A running mean rather than a stored history: the dashboard may watch a
    // bus for hours and keeping every interval would grow without bound.
    const double n = static_cast<double>(stats_it->count);
    stats_it->mean_period_ms =
        (stats_it->mean_period_ms * (n - 1.0) + period_ms) / n;
    if (stats_it->min_period_ms == 0.0 || period_ms < stats_it->min_period_ms) {
      stats_it->min_period_ms = period_ms;
    }
    stats_it->max_period_ms = std::max(stats_it->max_period_ms, period_ms);
    // Jitter as peak deviation from the mean, which is what a scheduling
    // argument cares about; the standard deviation would hide a single late
    // frame, and a single late frame is the interesting one.
    stats_it->jitter_ms =
        std::max(stats_it->max_period_ms - stats_it->mean_period_ms,
                 stats_it->mean_period_ms - stats_it->min_period_ms);
  }
  ++stats_it->count;
  stats_it->last_ns = frame.timestamp_ns();
  if (entry.decoded) {
    stats_it->signals = entry.signals;
    stats_it->units = entry.units;
  }

  if (!selected_.empty() && entry.decoded) {
    for (std::size_t i = 0; i < entry.signals.size(); ++i) {
      if (entry.message_name + "." + entry.signals[i].first == selected_) {
        history_.push(entry.signals[i].second);
        break;
      }
    }
  }

  // Pausing freezes the trace but not the statistics: an engineer who pauses
  // to read a frame still wants the bus load to be true when they resume.
  if (paused_) {
    return;
  }
  if (!passes_filter(entry)) {
    ++filtered_;
    return;
  }
  trace_.push_back(std::move(entry));
  while (trace_.size() > kTraceCapacity) {
    trace_.pop_front();
  }
}

bool ViewModel::passes_filter(const TraceEntry& entry) const {
  if (filter_.empty()) {
    return true;
  }
  const std::string needle = lowered(filter_);
  if (lowered(entry.message_name).find(needle) != std::string::npos) {
    return true;
  }
  if (lowered(hex_id(entry.id)).find(needle) != std::string::npos) {
    return true;
  }
  if (lowered(entry.node).find(needle) != std::string::npos) {
    return true;
  }
  for (const auto& signal : entry.signals) {
    if (lowered(signal.first).find(needle) != std::string::npos) {
      return true;
    }
  }
  return false;
}

Snapshot ViewModel::snapshot() const {
  const std::lock_guard<std::mutex> lock(mutex_);
  Snapshot out;
  out.trace.assign(trace_.begin(), trace_.end());
  out.messages = messages_;
  out.nodes = nodes_;
  out.bus = bus_;
  out.bus_load = bus_.bus_load(timing_);
  out.frames_per_second = static_cast<double>(recent_ns_.size());
  out.total_frames = total_;
  out.dropped_by_filter = filtered_;
  out.now_ns = now_ns_;
  out.paused = paused_;
  out.filter = filter_;
  return out;
}

void ViewModel::set_paused(bool paused) {
  const std::lock_guard<std::mutex> lock(mutex_);
  paused_ = paused;
}

bool ViewModel::paused() const {
  const std::lock_guard<std::mutex> lock(mutex_);
  return paused_;
}

void ViewModel::set_filter(std::string filter) {
  const std::lock_guard<std::mutex> lock(mutex_);
  filter_ = std::move(filter);
}

std::string ViewModel::filter() const {
  const std::lock_guard<std::mutex> lock(mutex_);
  return filter_;
}

void ViewModel::clear() {
  const std::lock_guard<std::mutex> lock(mutex_);
  trace_.clear();
  messages_.clear();
  nodes_.clear();
  recent_ns_.clear();
  history_.clear();
  bus_ = {};
  total_ = 0;
  filtered_ = 0;
}

void ViewModel::select_signal(std::string qualified_name) {
  const std::lock_guard<std::mutex> lock(mutex_);
  if (selected_ != qualified_name) {
    history_.clear();
  }
  selected_ = std::move(qualified_name);
}

std::string ViewModel::selected_signal() const {
  const std::lock_guard<std::mutex> lock(mutex_);
  return selected_;
}

SignalHistory ViewModel::history() const {
  const std::lock_guard<std::mutex> lock(mutex_);
  return history_;
}

std::vector<std::string> ViewModel::signal_names() const {
  std::vector<std::string> out;
  if (database_ == nullptr) {
    return out;
  }
  for (const core::Message& message : database_->messages()) {
    for (const core::Signal& signal : message.signals()) {
      out.push_back(message.name() + "." + signal.name());
    }
  }
  std::sort(out.begin(), out.end());
  return out;
}

}  // namespace canforge::tui
