// SPDX-License-Identifier: MIT
#ifndef CANFORGE_TUI_DASHBOARD_HPP
#define CANFORGE_TUI_DASHBOARD_HPP

/// The ftxui dashboard. Everything ftxui-specific is behind this header so
/// that ViewModel -- and its tests -- never sees the dependency.

#include <functional>
#include <memory>
#include <string>

#include "canforge/core/Database.hpp"
#include "canforge/core/Result.hpp"
#include "canforge/tui/ViewModel.hpp"

namespace canforge::tui {

/// Called by the transmit panel: message name, "Signal=value ..." text, and
/// whether to repeat. An empty message name means "stop repeating".
using TransmitFn =
    std::function<core::Status(const std::string&, const std::string&, bool)>;

class Dashboard {
 public:
  Dashboard(ViewModel& model, const core::Database* database, TransmitFn transmit);
  ~Dashboard();
  Dashboard(const Dashboard&) = delete;
  Dashboard& operator=(const Dashboard&) = delete;

  /// Blocks until the user quits. Returns a process exit code.
  int run();
  /// Safe to call from another thread.
  void stop();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace canforge::tui

#endif  // CANFORGE_TUI_DASHBOARD_HPP
