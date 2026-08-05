// SPDX-License-Identifier: MIT
//
// Soak test: run the simulator and the dashboard's view model together for a
// long time and assert that memory is flat.
//
// The failure this catches is the one unit tests never do -- a container that
// grows without bound. Every accumulating structure in the project is capped
// (the trace ring buffer, the signal history, the frames-per-second window,
// the per-message running mean), and this is what proves the caps are real
// rather than intended.
//
//   ./canforge_soak <seconds> <config> <database>
//
// Resident set is read from /proc/self/statm, so this is Linux-only; on other
// platforms it runs the loop and skips the assertion.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "canforge/dbc/Parser.hpp"
#include "canforge/sim/Config.hpp"
#include "canforge/sim/Simulator.hpp"
#include "canforge/tui/ViewModel.hpp"

#if defined(__linux__)
#include <unistd.h>
#endif

namespace {

/// Resident set size in KiB, or zero when it cannot be read.
std::size_t resident_kib() {
#if defined(__linux__)
  std::ifstream in("/proc/self/statm");
  std::size_t total = 0;
  std::size_t resident = 0;
  if (in >> total >> resident) {
    return resident * (static_cast<std::size_t>(::sysconf(_SC_PAGESIZE)) / 1024u);
  }
#endif
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  using namespace canforge;

  const double seconds = argc > 1 ? std::strtod(argv[1], nullptr) : 60.0;
  const std::string config_path = argc > 2 ? argv[2] : "sim/tests/data/vehicle.cfg";
  const std::string database_path =
      argc > 3 ? argv[3] : "dbc/tests/data/powertrain.dbc";

  auto db_parsed = dbc::parse_file(database_path);
  if (!db_parsed) {
    std::fprintf(stderr, "soak: cannot read %s\n", database_path.c_str());
    return 1;
  }
  const core::Database database = db_parsed.value().database;

  auto config_parsed = sim::parse_config_file(config_path);
  if (!config_parsed || config_parsed.value().diagnostics.has_errors()) {
    std::fprintf(stderr, "soak: cannot read %s\n", config_path.c_str());
    return 1;
  }
  auto simulator = sim::Simulator::build(config_parsed.value().config, database);
  if (!simulator) {
    std::fprintf(stderr, "soak: %.*s\n",
                 static_cast<int>(simulator.error().message().size()),
                 simulator.error().message().data());
    return 1;
  }

  tui::ViewModel model(&database, {config_parsed.value().config.nominal_bitrate,
                                   config_parsed.value().config.data_bitrate});
  model.select_signal("EngineData.EngineSpeed");

  std::fprintf(stderr, "soak: %.0f s of simulated traffic\n", seconds);
  const std::size_t start_kib = resident_kib();

  // Sample the resident set after a warm-up, so allocator growth during the
  // first second is not mistaken for a leak.
  std::size_t warm_kib = 0;
  std::size_t peak_kib = 0;
  std::uint64_t frames = 0;
  const std::uint64_t limit_ns = static_cast<std::uint64_t>(seconds * 1e9);
  const std::uint64_t warm_ns = limit_ns / 10;

  for (std::uint64_t now = 0; now < limit_ns; now += 1000000ULL) {
    for (const core::Frame& frame : simulator.value()->step(now)) {
      model.ingest(frame.widen<64>());
      ++frames;
    }
    // The dashboard snapshots at frame rate; doing it here keeps the copy path
    // in the measurement.
    if ((now % 16000000ULL) == 0u) {
      const tui::Snapshot snap = model.snapshot();
      static_cast<void>(snap.messages.size());
    }
    if (now >= warm_ns && warm_kib == 0) {
      warm_kib = resident_kib();
    }
    if ((now % 1000000000ULL) == 0u) {
      const std::size_t rss = resident_kib();
      peak_kib = std::max(peak_kib, rss);
      std::fprintf(stderr, "  t=%4llus  frames=%-10llu  rss=%zu KiB\n",
                   static_cast<unsigned long long>(now / 1000000000ULL),
                   static_cast<unsigned long long>(frames), rss);
    }
  }

  const std::size_t end_kib = resident_kib();
  std::fprintf(stderr,
               "soak: %llu frames, rss start %zu KiB, after warm-up %zu KiB, "
               "end %zu KiB, peak %zu KiB\n",
               static_cast<unsigned long long>(frames), start_kib, warm_kib,
               end_kib, peak_kib);

  if (warm_kib == 0 || end_kib == 0) {
    std::fprintf(stderr, "soak: no resident set reading available; loop only\n");
    return 0;
  }
  // Allow 8 MiB of drift for allocator behaviour. A genuine unbounded growth
  // shows up as tens of megabytes within a minute at this frame rate.
  const std::size_t allowed = 8u * 1024u;
  if (end_kib > warm_kib + allowed) {
    std::fprintf(stderr,
                 "soak: FAILED, resident set grew by %zu KiB after warm-up\n",
                 end_kib - warm_kib);
    return 1;
  }
  std::fprintf(stderr, "soak: memory flat within %zu KiB\n", allowed);
  return 0;
}
