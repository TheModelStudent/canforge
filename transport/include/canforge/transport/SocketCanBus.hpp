// SPDX-License-Identifier: MIT
#ifndef CANFORGE_TRANSPORT_SOCKETCANBUS_HPP
#define CANFORGE_TRANSPORT_SOCKETCANBUS_HPP

/// Linux SocketCAN backend.
///
/// The whole implementation is guarded by `__linux__`; on any other platform
/// the class still exists and `open()` returns `TransportUnsupported`, so
/// calling code and the test suite compile everywhere.
///
/// Development is against the kernel's virtual CAN interface, which needs no
/// hardware:
///
///     sudo modprobe vcan
///     sudo ip link add dev vcan0 type vcan
///     sudo ip link set up vcan0

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

#include "canforge/transport/IBus.hpp"

namespace canforge::transport {

struct SocketCanOptions {
  /// Ask for CAN_RAW_FD_FRAMES. When the interface does not support it, open()
  /// fails unless `fd_optional` is set, in which case it falls back silently
  /// to classic frames.
  bool enable_fd = true;
  bool fd_optional = true;

  /// Deliver frames this socket sent back to itself, which is SocketCAN's
  /// default and lets a single-process test see its own traffic.
  bool loopback = true;
  /// Also receive the frames *this* socket sent. Off by default, matching
  /// SocketCAN, so a sender does not see its own echo.
  bool receive_own_messages = false;

  /// Subscribe to the driver's error frames via CAN_ERR_FILTER.
  bool error_frames = true;
  std::uint32_t error_mask = 0x1FFFFFFFu;  // CAN_ERR_MASK: everything

  /// Ask the kernel for hardware timestamps through SO_TIMESTAMPING, falling
  /// back to the software timestamp when the driver cannot provide them. vcan
  /// has no hardware clock, so the fallback is the normal path in development.
  bool hardware_timestamps = true;

  /// Socket receive buffer in bytes; 0 leaves the system default.
  int receive_buffer_bytes = 0;

  /// Used only for the bus-load estimate: SocketCAN does not report the
  /// configured bitrate on a raw socket.
  BusTiming timing;
};

class SocketCanBus final : public IBus {
 public:
  explicit SocketCanBus(std::string interface_name, SocketCanOptions options = {});
  ~SocketCanBus() override;

  Status open() override;
  void close() noexcept override;
  bool is_open() const noexcept override { return fd_ >= 0; }

  Status send(const FdFrame& frame) override;
  using IBus::send;

  Result<FdFrame> receive(std::chrono::nanoseconds timeout) override;

  Status set_filters(const std::vector<Filter>& filters) override;
  const std::vector<Filter>& filters() const noexcept override { return filters_; }

  const BusStatistics& statistics() const noexcept override { return stats_; }
  void reset_statistics() noexcept override { stats_ = {}; }

  std::string_view name() const noexcept override { return interface_; }
  BusTiming timing() const noexcept override { return options_.timing; }

  bool fd_enabled() const noexcept { return fd_enabled_; }
  /// True when the kernel is providing hardware timestamps rather than the
  /// software fallback.
  bool hardware_timestamps_active() const noexcept { return hw_timestamps_; }

  /// Is the named interface present and up? Useful for skipping tests.
  static bool interface_exists(const std::string& name) noexcept;

 private:
  Status apply_filters();

  std::string interface_;
  SocketCanOptions options_;
  std::vector<Filter> filters_;
  BusStatistics stats_;
  int fd_ = -1;
  bool fd_enabled_ = false;
  bool hw_timestamps_ = false;
};

}  // namespace canforge::transport

#endif  // CANFORGE_TRANSPORT_SOCKETCANBUS_HPP
