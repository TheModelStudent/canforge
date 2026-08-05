// SPDX-License-Identifier: MIT
#ifndef CANFORGE_TRANSPORT_LOGREPLAYBUS_HPP
#define CANFORGE_TRANSPORT_LOGREPLAYBUS_HPP

/// Replays a recorded log as if it were live traffic.
///
/// The log's own inter-frame timing is preserved and scaled by a speed
/// multiplier, so a capture recorded at 2 kHz can be stepped through at 0.1x
/// to watch a fault develop, or run at 100x to reach the interesting minute of
/// a long trace quickly.
///
/// Two clock modes:
///
///   Virtual   the default. Time advances only when receive() is called, so a
///             test replays an hour-long log instantly and deterministically.
///   Realtime  sleeps between frames, for feeding a dashboard or a device
///             under test at the original rate.

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "canforge/transport/IBus.hpp"
#include "canforge/transport/LogFormat.hpp"

namespace canforge::transport {

struct ReplayOptions {
  /// 1.0 replays at the recorded rate, 2.0 at twice the speed, 0.5 at half.
  double speed = 1.0;
  /// Sleep between frames rather than advancing a virtual clock.
  bool realtime = false;
  /// Start over from the beginning once the log is exhausted.
  bool loop = false;
  /// Skip everything before this offset into the recording.
  std::chrono::nanoseconds start_offset{0};
  BusTiming timing;
};

class LogReplayBus final : public IBus {
 public:
  /// Reads the whole log up front. A trace worth replaying fits in memory, and
  /// having the full timeline is what makes seeking and looping possible.
  static Result<std::unique_ptr<LogReplayBus>> from_file(const std::string& path,
                                                         ReplayOptions options = {});
  static std::unique_ptr<LogReplayBus> from_records(std::vector<LogRecord> records,
                                                    ReplayOptions options = {});

  Status open() override;
  void close() noexcept override;
  bool is_open() const noexcept override { return open_; }

  /// A replayed log is read-only; sending is rejected rather than silently
  /// discarded, so a caller that mixes up its buses finds out.
  Status send(const FdFrame& frame) override;
  using IBus::send;

  Result<FdFrame> receive(std::chrono::nanoseconds timeout) override;

  Status set_filters(const std::vector<Filter>& filters) override;
  const std::vector<Filter>& filters() const noexcept override { return filters_; }

  const BusStatistics& statistics() const noexcept override { return stats_; }
  void reset_statistics() noexcept override { stats_ = {}; }

  std::string_view name() const noexcept override { return name_; }
  BusTiming timing() const noexcept override { return options_.timing; }

  std::size_t size() const noexcept { return records_.size(); }
  std::size_t position() const noexcept { return index_; }
  bool at_end() const noexcept { return index_ >= records_.size(); }
  /// Virtual time elapsed since the start of the replay.
  std::chrono::nanoseconds elapsed() const noexcept {
    return std::chrono::nanoseconds(virtual_now_ns_);
  }
  void rewind() noexcept;
  const std::vector<LogRecord>& records() const noexcept { return records_; }

 private:
  LogReplayBus(std::vector<LogRecord> records, ReplayOptions options);

  std::vector<LogRecord> records_;
  ReplayOptions options_;
  std::vector<Filter> filters_;
  BusStatistics stats_;
  std::string name_ = "replay";
  std::uint64_t first_ns_ = 0;
  std::uint64_t virtual_now_ns_ = 0;
  std::size_t index_ = 0;
  bool open_ = false;
};

}  // namespace canforge::transport

#endif  // CANFORGE_TRANSPORT_LOGREPLAYBUS_HPP
