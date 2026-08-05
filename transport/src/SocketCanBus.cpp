// SPDX-License-Identifier: MIT
#include "canforge/transport/SocketCanBus.hpp"

#include <cstring>
#include <utility>

#ifdef __linux__
// The kernel headers are C, and their macros use C casts that -Wold-style-cast
// would reject. They are included as system headers so the project's warning
// set does not apply to them.
#include <errno.h>       // NOLINT
#include <linux/can.h>   // NOLINT
#include <linux/can/error.h>  // NOLINT
#include <linux/can/raw.h>    // NOLINT
#include <net/if.h>      // NOLINT
#include <poll.h>        // NOLINT
#include <sys/ioctl.h>   // NOLINT
#include <sys/socket.h>  // NOLINT
#include <sys/types.h>   // NOLINT
#include <unistd.h>      // NOLINT

#ifndef SO_TIMESTAMPING
#define SO_TIMESTAMPING 37
#endif
#ifndef SOF_TIMESTAMPING_RX_HARDWARE
#define SOF_TIMESTAMPING_RX_HARDWARE (1 << 2)
#define SOF_TIMESTAMPING_RX_SOFTWARE (1 << 3)
#define SOF_TIMESTAMPING_SOFTWARE (1 << 4)
#define SOF_TIMESTAMPING_RAW_HARDWARE (1 << 6)
#endif
#endif  // __linux__

namespace canforge::transport {

SocketCanBus::SocketCanBus(std::string interface_name, SocketCanOptions options)
    : interface_(std::move(interface_name)), options_(options) {}

SocketCanBus::~SocketCanBus() { close(); }

#ifndef __linux__

// Portable stub. Everything compiles; nothing opens.

bool SocketCanBus::interface_exists(const std::string&) noexcept { return false; }

Status SocketCanBus::open() {
  return core::Error(core::ErrorCode::TransportUnsupported,
                     "SocketCAN is only available on Linux");
}
void SocketCanBus::close() noexcept {}
Status SocketCanBus::send(const FdFrame&) {
  return core::Error(core::ErrorCode::TransportUnsupported,
                     "SocketCAN is only available on Linux");
}
Result<FdFrame> SocketCanBus::receive(std::chrono::nanoseconds) {
  return core::Error(core::ErrorCode::TransportUnsupported,
                     "SocketCAN is only available on Linux");
}
Status SocketCanBus::set_filters(const std::vector<Filter>& filters) {
  filters_ = filters;
  return core::ok();
}
Status SocketCanBus::apply_filters() { return core::ok(); }

#else  // __linux__

namespace {

core::Error errno_error(core::ErrorCode code, std::string_view message) {
  return core::Error(code, message,
                     {static_cast<std::uint32_t>(errno), 0});
}

/// SocketCAN packs RTR and ERR into the identifier word alongside EFF.
constexpr std::uint32_t kRtrFlag = 0x40000000u;  // CAN_RTR_FLAG
constexpr std::uint32_t kErrFlag = 0x20000000u;  // CAN_ERR_FLAG

}  // namespace

bool SocketCanBus::interface_exists(const std::string& name) noexcept {
  return if_nametoindex(name.c_str()) != 0;
}

Status SocketCanBus::open() {
  if (fd_ >= 0) {
    return core::ok();
  }
  const int sock = ::socket(PF_CAN, SOCK_RAW, CAN_RAW);
  if (sock < 0) {
    return errno_error(core::ErrorCode::TransportOpenFailed,
                       "cannot create a raw CAN socket");
  }

  const unsigned int index = if_nametoindex(interface_.c_str());
  if (index == 0) {
    ::close(sock);
    return errno_error(core::ErrorCode::TransportOpenFailed,
                       "no such CAN interface; is vcan0 up?");
  }

  if (options_.enable_fd) {
    const int on = 1;
    if (::setsockopt(sock, SOL_CAN_RAW, CAN_RAW_FD_FRAMES, &on, sizeof(on)) == 0) {
      fd_enabled_ = true;
    } else if (!options_.fd_optional) {
      ::close(sock);
      return errno_error(core::ErrorCode::TransportOpenFailed,
                         "the interface does not support CAN FD frames");
    }
  }

  {
    const int on = options_.loopback ? 1 : 0;
    ::setsockopt(sock, SOL_CAN_RAW, CAN_RAW_LOOPBACK, &on, sizeof(on));
  }
  {
    const int on = options_.receive_own_messages ? 1 : 0;
    ::setsockopt(sock, SOL_CAN_RAW, CAN_RAW_RECV_OWN_MSGS, &on, sizeof(on));
  }
  if (options_.error_frames) {
    const can_err_mask_t mask = options_.error_mask;
    ::setsockopt(sock, SOL_CAN_RAW, CAN_RAW_ERR_FILTER, &mask, sizeof(mask));
  }
  if (options_.receive_buffer_bytes > 0) {
    ::setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &options_.receive_buffer_bytes,
                 sizeof(options_.receive_buffer_bytes));
  }
  if (options_.hardware_timestamps) {
    // Ask for hardware timestamps and for the software ones at the same time.
    // vcan has no clock of its own, so in development the software timestamp
    // is what actually arrives; the receive path uses whichever it is given.
    const int flags = SOF_TIMESTAMPING_RX_HARDWARE | SOF_TIMESTAMPING_RAW_HARDWARE |
                      SOF_TIMESTAMPING_RX_SOFTWARE | SOF_TIMESTAMPING_SOFTWARE;
    hw_timestamps_ =
        ::setsockopt(sock, SOL_SOCKET, SO_TIMESTAMPING, &flags, sizeof(flags)) == 0;
    if (!hw_timestamps_) {
      // Fall back to the coarse per-packet software timestamp, which every
      // driver provides.
      const int on = 1;
      ::setsockopt(sock, SOL_SOCKET, SO_TIMESTAMPNS, &on, sizeof(on));
    }
  }

