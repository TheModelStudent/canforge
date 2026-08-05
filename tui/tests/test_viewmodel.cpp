// SPDX-License-Identifier: MIT
//
// The dashboard's logic, tested without a terminal. Everything here would be
// untestable if it lived inside the ftxui render callbacks, which is the
// reason for the split.

#include "canforge/tui/ViewModel.hpp"

#include <gtest/gtest.h>

#include <string>
#include <thread>
#include <vector>

#include "canforge/dbc/Parser.hpp"

namespace canforge::tui {
namespace {

constexpr std::uint64_t kMs = 1000000ULL;

core::Database load_db() {
  auto parsed =
      dbc::parse_file(std::string(CANFORGE_DBC_TEST_DATA) + "/powertrain.dbc");
  EXPECT_TRUE(parsed.has_value());
  return std::move(parsed).value().database;
}

core::FdFrame engine_frame(const core::Database& db, std::uint64_t at_ns,
                           std::uint16_t rpm_raw = 8000) {
  const core::Message* m = db.find_message("EngineData");
  auto frame = core::FdFrame::make_empty(m->id(), 8, core::FrameFlags::None, at_ns)
                   .value();
  m->find_signal("EngineSpeed")->layout().encode_raw(rpm_raw, frame.data());
  return frame;
}

TEST(SignalHistory, TracksBounds) {
  SignalHistory h;
  EXPECT_TRUE(h.empty());
  h.push(5.0);
  h.push(1.0);
  h.push(9.0);
  EXPECT_EQ(h.samples().size(), 3u);
  EXPECT_DOUBLE_EQ(h.minimum(), 1.0);
  EXPECT_DOUBLE_EQ(h.maximum(), 9.0);
}

TEST(SignalHistory, BoundsFollowTheWindowWhenSamplesFallOff) {
  // The extreme leaving the window has to update the bound. An incremental
  // minimum would stay wrong for the rest of the session.
  SignalHistory h;
  h.push(-1000.0);
  for (std::size_t i = 0; i < SignalHistory::kCapacity; ++i) {
    h.push(1.0);
  }
  EXPECT_EQ(h.samples().size(), SignalHistory::kCapacity);
  EXPECT_DOUBLE_EQ(h.minimum(), 1.0) << "the outlier should have aged out";
  EXPECT_DOUBLE_EQ(h.maximum(), 1.0);
}

TEST(SignalHistory, IgnoresNonFiniteSamples) {
  SignalHistory h;
  h.push(std::numeric_limits<double>::quiet_NaN());
  h.push(std::numeric_limits<double>::infinity());
  EXPECT_TRUE(h.empty());
}

TEST(SignalHistory, LevelsSpanTheRequestedRows) {
  SignalHistory h;
  for (int i = 0; i <= 100; ++i) {
    h.push(static_cast<double>(i));
  }
  const std::vector<int> levels = h.levels(8);
  ASSERT_EQ(levels.size(), 101u);
  EXPECT_EQ(levels.front(), 0);
  EXPECT_EQ(levels.back(), 7);
  for (const int level : levels) {
    EXPECT_GE(level, 0);
    EXPECT_LE(level, 7);
  }
}

TEST(SignalHistory, AConstantSignalDrawsDownTheMiddle) {
  // Mapping a flat line to zero makes a healthy steady value look like a
  // dropout, which is exactly the wrong signal to send an engineer.
  SignalHistory h;
  for (int i = 0; i < 20; ++i) {
    h.push(42.0);
  }
  const std::vector<int> levels = h.levels(8);
  ASSERT_FALSE(levels.empty());
  for (const int level : levels) {
    EXPECT_EQ(level, 4);
  }
}

TEST(SignalHistory, LevelsOfAnEmptyHistoryIsEmpty) {
  SignalHistory h;
  EXPECT_TRUE(h.levels(8).empty());
  h.push(1.0);
  EXPECT_TRUE(h.levels(0).empty());
}

TEST(ViewModel, DecodesFramesIntoNamedSignals) {
  const core::Database db = load_db();
  ViewModel model(&db, {500000, 0});
  model.ingest(engine_frame(db, kMs, 8000));

  const Snapshot snap = model.snapshot();
  ASSERT_EQ(snap.trace.size(), 1u);
  const TraceEntry& e = snap.trace.front();
  EXPECT_EQ(e.message_name, "EngineData");
  EXPECT_EQ(e.node, "ECM");
  EXPECT_TRUE(e.decoded);
  ASSERT_FALSE(e.signals.empty());
  EXPECT_EQ(e.signals[0].first, "EngineSpeed");
  EXPECT_DOUBLE_EQ(e.signals[0].second, 1000.0);  // 8000 * 0.125
  EXPECT_EQ(e.units[0], "rpm");
}

TEST(ViewModel, UnknownIdentifiersStillAppear) {
  const core::Database db = load_db();
  ViewModel model(&db, {500000, 0});
  const auto id = core::CanId::standard(0x555).value();
  model.ingest(core::FdFrame::make(id, {1, 2, 3}).value());

  const Snapshot snap = model.snapshot();
  ASSERT_EQ(snap.trace.size(), 1u);
  EXPECT_TRUE(snap.trace.front().message_name.empty());
  EXPECT_FALSE(snap.trace.front().decoded);
  ASSERT_EQ(snap.messages.size(), 1u);
  EXPECT_EQ(snap.messages.front().name, "id 555")
      << "an undecodable id still needs a row";
}

TEST(ViewModel, TraceIsBounded) {
  const core::Database db = load_db();
  ViewModel model(&db, {500000, 0});
  for (std::size_t i = 0; i < ViewModel::kTraceCapacity + 500; ++i) {
    model.ingest(engine_frame(db, i * kMs));
  }
  const Snapshot snap = model.snapshot();
  EXPECT_EQ(snap.trace.size(), ViewModel::kTraceCapacity);
  EXPECT_EQ(snap.total_frames, ViewModel::kTraceCapacity + 500);
}

// Period and jitter, the numbers the grouped view exists for

TEST(ViewModel, MeasuresPeriodAndJitter) {
  const core::Database db = load_db();
  ViewModel model(&db, {500000, 0});
  // Exactly 10 ms apart: mean 10, no jitter.
  for (int i = 0; i < 50; ++i) {
    model.ingest(engine_frame(db, static_cast<std::uint64_t>(i) * 10 * kMs));
  }
  Snapshot snap = model.snapshot();
  ASSERT_EQ(snap.messages.size(), 1u);
  EXPECT_NEAR(snap.messages[0].mean_period_ms, 10.0, 1e-9);
  EXPECT_NEAR(snap.messages[0].jitter_ms, 0.0, 1e-9);
  EXPECT_EQ(snap.messages[0].count, 50u);
  EXPECT_EQ(snap.messages[0].expected_cycle_ms, 10u)
      << "read from the GenMsgCycleTime attribute";
  EXPECT_FALSE(snap.messages[0].period_suspect());
}

TEST(ViewModel, JitterIsPeakDeviationNotStandardDeviation) {
  const core::Database db = load_db();
  ViewModel model(&db, {500000, 0});
  // Forty-nine frames on time, one arriving 25 ms late. A standard deviation
  // would bury that; peak deviation is what a scheduling argument needs.
  std::uint64_t t = 0;
  for (int i = 0; i < 50; ++i) {
    model.ingest(engine_frame(db, t));
    t += (i == 25 ? 35 : 10) * kMs;
  }
  const Snapshot snap = model.snapshot();
  ASSERT_EQ(snap.messages.size(), 1u);
  EXPECT_GT(snap.messages[0].max_period_ms, 30.0);
  EXPECT_GT(snap.messages[0].jitter_ms, 20.0)
      << "one late frame must be visible in the jitter figure";
}

TEST(ViewModel, FlagsAMessageRunningOffItsDeclaredCycle) {
  const core::Database db = load_db();
  ViewModel model(&db, {500000, 0});
  // The database declares 10 ms; send at 40 ms.
  for (int i = 0; i < 20; ++i) {
    model.ingest(engine_frame(db, static_cast<std::uint64_t>(i) * 40 * kMs));
  }
  const Snapshot snap = model.snapshot();
  ASSERT_EQ(snap.messages.size(), 1u);
  EXPECT_NEAR(snap.messages[0].mean_period_ms, 40.0, 1e-9);
  EXPECT_TRUE(snap.messages[0].period_suspect());
}

TEST(ViewModel, MessagesAreSortedByIdentifier) {
  const core::Database db = load_db();
  ViewModel model(&db, {500000, 0});
  model.ingest(core::FdFrame::make(db.find_message("TransmissionData")->id(),
                                   {0, 0, 0, 0, 0, 0, 0, 0})
                   .value());
  model.ingest(engine_frame(db, kMs));
  const Snapshot snap = model.snapshot();
  ASSERT_EQ(snap.messages.size(), 2u);
  EXPECT_LT(snap.messages[0].id.value(), snap.messages[1].id.value());
}

TEST(ViewModel, FilterMatchesNameIdNodeAndSignal) {
  const core::Database db = load_db();
  const auto ingest_two = [&db](ViewModel& model) {
    model.ingest(engine_frame(db, kMs));
    model.ingest(core::FdFrame::make(db.find_message("TransmissionData")->id(),
                                     {0, 0, 0, 0, 0, 0, 0, 0}, core::FrameFlags::None,
                                     2 * kMs)
                     .value());
  };
  for (const char* needle : {"EngineData", "100", "ECM", "ThrottlePosition"}) {
    ViewModel model(&db, {500000, 0});
    model.set_filter(needle);
    ingest_two(model);
    const Snapshot snap = model.snapshot();
    ASSERT_EQ(snap.trace.size(), 1u) << "filter '" << needle << "'";
    EXPECT_EQ(snap.trace.front().message_name, "EngineData") << needle;
  }
}

TEST(ViewModel, FilterIsCaseInsensitive) {
  const core::Database db = load_db();
  ViewModel model(&db, {500000, 0});
  model.set_filter("enginedata");
  model.ingest(engine_frame(db, kMs));
  EXPECT_EQ(model.snapshot().trace.size(), 1u);
}

TEST(ViewModel, FilteringDoesNotFalsifyTheStatistics) {
  // The point: an engineer who filters the trace still needs bus load to be
  // true, so filtered frames must keep counting everywhere except the trace.
  const core::Database db = load_db();
  ViewModel model(&db, {500000, 0});
  model.set_filter("nothing-matches-this");
  for (int i = 0; i < 100; ++i) {
    model.ingest(engine_frame(db, static_cast<std::uint64_t>(i) * 10 * kMs));
  }
  const Snapshot snap = model.snapshot();
  EXPECT_TRUE(snap.trace.empty());
  EXPECT_EQ(snap.dropped_by_filter, 100u);
  EXPECT_EQ(snap.total_frames, 100u);
  EXPECT_EQ(snap.bus.frames_received, 100u);
  EXPECT_EQ(snap.messages.size(), 1u);
  EXPECT_EQ(snap.messages[0].count, 100u);
  EXPECT_GT(snap.bus_load, 0.0);
}

TEST(ViewModel, PauseFreezesTheTraceButNotTheStatistics) {
  const core::Database db = load_db();
  ViewModel model(&db, {500000, 0});
  model.ingest(engine_frame(db, 0));
  model.set_paused(true);
  for (int i = 1; i < 20; ++i) {
    model.ingest(engine_frame(db, static_cast<std::uint64_t>(i) * 10 * kMs));
  }
  Snapshot snap = model.snapshot();
  EXPECT_EQ(snap.trace.size(), 1u) << "the trace is frozen";
  EXPECT_EQ(snap.messages[0].count, 20u) << "but the counters keep running";
  EXPECT_TRUE(snap.paused);

  model.set_paused(false);
  model.ingest(engine_frame(db, 500 * kMs));
  EXPECT_EQ(model.snapshot().trace.size(), 2u);
}

TEST(ViewModel, ClearResetsEverything) {
  const core::Database db = load_db();
  ViewModel model(&db, {500000, 0});
  for (int i = 0; i < 10; ++i) {
    model.ingest(engine_frame(db, static_cast<std::uint64_t>(i) * 10 * kMs));
  }
  model.clear();
  const Snapshot snap = model.snapshot();
  EXPECT_TRUE(snap.trace.empty());
  EXPECT_TRUE(snap.messages.empty());
  EXPECT_TRUE(snap.nodes.empty());
  EXPECT_EQ(snap.total_frames, 0u);
  EXPECT_EQ(snap.bus.frames_received, 0u);
}

TEST(ViewModel, PerNodeBreakdown) {
  const core::Database db = load_db();
  ViewModel model(&db, {500000, 0});
  for (int i = 0; i < 10; ++i) {
    model.ingest(engine_frame(db, static_cast<std::uint64_t>(i) * 10 * kMs));
  }
  model.ingest(core::FdFrame::make(db.find_message("TransmissionData")->id(),
                                   {0, 0, 0, 0, 0, 0, 0, 0})
                   .value());
  const Snapshot snap = model.snapshot();
  ASSERT_EQ(snap.nodes.size(), 2u);
  const auto ecm = std::find_if(snap.nodes.begin(), snap.nodes.end(),
                                [](const NodeStats& n) { return n.name == "ECM"; });
  ASSERT_NE(ecm, snap.nodes.end());
  EXPECT_EQ(ecm->frames, 10u);
  EXPECT_EQ(ecm->bytes, 80u);
  EXPECT_GT(ecm->wire_bits, 10u * 100u);
}

TEST(ViewModel, FramesPerSecondUsesASlidingWindow) {
  const core::Database db = load_db();
  ViewModel model(&db, {500000, 0});
  // 100 frames spread across exactly one second.
  for (int i = 0; i < 100; ++i) {
    model.ingest(engine_frame(db, static_cast<std::uint64_t>(i) * 10 * kMs));
  }
  EXPECT_NEAR(model.snapshot().frames_per_second, 100.0, 1.0);

  // Jump forward two seconds: the window should have emptied.
  model.ingest(engine_frame(db, 3000 * kMs));
  EXPECT_NEAR(model.snapshot().frames_per_second, 1.0, 0.001);
}

TEST(ViewModel, BusLoadMatchesTheTimingModel) {
  const core::Database db = load_db();
  ViewModel model(&db, {500000, 0});
  // 8-byte standard frames are 135 worst-case bits. 100 of them in one second
  // on a 500 kbit/s bus is 13500 / 500000 = 2.7%.
  for (int i = 0; i < 100; ++i) {
    model.ingest(engine_frame(db, static_cast<std::uint64_t>(i) * 10 * kMs));
  }
  EXPECT_NEAR(model.snapshot().bus_load, 13500.0 / 500000.0, 0.001);
}

TEST(ViewModel, SelectedSignalAccumulatesHistory) {
  const core::Database db = load_db();
  ViewModel model(&db, {500000, 0});
  model.select_signal("EngineData.EngineSpeed");
  for (int i = 0; i < 30; ++i) {
    model.ingest(engine_frame(db, static_cast<std::uint64_t>(i) * 10 * kMs,
                              static_cast<std::uint16_t>(i * 100)));
  }
  const SignalHistory h = model.history();
  ASSERT_EQ(h.samples().size(), 30u);
  EXPECT_DOUBLE_EQ(h.samples().front(), 0.0);
  EXPECT_DOUBLE_EQ(h.samples().back(), 29.0 * 100.0 * 0.125);
  EXPECT_DOUBLE_EQ(h.maximum(), 29.0 * 100.0 * 0.125);
}

TEST(ViewModel, ChangingTheSelectionDiscardsTheOldHistory) {
  const core::Database db = load_db();
  ViewModel model(&db, {500000, 0});
  model.select_signal("EngineData.EngineSpeed");
  model.ingest(engine_frame(db, kMs));
  ASSERT_FALSE(model.history().empty());
  model.select_signal("EngineData.ThrottlePosition");
  EXPECT_TRUE(model.history().empty())
      << "mixing two signals in one plot would be worse than useless";
}

TEST(ViewModel, SignalNamesAreQualifiedAndSorted) {
  const core::Database db = load_db();
  ViewModel model(&db, {500000, 0});
  const std::vector<std::string> names = model.signal_names();
  EXPECT_FALSE(names.empty());
  EXPECT_TRUE(std::is_sorted(names.begin(), names.end()));
  EXPECT_NE(std::find(names.begin(), names.end(), "EngineData.EngineSpeed"),
            names.end());
}

TEST(ViewModel, IngestAndSnapshotAreSafeFromTwoThreads) {
  // The real arrangement: the bus thread ingests while the render thread
  // snapshots at frame rate. Run under TSan in CI.
  const core::Database db = load_db();
  ViewModel model(&db, {500000, 0});
  std::atomic<bool> stop{false};

  std::thread producer([&] {
    for (int i = 0; i < 20000; ++i) {
      model.ingest(engine_frame(db, static_cast<std::uint64_t>(i) * kMs,
                                static_cast<std::uint16_t>(i & 0xFFFF)));
    }
    stop = true;
  });
  std::size_t snapshots = 0;
  while (!stop) {
    const Snapshot snap = model.snapshot();
    static_cast<void>(snap.messages.size());
    ++snapshots;
  }
  producer.join();
  EXPECT_GT(snapshots, 0u);
  EXPECT_EQ(model.snapshot().total_frames, 20000u);
}

}  // namespace
}  // namespace canforge::tui
