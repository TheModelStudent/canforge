// SPDX-License-Identifier: MIT
#include "canforge/sim/Config.hpp"
#include "canforge/sim/Simulator.hpp"

#include <gtest/gtest.h>

#include <fstream>
#include <sstream>

#include "canforge/dbc/Parser.hpp"

namespace canforge::sim {
namespace {

constexpr std::uint64_t kMs = 1000000ULL;
constexpr std::uint64_t kS = 1000000000ULL;

core::Database load_db() {
  auto parsed =
      dbc::parse_file(std::string(CANFORGE_DBC_TEST_DATA) + "/powertrain.dbc");
  EXPECT_TRUE(parsed.has_value());
  return std::move(parsed).value().database;
}

TEST(Config, ParsesTheExampleFile) {
  auto parsed = parse_config_file(std::string(CANFORGE_SIM_TEST_DATA) + "/vehicle.cfg");
  ASSERT_TRUE(parsed.has_value()) << parsed.error().message();
  const ConfigParseResult& r = parsed.value();
  ASSERT_TRUE(r.ok()) << r.diagnostics.format("vehicle.cfg");

  EXPECT_EQ(r.config.bus, "vcan0");
  EXPECT_EQ(r.config.nominal_bitrate, 500000u);
  EXPECT_EQ(r.config.database_path, "powertrain.dbc");
  EXPECT_TRUE(r.config.has_plant);
  EXPECT_DOUBLE_EQ(r.config.plant.mass_kg, 1500.0);
  EXPECT_EQ(r.config.plant.gear_ratios.size(), 5u);
  EXPECT_DOUBLE_EQ(r.config.plant.gear_ratios[0], 3.5);

  ASSERT_EQ(r.config.nodes.size(), 3u);
  EXPECT_EQ(r.config.nodes[0].name, "ECM");
  ASSERT_EQ(r.config.nodes[0].transmits.size(), 2u);
  EXPECT_EQ(r.config.nodes[0].transmits[0].message, "EngineData");
  EXPECT_EQ(r.config.nodes[0].transmits[0].cycle_ns, 10 * kMs);
  EXPECT_EQ(r.config.nodes[0].transmits[0].jitter_ns, 500000ULL);
  EXPECT_EQ(r.config.nodes[0].transmits[1].phase_ns, 3 * kMs);

  EXPECT_EQ(r.config.drive.size(), 6u);
  EXPECT_EQ(r.config.drive[1].at_ns, 2 * kS);
  EXPECT_DOUBLE_EQ(r.config.drive[1].throttle_pct, 60.0);

  EXPECT_EQ(r.config.faults.size(), 6u);
  EXPECT_EQ(r.config.faults[0].name, "dropsome");
  EXPECT_EQ(r.config.faults[0].kind, FaultKind::DropFrames);
  EXPECT_DOUBLE_EQ(r.config.faults[0].probability, 0.05);
  EXPECT_EQ(r.config.faults[0].node, "ABS");
  EXPECT_FALSE(r.config.faults[0].active) << "faults start disarmed";
}

TEST(Config, DurationUnits) {
  const std::string text =
      "node A\n"
      "  tx M cycle 250us\n"
      "  tx N cycle 1.5s\n"
      "  tx O cycle 20ms\n"
      "  tx P cycle 1000ns\n";
  const auto r = parse_config_string(text, "d.cfg");
  ASSERT_TRUE(r.ok()) << r.diagnostics.format("d.cfg");
  ASSERT_EQ(r.config.nodes.size(), 1u);
  const auto& tx = r.config.nodes[0].transmits;
  ASSERT_EQ(tx.size(), 4u);
  EXPECT_EQ(tx[0].cycle_ns, 250000ULL);
  EXPECT_EQ(tx[1].cycle_ns, 1500000000ULL);
  EXPECT_EQ(tx[2].cycle_ns, 20000000ULL);
  EXPECT_EQ(tx[3].cycle_ns, 1000ULL);
}

TEST(Config, SourceExpressions) {
  struct Case {
    const char* expression;
    std::uint64_t at_ns;
    double expected;
  };
  const Case cases[] = {
      {"const 5", 0, 5.0},
      {"7.5", 0, 7.5},
      {"ramp 0 to 100 over 10s", 5 * kS, 50.0},
      {"ramp 0 to 10 over 1s repeat", 1500 * kMs, 5.0},
      {"sine amp 10 freq 1 offset 50", 250 * kMs, 60.0},
      {"square low 1 high 9 period 1s duty 0.5", 100 * kMs, 9.0},
      {"square low 1 high 9 period 1s duty 0.5", 600 * kMs, 1.0},
      {"keyframes 0s=0 10s=100", 5 * kS, 50.0},
      {"const 3 * 4", 0, 12.0},
      {"const 3 + const 4", 0, 7.0},
      {"ramp 0 to 100 over 10s clamp 20 80", 0, 20.0},
  };
  for (const Case& c : cases) {
    auto source = build_source(c.expression);
    ASSERT_TRUE(source.has_value()) << c.expression << ": " << source.error().message();
    EXPECT_NEAR(source.value()->sample(c.at_ns), c.expected, 1e-6) << c.expression;
  }
}

TEST(Config, RejectsBadSourceExpressions) {
  EXPECT_FALSE(build_source("").has_value());
  EXPECT_FALSE(build_source("wibble 1 2").has_value());
  EXPECT_FALSE(build_source("const").has_value());
  EXPECT_FALSE(build_source("const 1 nonsense").has_value());
}

TEST(Config, DiagnosesAnUnknownSection) {
  const auto r = parse_config_string("wobble ECM\n", "bad.cfg");
  EXPECT_TRUE(r.diagnostics.has_errors());
  const std::string text = r.diagnostics.format("bad.cfg");
  EXPECT_NE(text.find("bad.cfg:1:1"), std::string::npos) << text;
  EXPECT_NE(text.find("unknown section"), std::string::npos) << text;
  EXPECT_NE(text.find('^'), std::string::npos) << text;
}

TEST(Config, DiagnosesAnUnknownPlantField) {
  const std::string text =
      "node A\n"
      "  tx M cycle 10ms\n"
      "    signal S = plant.not_a_field\n";
  const auto r = parse_config_string(text, "bad.cfg");
  EXPECT_TRUE(r.diagnostics.has_errors());
  const std::string rendered = r.diagnostics.format("bad.cfg");
  EXPECT_NE(rendered.find("unknown plant field"), std::string::npos) << rendered;
  EXPECT_NE(rendered.find("engine_rpm"), std::string::npos)
      << "the note should list the valid fields";
}

TEST(Config, DiagnosesABadSourceExpression) {
  const std::string text =
      "node A\n"
      "  tx M cycle 10ms\n"
      "    signal S = wibble 3\n";
  const auto r = parse_config_string(text, "bad.cfg");
  EXPECT_TRUE(r.diagnostics.has_errors());
  EXPECT_NE(r.diagnostics.format("bad.cfg").find("unknown signal source"),
            std::string::npos);
}

TEST(Config, DiagnosesAnUnknownFaultKind) {
  const auto r = parse_config_string("fault f explode\n", "bad.cfg");
  EXPECT_TRUE(r.diagnostics.has_errors());
  EXPECT_NE(r.diagnostics.format("bad.cfg").find("unknown fault kind"),
            std::string::npos);
}

TEST(Config, CommentsAndBlankLinesAreIgnored) {
  const std::string text =
      "// leading comment\n"
      "\n"
      "bus vcan1 250000\n"
      "\n";
  const auto r = parse_config_string(text, "c.cfg");
  ASSERT_TRUE(r.ok()) << r.diagnostics.format("c.cfg");
  EXPECT_EQ(r.config.bus, "vcan1");
  EXPECT_EQ(r.config.nominal_bitrate, 250000u);
}

TEST(Simulator, BuildsAndRunsTheExample) {
  const core::Database db = load_db();
  auto parsed = parse_config_file(std::string(CANFORGE_SIM_TEST_DATA) + "/vehicle.cfg");
  ASSERT_TRUE(parsed.has_value());
  ASSERT_TRUE(parsed.value().ok());

  auto sim = Simulator::build(parsed.value().config, db);
  ASSERT_TRUE(sim.has_value()) << sim.error().message();
  ASSERT_EQ(sim.value()->nodes().size(), 3u);

  std::size_t frames = 0;
  for (std::uint64_t t = 0; t <= 10 * kS; t += kMs) {
    frames += sim.value()->step(t).size();
  }
  // ECM: 100/s + 50/s, TCM: 50/s, ABS: 50/s -> 250/s over 10 s.
  EXPECT_NEAR(static_cast<double>(frames), 2500.0, 60.0);
  EXPECT_GT(sim.value()->vehicle().state().speed_mps, 1.0)
      << "the driver script should have got the car moving";
}

TEST(Simulator, TheBusTellsACoherentStory) {
  const core::Database db = load_db();
  auto parsed = parse_config_file(std::string(CANFORGE_SIM_TEST_DATA) + "/vehicle.cfg");
  auto sim = Simulator::build(parsed.value().config, db).value();

  const core::Message* engine = db.find_message("EngineData");
  const core::Message* wheels = db.find_message("WheelSpeeds");
  double last_rpm = 0.0;
  double last_speed = 0.0;
  for (std::uint64_t t = 0; t <= 12 * kS; t += kMs) {
    for (const core::Frame& f : sim->step(t)) {
      if (f.id() == engine->id()) {
        last_rpm = engine->find_signal("EngineSpeed")->decode(f);
      } else if (f.id() == wheels->id()) {
        last_speed = wheels->find_signal("WheelBasedSpeed")->decode(f);
      }
    }
  }
  // Two different ECUs, two different messages, one physical story: the car
  // is moving and the engine is turning above idle.
  EXPECT_GT(last_speed, 5.0);
  EXPECT_GT(last_rpm, 800.0);
  EXPECT_NEAR(last_rpm, sim->vehicle().state().engine_rpm, 1.0);
}

TEST(Simulator, RejectsAConfigThatNamesAMissingMessage) {
  const core::Database db = load_db();
  const auto r = parse_config_string("node X\n  tx NotAMessage cycle 10ms\n", "x.cfg");
  ASSERT_TRUE(r.ok());
  const auto sim = Simulator::build(r.config, db);
  ASSERT_FALSE(sim.has_value());
  EXPECT_EQ(sim.error().code(), core::ErrorCode::ParseUndefinedReference);
}

TEST(Simulator, RejectsAConfigThatNamesAMissingSignal) {
  const core::Database db = load_db();
  const auto r = parse_config_string(
      "node X\n  tx EngineData cycle 10ms\n    signal NoSuchSignal = const 1\n",
      "x.cfg");
  ASSERT_TRUE(r.ok());
  EXPECT_FALSE(Simulator::build(r.config, db).has_value());
}

TEST(Simulator, ResetIsReproducible) {
  const core::Database db = load_db();
  auto parsed = parse_config_file(std::string(CANFORGE_SIM_TEST_DATA) + "/vehicle.cfg");
  auto sim = Simulator::build(parsed.value().config, db).value();

  const auto run = [&sim]() {
    std::vector<std::uint8_t> digest;
    for (std::uint64_t t = 0; t <= 3 * kS; t += kMs) {
      for (const core::Frame& f : sim->step(t)) {
        digest.push_back(f.data()[0]);
      }
    }
    return digest;
  };
  const auto first = run();
  sim->reset();
  const auto second = run();
  EXPECT_EQ(first, second) << "a reset simulation must replay identically";
}

}  // namespace
}  // namespace canforge::sim
