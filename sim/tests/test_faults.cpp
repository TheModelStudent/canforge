// SPDX-License-Identifier: MIT
#include "canforge/sim/Faults.hpp"
#include "canforge/sim/Simulator.hpp"

#include <gtest/gtest.h>

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

std::unique_ptr<Simulator> build_sim(const core::Database& db) {
  auto parsed = parse_config_file(std::string(CANFORGE_SIM_TEST_DATA) + "/vehicle.cfg");
  EXPECT_TRUE(parsed.has_value());
  auto sim = Simulator::build(parsed.value().config, db);
  EXPECT_TRUE(sim.has_value());
  return std::move(sim).value();
}

std::size_t count_frames(Simulator& sim, core::CanId id, std::uint64_t until_ns) {
  std::size_t n = 0;
  for (std::uint64_t t = 0; t <= until_ns; t += kMs) {
    for (const core::Frame& f : sim.step(t)) {
      if (f.id() == id) {
        ++n;
      }
    }
  }
  return n;
}

TEST(Faults, InactiveByDefault) {
  const core::Database db = load_db();
  auto sim = build_sim(db);
  EXPECT_EQ(sim->faults().faults().size(), 6u);
  for (const Fault& f : sim->faults().faults()) {
    EXPECT_FALSE(f.active) << f.name;
  }
  EXPECT_EQ(sim->faults().dropped(), 0u);
}

TEST(Faults, StopNodeSilencesIt) {
  const core::Database db = load_db();
  auto sim = build_sim(db);
  const core::CanId engine = db.find_message("EngineData")->id();

  EXPECT_GT(count_frames(*sim, engine, 1 * kS), 50u);

  sim->reset();
  ASSERT_TRUE(sim->faults().set_active("ecm_offline", true));
  EXPECT_EQ(count_frames(*sim, engine, 1 * kS), 0u);

  // And it comes back when the fault is cleared.
  ASSERT_TRUE(sim->faults().set_active("ecm_offline", false));
  EXPECT_GT(count_frames(*sim, engine, 2 * kS), 50u);
}

TEST(Faults, DropIsSeededAndProportional) {
  const core::Database db = load_db();
  auto sim = build_sim(db);
  const core::CanId wheels = db.find_message("WheelSpeeds")->id();

  const std::size_t baseline = count_frames(*sim, wheels, 10 * kS);
  ASSERT_GT(baseline, 400u);

  sim->reset();
  ASSERT_TRUE(sim->faults().set_active("dropsome", true));
  const std::size_t with_drops = count_frames(*sim, wheels, 10 * kS);
  EXPECT_LT(with_drops, baseline);
  EXPECT_NEAR(static_cast<double>(with_drops) / static_cast<double>(baseline),
              0.95, 0.04)
      << "the config drops 5%";

  // Same seed, same result.
  sim->reset();
  sim->faults().set_active("dropsome", true);
  EXPECT_EQ(count_frames(*sim, wheels, 10 * kS), with_drops);
}

TEST(Faults, FreezeHoldsTheFirstValue) {
  const core::Database db = load_db();
  auto sim = build_sim(db);
  const core::Message* engine = db.find_message("EngineData");
  const core::Signal* rpm = engine->find_signal("EngineSpeed");

  ASSERT_TRUE(sim->faults().set_active("stuck_rpm", true));
  double first = -1.0;
  double last = -1.0;
  for (std::uint64_t t = 0; t <= 8 * kS; t += kMs) {
    for (const core::Frame& f : sim->step(t)) {
      if (f.id() == engine->id()) {
        last = rpm->decode(f);
        if (first < 0.0) {
          first = last;
        }
      }
    }
  }
  ASSERT_GE(first, 0.0);
  EXPECT_DOUBLE_EQ(last, first) << "a frozen signal never moves";
  // The plant itself did move, which is what makes the fault visible.
  EXPECT_GT(sim->vehicle().state().engine_rpm, first + 100.0);
}

