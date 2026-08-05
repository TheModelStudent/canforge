// SPDX-License-Identifier: MIT
#ifndef CANFORGE_TUI_VIEWMODEL_HPP
#define CANFORGE_TUI_VIEWMODEL_HPP

/// Everything the dashboard shows, with no ftxui in sight.
///
/// The split is deliberate. A terminal UI is close to untestable in CI, but
/// almost all of the logic worth testing -- period and jitter measurement,
/// filtering, ring buffers, sparkline scaling, per-node statistics -- has
/// nothing to do with drawing. That logic lives here and is covered by
/// ordinary unit tests; `Dashboard.cpp` is then a thin layer that turns this
/// state into ftxui elements and does nothing else.
///
/// Thread safety: `ingest()` is called from the bus reader thread and the
/// render thread reads through `snapshot()`. The mutex is held only for the
/// duration of a copy, never across rendering, so a slow terminal cannot stall
/// the bus.

#include <chrono>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "canforge/core/Database.hpp"
#include "canforge/core/Frame.hpp"
#include "canforge/transport/IBus.hpp"

namespace canforge::tui {

/// A frame plus everything the trace view needs to draw it without touching
/// the database again.
struct TraceEntry {
  std::uint64_t timestamp_ns = 0;
  core::CanId id;
  std::uint8_t dlc = 0;
  std::array<std::uint8_t, 64> data{};
  std::size_t size = 0;
  std::string message_name;
  std::string node;
  bool is_error = false;
  bool decoded = false;
  std::vector<std::pair<std::string, double>> signals;
  std::vector<std::string> units;
};

/// One row of the grouped view: the thing an engineer actually watches.
struct MessageStats {
  core::CanId id;
  std::string name;
  std::string node;
  std::uint64_t count = 0;
  std::uint64_t first_ns = 0;
  std::uint64_t last_ns = 0;
  /// From the database's GenMsgCycleTime attribute; zero when not declared.
  std::uint32_t expected_cycle_ms = 0;
  /// Measured mean period and the peak absolute deviation from it.
  double mean_period_ms = 0.0;
  double jitter_ms = 0.0;
  double min_period_ms = 0.0;
  double max_period_ms = 0.0;
  std::vector<std::pair<std::string, double>> signals;
  std::vector<std::string> units;

  /// True when the measured period is more than 20% off the declared one.
  /// The number is a convention, not a standard; it is the threshold at which
  /// a cyclic message is worth a second look.
  bool period_suspect() const noexcept {
    return expected_cycle_ms != 0 && mean_period_ms > 0.0 &&
           std::abs(mean_period_ms - expected_cycle_ms) >
               0.2 * static_cast<double>(expected_cycle_ms);
  }
};

struct NodeStats {
  std::string name;
  std::uint64_t frames = 0;
  std::uint64_t bytes = 0;
  std::uint64_t wire_bits = 0;
};

/// Fixed-capacity history for one signal, for the sparkline view.
class SignalHistory {
 public:
  void push(double value);
  const std::deque<double>& samples() const noexcept { return samples_; }
  double minimum() const noexcept { return min_; }
  double maximum() const noexcept { return max_; }
  bool empty() const noexcept { return samples_.empty(); }
  void clear();

  /// Map the history onto `rows` sparkline levels, 0 being the bottom.
  /// Returns an empty vector when there is nothing to draw. A flat signal maps
  /// to the middle row rather than to zero, so a constant does not look like a
  /// dropout.
  std::vector<int> levels(std::size_t rows) const;

  static constexpr std::size_t kCapacity = 512;

 private:
  std::deque<double> samples_;
  double min_ = 0.0;
  double max_ = 0.0;
};

/// Immutable copy handed to the renderer.
struct Snapshot {
  std::vector<TraceEntry> trace;
  std::vector<MessageStats> messages;
  std::vector<NodeStats> nodes;
  transport::BusStatistics bus;
  double bus_load = 0.0;
  double frames_per_second = 0.0;
  std::uint64_t total_frames = 0;
  std::uint64_t dropped_by_filter = 0;
  std::uint64_t now_ns = 0;
  bool paused = false;
  std::string filter;
};

class ViewModel {
 public:
  explicit ViewModel(const core::Database* database, transport::BusTiming timing)
      : database_(database), timing_(timing) {}

  /// Called from the bus thread. Cheap: parsing and formatting happen here so
  /// the render thread only copies.
  void ingest(const core::FdFrame& frame);

  Snapshot snapshot() const;

  void set_paused(bool paused);
  bool paused() const;
  /// Substring match against the message name or the hex identifier. Empty
  /// accepts everything. Filtering happens on ingest for the trace but never
  /// hides a message from the grouped view, because a filtered-out message
  /// still counts toward bus load.
  void set_filter(std::string filter);
  std::string filter() const;

  void clear();

  /// Track one signal for the plot view. Names are "Message.Signal".
  void select_signal(std::string qualified_name);
  std::string selected_signal() const;
  SignalHistory history() const;

  /// Every "Message.Signal" the database offers, sorted, for the picker.
  std::vector<std::string> signal_names() const;

  static constexpr std::size_t kTraceCapacity = 2000;

 private:
  bool passes_filter(const TraceEntry& entry) const;

  const core::Database* database_;
  transport::BusTiming timing_;

  mutable std::mutex mutex_;
  std::deque<TraceEntry> trace_;
  std::vector<MessageStats> messages_;
  std::vector<NodeStats> nodes_;
  transport::BusStatistics bus_;
  std::deque<std::uint64_t> recent_ns_;  ///< For the frames-per-second figure.
  std::string filter_;
  std::string selected_;
  SignalHistory history_;
  std::uint64_t total_ = 0;
  std::uint64_t filtered_ = 0;
  std::uint64_t now_ns_ = 0;
  bool paused_ = false;
};

}  // namespace canforge::tui

#endif  // CANFORGE_TUI_VIEWMODEL_HPP
