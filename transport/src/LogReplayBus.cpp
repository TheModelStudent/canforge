// SPDX-License-Identifier: MIT
#include "canforge/transport/LogReplayBus.hpp"

#include <algorithm>
#include <thread>

namespace canforge::transport {

LogReplayBus::LogReplayBus(std::vector<LogRecord> records, ReplayOptions options)
    : records_(std::move(records)), options_(options) {
  if (options_.speed <= 0.0) {
    options_.speed = 1.0;
  }
  // The log's timestamps are absolute; the replay works in offsets from the
  // first frame so that a capture made in 2019 still starts at zero.
  std::stable_sort(records_.begin(), records_.end(),
                   [](const LogRecord& a, const LogRecord& b) {
                     return a.frame.timestamp_ns() < b.frame.timestamp_ns();
                   });
  if (!records_.empty()) {
    first_ns_ = records_.front().frame.timestamp_ns();
  }
  rewind();
}

Result<std::unique_ptr<LogReplayBus>> LogReplayBus::from_file(
    const std::string& path, ReplayOptions options) {
  CANFORGE_TRY(auto reader, open_reader(path));
  CANFORGE_TRY(auto records, reader->read_all());
  auto bus = std::unique_ptr<LogReplayBus>(
      new LogReplayBus(std::move(records), options));
  bus->name_ = path;
  return bus;
}

std::unique_ptr<LogReplayBus> LogReplayBus::from_records(
    std::vector<LogRecord> records, ReplayOptions options) {
  return std::unique_ptr<LogReplayBus>(
      new LogReplayBus(std::move(records), options));
}

void LogReplayBus::rewind() noexcept {
  index_ = 0;
  virtual_now_ns_ = 0;
  const auto skip = static_cast<std::uint64_t>(
      options_.start_offset.count() < 0 ? 0 : options_.start_offset.count());
  while (index_ < records_.size() &&
         (records_[index_].frame.timestamp_ns() - first_ns_) < skip) {
    ++index_;
  }
  virtual_now_ns_ = skip;
}

Status LogReplayBus::open() {
  open_ = true;
  return core::ok();
}

void LogReplayBus::close() noexcept { open_ = false; }

Status LogReplayBus::send(const FdFrame&) {
  return core::Error(core::ErrorCode::TransportUnsupported,
                     "a log replay bus is read-only");
}

Status LogReplayBus::set_filters(const std::vector<Filter>& filters) {
  filters_ = filters;
  return core::ok();
}

Result<FdFrame> LogReplayBus::receive(std::chrono::nanoseconds timeout) {
  if (!open_) {
    return core::Error(core::ErrorCode::TransportNotOpen,
                       "the replay bus is not open");
  }
  const auto budget =
      static_cast<std::uint64_t>(timeout.count() < 0 ? 0 : timeout.count());
  const std::uint64_t deadline = virtual_now_ns_ + budget;

  for (;;) {
    if (index_ >= records_.size()) {
      if (options_.loop && !records_.empty()) {
        // Looping restarts the timeline; the offsets stay relative so the
        // gap between the last frame and the first repeat is one frame time.
        index_ = 0;
        continue;
      }
      virtual_now_ns_ = deadline;
      return core::Error(core::ErrorCode::LogEndOfFile,
                         "the recording is exhausted");
    }

    const LogRecord& record = records_[index_];
    const std::uint64_t offset = record.frame.timestamp_ns() - first_ns_;
    // Scaling the offset, not the delay, keeps rounding error from
    // accumulating across a long capture.
    const auto due = static_cast<std::uint64_t>(
        static_cast<double>(offset) / options_.speed);

    if (due > deadline) {
      virtual_now_ns_ = deadline;
      return core::Error(core::ErrorCode::TransportTimeout,
                         "the next recorded frame is not due yet");
    }

    if (options_.realtime && due > virtual_now_ns_) {
      std::this_thread::sleep_for(std::chrono::nanoseconds(due - virtual_now_ns_));
    }
    virtual_now_ns_ = std::max(virtual_now_ns_, due);
    ++index_;

    if (!accepted_by(filters_, record.frame.id())) {
      continue;  // filtered out; keep looking within the same budget
    }

    FdFrame frame = record.frame;
    frame.set_timestamp_ns(virtual_now_ns_);
    ++stats_.frames_received;
    stats_.bytes_received += frame.size();
    stats_.wire_bits += frame_timing(frame, options_.timing).total_bits();
    if (frame.is_error_frame()) {
      ++stats_.error_frames;
    }
    if (stats_.frames_received == 1) {
      stats_.first_timestamp_ns = virtual_now_ns_;
    }
    stats_.last_timestamp_ns = virtual_now_ns_;
    return frame;
  }
}

}  // namespace canforge::transport