TEST(Faults, OutOfRangeBypassesTheDeclaredLimits) {
  const core::Database db = load_db();
  auto sim = build_sim(db);
  const core::Message* wheels = db.find_message("WheelSpeeds");
  const core::Signal* speed = wheels->find_signal("WheelBasedSpeed");
  // The signal's 16 bits reach 255.996 but the database declares a maximum of
  // 250.996, so 255 is representable on the wire yet outside the range a
  // conforming ECU would ever send. That gap is what the fault exploits; the
  // bit width still bounds it, exactly as it would on real hardware.
  ASSERT_LT(speed->layout().maximum, 255.0);

  ASSERT_TRUE(sim->faults().set_active("silly_speed", true));
  bool saw = false;
  for (std::uint64_t t = 0; t <= 200 * kMs && !saw; t += kMs) {
    for (const core::Frame& f : sim->step(t)) {
      if (f.id() == wheels->id()) {
        EXPECT_GT(speed->decode(f), speed->layout().maximum);
        EXPECT_NEAR(speed->decode(f), 255.0, 0.01);
        saw = true;
        break;
      }
    }
  }
  EXPECT_TRUE(saw);
}

TEST(Faults, DelayShiftsAMessageInTime) {
  const core::Database db = load_db();
  auto sim = build_sim(db);
  const core::CanId trans = db.find_message("TransmissionData")->id();

  ASSERT_TRUE(sim->faults().set_active("lagbox", true));
  std::uint64_t first_seen = 0;
  for (std::uint64_t t = 0; t <= 200 * kMs && first_seen == 0; t += kMs) {
    for (const core::Frame& f : sim->step(t)) {
      if (f.id() == trans) {
        first_seen = t;
        break;
      }
    }
  }
  // The TCM's first transmission is at its 5 ms phase offset; the fault holds
  // it back by 40 ms.
  EXPECT_GE(first_seen, 45 * kMs);
  EXPECT_LE(first_seen, 47 * kMs);
  EXPECT_GT(sim->faults().delayed(), 0u);
}

TEST(Faults, BitFlipCorruptsSomeFrames) {
  const core::Database db = load_db();
  auto sim = build_sim(db);
  ASSERT_TRUE(sim->faults().set_active("noise", true));
  for (std::uint64_t t = 0; t <= 20 * kS; t += kMs) {
    sim->step(t);
  }
  EXPECT_GT(sim->faults().corrupted(), 0u);
  // 1% of roughly 1000 ABS frames.
  EXPECT_LT(sim->faults().corrupted(), 60u);
}

TEST(Faults, ToggleAndLookup) {
  const core::Database db = load_db();
  auto sim = build_sim(db);
  EXPECT_NE(sim->faults().find("dropsome"), nullptr);
  EXPECT_EQ(sim->faults().find("no_such_fault"), nullptr);
  EXPECT_FALSE(sim->faults().set_active("no_such_fault", true));

  EXPECT_TRUE(sim->faults().toggle("dropsome"));
  EXPECT_TRUE(sim->faults().find("dropsome")->active);
  EXPECT_TRUE(sim->faults().toggle("dropsome"));
  EXPECT_FALSE(sim->faults().find("dropsome")->active);
}

TEST(Faults, ScopedToOneNode) {
  const core::Database db = load_db();
  auto sim = build_sim(db);
  const core::CanId engine = db.find_message("EngineData")->id();
  const core::CanId wheels = db.find_message("WheelSpeeds")->id();

  // dropsome is scoped to ABS, so the ECM's traffic is untouched.
  ASSERT_TRUE(sim->faults().set_active("dropsome", true));
  std::size_t engine_frames = 0;
  std::size_t wheel_frames = 0;
  for (std::uint64_t t = 0; t <= 10 * kS; t += kMs) {
    for (const core::Frame& f : sim->step(t)) {
      if (f.id() == engine) {
        ++engine_frames;
      } else if (f.id() == wheels) {
        ++wheel_frames;
      }
    }
  }
  EXPECT_NEAR(static_cast<double>(engine_frames), 1000.0, 20.0)
      << "the ECM should be unaffected";
  EXPECT_LT(wheel_frames, 500u);
}

}  // namespace
}  // namespace canforge::sim
