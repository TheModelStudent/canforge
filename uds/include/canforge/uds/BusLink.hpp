// SPDX-License-Identifier: MIT
#ifndef CANFORGE_UDS_BUSLINK_HPP
#define CANFORGE_UDS_BUSLINK_HPP

/// Runs the poll-driven UDS client on a real bus.
///
/// `Client` never touches a bus: it turns requests into frames and frames into
/// responses, and leaves moving those frames to its caller. BusLink is that
/// caller for anything behind an `IBus` -- SocketCAN for an ECU on the wire, a
/// VirtualBus for a test. One call to `run()` carries a request the client has
/// started all the way to its response:
///
///   client.read_data_by_identifier(0xF190, link.now_ns());
///   auto response = link.run(std::chrono::seconds(2));
///
/// The clock is the wall clock by default. Every timestamp handed to the client
/// must come from `now_ns()`, or its P2 and S3 arithmetic compares two different
/// clocks.

#include <chrono>
#include <cstdint>
#include <functional>

#include "canforge/core/Result.hpp"
#include "canforge/transport/IBus.hpp"
#include "canforge/uds/Uds.hpp"

namespace canforge::uds {

class BusLink {
 public:
  /// Nanoseconds on whatever clock the bus lives on.
  using Clock = std::function<std::uint64_t()>;
  /// Called once per turn of the loop in `run()` and `idle()`.
  using Hook = std::function<void(std::uint64_t now_ns)>;

  BusLink(transport::IBus& bus, Client& client);

  /// Replace the wall clock. A test on a VirtualBus passes the medium's clock,
  /// so that simulated seconds cost no real time.
  void set_clock(Clock clock) { clock_ = std::move(clock); }

  /// A single-threaded test uses this to run the ECU end of a virtual bus
  /// between two polls of the client.
  void set_hook(Hook hook) { hook_ = std::move(hook); }

  std::uint64_t now_ns() const { return clock_(); }

  /// Ask the bus to deliver only the identifier the server answers on. On
  /// SocketCAN the kernel then drops everything else, which matters on a bus
  /// that carries hundreds of other frames a second. Call once the bus is open.
  Status install_filter();

  /// Drive the request the client has in flight until it completes or fails.
  /// The client applies P2 and P2* itself; `timeout` is the outer limit for the
  /// whole exchange, including any number of responsePending answers.
  Result<Response> run(std::chrono::milliseconds timeout);

  /// Keep the link turning for `duration` with no request in flight. The
  /// client's TesterPresent keepalive goes out from here, so a non-default
  /// session survives a pause between two requests.
  Status idle(std::chrono::milliseconds duration);

 private:
  /// One turn: send what the client has queued, then wait up to a millisecond
  /// for a frame and hand it over if it is addressed to the client.
  Status turn();

  transport::IBus& bus_;
  Client& client_;
  Clock clock_;
  Hook hook_;
};

}  // namespace canforge::uds

#endif  // CANFORGE_UDS_BUSLINK_HPP
