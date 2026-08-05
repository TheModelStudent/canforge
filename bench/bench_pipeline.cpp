// SPDX-License-Identifier: MIT
//
// The three numbers that are not the codec: how long a large DBC takes to
// parse, how many frames per second the virtual bus sustains, and how fast a
// whole decode pipeline (bus -> database -> named signals) runs.

#include <chrono>
#include <cstdio>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "canforge/dbc/Parser.hpp"
#include "canforge/dbc/Writer.hpp"
#include "canforge/transport/VirtualBus.hpp"

namespace {

using clock_type = std::chrono::steady_clock;

double seconds_since(clock_type::time_point start) {
  return std::chrono::duration<double>(clock_type::now() - start).count();
}

/// Build a synthetic DBC with roughly `signal_target` signals, so the parse
/// benchmark runs on something the size of a real vehicle database instead of
/// on the four-message test file.
std::string synthetic_dbc(std::size_t signal_target) {
  std::ostringstream os;
  os << "VERSION \"synthetic\"\n\n\nNS_ : \n\nBS_:\n\nBU_:";
  const std::size_t nodes = 12;
  for (std::size_t n = 0; n < nodes; ++n) {
    os << " NODE" << n;
  }
  os << "\n\n";

  const std::size_t per_message = 8;
  const std::size_t messages = (signal_target + per_message - 1) / per_message;
  for (std::size_t m = 0; m < messages; ++m) {
    os << "BO_ " << (256 + m) << " MSG" << m << ": 8 NODE" << (m % nodes) << "\n";
    for (std::size_t s = 0; s < per_message; ++s) {
      // Alternate byte orders and signedness so the parser exercises both
      // paths rather than one hot line.
      os << " SG_ MSG" << m << "_SIG" << s << " : " << (s * 8) << "|8@"
         << (s % 2) << (s % 3 == 0 ? '-' : '+') << " (0." << (s + 1) << ",-40)"
         << " [0|255] \"unit" << s << "\" NODE" << ((m + 1) % nodes) << "\n";
    }
    os << "\n";
  }
  for (std::size_t m = 0; m < messages; ++m) {
    os << "BA_ \"GenMsgCycleTime\" BO_ " << (256 + m) << " " << (10 + (m % 100))
       << ";\n";
    os << "CM_ BO_ " << (256 + m) << " \"synthetic message " << m << "\";\n";
  }
  os << "BA_DEF_ BO_ \"GenMsgCycleTime\" INT 0 65535;\n";
  os << "BA_DEF_DEF_ \"GenMsgCycleTime\" 100;\n";
  return os.str();
}

}  // namespace

int main() {
  using namespace canforge;

  const std::string text = synthetic_dbc(5000);
  std::printf("canforge pipeline benchmark\n\n");
  std::printf("  synthetic DBC: %zu KiB of text\n", text.size() / 1024);

  {
    // Warm once so the measurement is not dominated by first-touch page faults.
    static_cast<void>(dbc::parse_string(text, "warm.dbc"));

    constexpr int kRuns = 10;
    const auto start = clock_type::now();
    std::size_t signals = 0;
    std::size_t messages = 0;
    for (int i = 0; i < kRuns; ++i) {
      auto parsed = dbc::parse_string(text, "bench.dbc");
      messages = parsed.database.messages().size();
      signals = 0;
      for (const auto& m : parsed.database.messages()) {
        signals += m.signals().size();
      }
    }
    const double elapsed = seconds_since(start) / kRuns;
    std::printf("  parse %zu messages / %zu signals   %8.1f ms   %6.2f MB/s\n",
                messages, signals, elapsed * 1e3,
                static_cast<double>(text.size()) / elapsed / 1e6);

    auto parsed = dbc::parse_string(text, "bench.dbc");
    const auto write_start = clock_type::now();
    std::string out;
    for (int i = 0; i < kRuns; ++i) {
      out = dbc::write_string(parsed.database);
    }
    std::printf("  write the same database          %8.1f ms   (%zu KiB out)\n",
                seconds_since(write_start) / kRuns * 1e3, out.size() / 1024);

    const auto validate_start = clock_type::now();
    for (int i = 0; i < kRuns; ++i) {
      static_cast<void>(parsed.database.validate());
    }
    std::printf("  validate()                       %8.1f ms\n",
                seconds_since(validate_start) / kRuns * 1e3);
  }

  {
    auto medium = transport::VirtualMedium::create({500000, 0});
    auto a = medium->attach("A");
    auto b = medium->attach("B");
    static_cast<void>(a->open());
    static_cast<void>(b->open());
    medium->set_transmit_queue_limit(200000);
    medium->set_receive_queue_limit(400000);

    const auto id = core::CanId::standard(0x123).value();
    const auto frame = core::FdFrame::make(id, nullptr, 8).value();
    constexpr std::size_t kFrames = 200000;

    const auto start = clock_type::now();
    for (std::size_t i = 0; i < kFrames; ++i) {
      static_cast<void>(a->send(frame));
    }
    medium->drain(kFrames + 16);
    std::size_t received = 0;
    while (b->receive(std::chrono::nanoseconds(0))) {
      ++received;
    }
    const double elapsed = seconds_since(start);
    std::printf("\n  virtual bus: %zu frames arbitrated and delivered\n", received);
    std::printf("  wall time %.3f s -> %8.2f M frames/s of simulation\n", elapsed,
                static_cast<double>(kFrames) / elapsed / 1e6);
    std::printf("  simulated bus time %.3f s (a 500 kbit/s bus needs that long)\n",
                static_cast<double>(medium->now_ns()) / 1e9);
    std::printf("  speedup over real time: %.0fx\n",
                static_cast<double>(medium->now_ns()) / 1e9 / elapsed);
  }

  {
    auto parsed = dbc::parse_string(text, "bench.dbc");
    const core::Message* message = &parsed.database.messages().front();
    const auto frame =
        core::Frame::make(message->id(), {1, 2, 3, 4, 5, 6, 7, 8}).value();

    constexpr std::size_t kRounds = 200000;
    const auto start = clock_type::now();
    double sink = 0.0;
    std::size_t decoded = 0;
    for (std::size_t i = 0; i < kRounds; ++i) {
      // The allocation-free path, the one a real receive loop uses.
      static_cast<void>(message->for_each_active_signal(
          frame, [&](const core::Signal&, std::uint64_t, double value) {
            sink += value;
            ++decoded;
          }));
    }
    const double elapsed = seconds_since(start);
    std::printf("\n  full pipeline: message lookup + decode of every signal\n");
    std::printf("  %zu signals in %.3f s -> %8.2f M signals/s\n", decoded, elapsed,
                static_cast<double>(decoded) / elapsed / 1e6);
    std::printf("  checksum %.3f\n", sink);
  }
  return 0;
}