  sockaddr_can addr{};
  addr.can_family = AF_CAN;
  addr.can_ifindex = static_cast<int>(index);
  if (::bind(sock, reinterpret_cast<const sockaddr*>(&addr),  // NOLINT
             sizeof(addr)) < 0) {
    ::close(sock);
    return errno_error(core::ErrorCode::TransportOpenFailed,
                       "cannot bind the socket to the interface");
  }

  fd_ = sock;
  const Status st = apply_filters();
  if (!st) {
    close();
    return st;
  }
  return core::ok();
}

void SocketCanBus::close() noexcept {
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
  fd_enabled_ = false;
  hw_timestamps_ = false;
}

Status SocketCanBus::apply_filters() {
  if (fd_ < 0) {
    return core::ok();
  }
  if (filters_.empty()) {
    // A zero-length filter list means "receive nothing" to the kernel, so the
    // default of accepting everything is expressed as one wide-open filter.
    can_filter all{};
    all.can_id = 0;
    all.can_mask = 0;
    if (::setsockopt(fd_, SOL_CAN_RAW, CAN_RAW_FILTER, &all, sizeof(all)) < 0) {
      return errno_error(core::ErrorCode::TransportOpenFailed,
                         "cannot install the default CAN filter");
    }
    return core::ok();
  }
  std::vector<can_filter> native;
  native.reserve(filters_.size());
  for (const Filter& f : filters_) {
    can_filter cf{};
    cf.can_id = f.id | (f.invert ? CAN_INV_FILTER : 0u);
    cf.can_mask = f.mask | (f.match_format ? CAN_EFF_FLAG : 0u);
    native.push_back(cf);
  }
  if (::setsockopt(fd_, SOL_CAN_RAW, CAN_RAW_FILTER, native.data(),
                   static_cast<socklen_t>(native.size() * sizeof(can_filter))) < 0) {
    return errno_error(core::ErrorCode::TransportOpenFailed,
                       "cannot install the CAN filters");
  }
  return core::ok();
}

Status SocketCanBus::set_filters(const std::vector<Filter>& filters) {
  filters_ = filters;
  return apply_filters();
}

Status SocketCanBus::send(const FdFrame& frame) {
  if (fd_ < 0) {
    return core::Error(core::ErrorCode::TransportNotOpen, "the socket is closed");
  }
  std::uint32_t id = frame.id().packed();  // already carries CAN_EFF_FLAG
  if (frame.is_remote()) {
    id |= kRtrFlag;
  }
  if (frame.is_error_frame()) {
    id |= kErrFlag;
  }

  ssize_t written = 0;
  if (frame.is_fd()) {
    if (!fd_enabled_) {
      return core::Error(core::ErrorCode::TransportUnsupported,
                         "the socket was not opened with CAN FD support");
    }
    canfd_frame out{};
    out.can_id = id;
    out.len = static_cast<__u8>(frame.size());
    out.flags = static_cast<__u8>(CANFD_FDF | (frame.is_brs() ? CANFD_BRS : 0) |
                                  (frame.is_esi() ? CANFD_ESI : 0));
    std::memcpy(out.data, frame.data(), frame.size());
    written = ::write(fd_, &out, sizeof(out));
    if (written != static_cast<ssize_t>(sizeof(out))) {
      ++stats_.send_errors;
      return errno_error(core::ErrorCode::TransportWriteFailed,
                         "short write on a CAN FD frame");
    }
  } else {
    can_frame out{};
    out.can_id = id;
    out.can_dlc = static_cast<__u8>(frame.size());
    std::memcpy(out.data, frame.data(), frame.size());
    written = ::write(fd_, &out, sizeof(out));
    if (written != static_cast<ssize_t>(sizeof(out))) {
      ++stats_.send_errors;
      return errno_error(core::ErrorCode::TransportWriteFailed,
                         "short write on a classic CAN frame");
    }
  }

  ++stats_.frames_sent;
  stats_.bytes_sent += frame.size();
  stats_.wire_bits += frame_timing(frame, options_.timing).total_bits();
  return core::ok();
}

