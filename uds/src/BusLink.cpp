// SPDX-License-Identifier: MIT
#include "canforge/uds/BusLink.hpp"

#include <utility>
#include <vector>

namespace canforge::uds {

using core::Error;
using core::ErrorCode;

namespace {

std::uint64_t to_ns(std::chrono::milliseconds ms) {
  return static_cast<std::uint64_t>(ms.count() < 0 ? 0 : ms.count()) * 1000000ULL;
}

}  // namespace

BusLink::BusLink(transport::IBus& bus, Client& client) : bus_(bus), client_(client) {
  // Zero at construction: timestamps stay small and readable in a debugger, and
  // nothing downstream depends on what the epoch is.
  const auto start = std::chrono::steady_clock::now();
  clock_ = [start]() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - start)
            .count());
  };
}

Status BusLink::install_filter() {
  return bus_.set_filters(
      {transport::Filter::exact(client_.config().transport.address.rx_id)});
}

Status BusLink::turn() {
  const std::uint64_t now = now_ns();
  for (const core::FdFrame& frame : client_.poll(now)) {
    CANFORGE_CHECK(bus_.send(frame));
  }
  if (hook_) {
    hook_(now);
  }
  // The wait is what paces the loop: a millisecond is short against every UDS
  // timeout and long enough not to spin a core.
  const auto received = bus_.receive(std::chrono::milliseconds(1));
  if (received) {
    const core::FdFrame& frame = received.value();
    // Without a kernel filter (a VirtualBus, or a caller that skipped
    // install_filter) other traffic arrives here too.
    if (!frame.is_error_frame() &&
        frame.id() == client_.config().transport.address.rx_id) {
      client_.on_frame(frame, now_ns());
    }
  } else if (received.error().code() != ErrorCode::TransportTimeout) {
    return received.error();
  }
  return core::ok();
}

Result<Response> BusLink::run(std::chrono::milliseconds timeout) {
  const std::uint64_t deadline = now_ns() + to_ns(timeout);
  while (client_.busy()) {
    if (now_ns() >= deadline) {
      return Error(ErrorCode::TransportTimeout,
                   "the exchange did not finish within the time allowed");
    }
    CANFORGE_CHECK(turn());
  }
  // A request that fails inside the client's first poll never makes it busy.
  if (client_.state() == RequestState::Failed) {
    return client_.failure();
  }
  if (client_.state() != RequestState::Complete) {
    return Error(ErrorCode::InvalidArgument, "no request was started on the client");
  }
  return client_.response();
}

Status BusLink::idle(std::chrono::milliseconds duration) {
  const std::uint64_t until = now_ns() + to_ns(duration);
  while (now_ns() < until) {
    CANFORGE_CHECK(turn());
  }
  return core::ok();
}

}  // namespace canforge::uds