Result<FdFrame> SocketCanBus::receive(std::chrono::nanoseconds timeout) {
  if (fd_ < 0) {
    return core::Error(core::ErrorCode::TransportNotOpen, "the socket is closed");
  }

  pollfd pfd{};
  pfd.fd = fd_;
  pfd.events = POLLIN;
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(timeout);
  const int poll_ms = ms.count() < 0 ? 0 : static_cast<int>(ms.count());
  const int ready = ::poll(&pfd, 1, poll_ms);
  if (ready == 0) {
    return core::Error(core::ErrorCode::TransportTimeout,
                       "no frame arrived before the timeout expired");
  }
  if (ready < 0) {
    if (errno == EINTR) {
      return core::Error(core::ErrorCode::TransportTimeout,
                         "interrupted while waiting for a frame");
    }
    ++stats_.receive_errors;
    return errno_error(core::ErrorCode::TransportReadFailed, "poll failed");
  }

  // One buffer big enough for either frame type; the return length says which
  // one arrived.
  canfd_frame raw{};
  char control[512];
  iovec iov{};
  iov.iov_base = &raw;
  iov.iov_len = sizeof(raw);
  msghdr msg{};
  msg.msg_iov = &iov;
  msg.msg_iovlen = 1;
  msg.msg_control = control;
  msg.msg_controllen = sizeof(control);

  const ssize_t got = ::recvmsg(fd_, &msg, 0);
  if (got < 0) {
    ++stats_.receive_errors;
    return errno_error(core::ErrorCode::TransportReadFailed, "recvmsg failed");
  }
  if (got != static_cast<ssize_t>(sizeof(can_frame)) &&
      got != static_cast<ssize_t>(sizeof(canfd_frame))) {
    ++stats_.receive_errors;
    return core::Error(core::ErrorCode::TransportReadFailed,
                       "unexpected frame size from the kernel",
                       {static_cast<std::uint32_t>(got), 0});
  }
  const bool is_fd = (got == static_cast<ssize_t>(sizeof(canfd_frame)));

  // Timestamp: SO_TIMESTAMPING delivers three timespecs, of which index 0 is
  // the software one and index 2 the raw hardware one. Prefer hardware, fall
  // back to software, and fall back again to zero.
  std::uint64_t timestamp_ns = 0;
  for (cmsghdr* c = CMSG_FIRSTHDR(&msg); c != nullptr; c = CMSG_NXTHDR(&msg, c)) {
    if (c->cmsg_level != SOL_SOCKET) {
      continue;
    }
    if (c->cmsg_type == SO_TIMESTAMPING) {
      timespec stamps[3];
      std::memcpy(stamps, CMSG_DATA(c), sizeof(stamps));
      const timespec& hw = stamps[2];
      const timespec& sw = stamps[0];
      const timespec& use = (hw.tv_sec != 0 || hw.tv_nsec != 0) ? hw : sw;
      timestamp_ns = static_cast<std::uint64_t>(use.tv_sec) * 1000000000ULL +
                     static_cast<std::uint64_t>(use.tv_nsec);
    } else if (c->cmsg_type == SO_TIMESTAMPNS && timestamp_ns == 0) {
      timespec ts{};
      std::memcpy(&ts, CMSG_DATA(c), sizeof(ts));
      timestamp_ns = static_cast<std::uint64_t>(ts.tv_sec) * 1000000000ULL +
                     static_cast<std::uint64_t>(ts.tv_nsec);
    }
  }

  core::FrameFlags flags = core::FrameFlags::None;
  if ((raw.can_id & kErrFlag) != 0u) {
    flags |= core::FrameFlags::Error;
    ++stats_.error_frames;
  }
  if (!is_fd && (raw.can_id & kRtrFlag) != 0u) {
    flags |= core::FrameFlags::Rtr;
  }
  if (is_fd) {
    flags |= core::FrameFlags::Fd;
    if ((raw.flags & CANFD_BRS) != 0) {
      flags |= core::FrameFlags::Brs;
    }
    if ((raw.flags & CANFD_ESI) != 0) {
      flags |= core::FrameFlags::Esi;
    }
  }

  const std::uint32_t packed = raw.can_id & (CAN_EFF_MASK | CAN_EFF_FLAG);
  auto id = CanId::from_packed(packed);
  if (!id) {
    ++stats_.receive_errors;
    return id.error();
  }

  const std::size_t length =
      is_fd ? static_cast<std::size_t>(raw.len)
            : static_cast<std::size_t>(
                  reinterpret_cast<const can_frame*>(&raw)->can_dlc);  // NOLINT
  auto frame = FdFrame::make(id.value(), raw.data, length, flags, timestamp_ns);
  if (!frame) {
    ++stats_.receive_errors;
    return frame.error();
  }

  ++stats_.frames_received;
  stats_.bytes_received += frame.value().size();
  stats_.wire_bits += frame_timing(frame.value(), options_.timing).total_bits();
  if (stats_.first_timestamp_ns == 0) {
    stats_.first_timestamp_ns = timestamp_ns;
  }
  stats_.last_timestamp_ns = timestamp_ns;
  return frame;
}

#endif  // __linux__

}  // namespace canforge::transport
