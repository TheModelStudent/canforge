// SPDX-License-Identifier: MIT
#include "canforge/sim/Ecu.hpp"

#include <gtest/gtest.h>

#include "canforge/dbc/Parser.hpp"

namespace canforge::sim {
namespace {

constexpr std::uint64_t kMs = 1000000ULL;

core::Database load_db() {
  auto parsed =
      dbc::parse_file(std::string(CANFORGE_DBC_TEST_DATA) + "/powertrain.dbc");
  EXPECT_TRUE(parsed.has_value());
  return std::move(parsed).value().database;
}

TEST(Checksum, Crc8SaeMatchesTheKnownVector) {
  // SAE J1850: poly 0x1D, init 0xFF, final XOR 0xFF. The check value for the
  // ASCII string "123456789" is 0x4B.
  const std::string check = "123456789";
  EXPECT_EQ(
      crc8_sae_j1850(reinterpret_cast<const std::uint8_t*>(check.data()), check.size()),
      0x4B);
  EXPECT_EQ(crc8_sae_j1850(nullptr, 0), 0x00);
}

TEST(Checksum, Xor8SkipsItsOwnByte) {
  const std::array<std::uint8_t, 4> data = {0x01, 0xFF, 0x02, 0x04};
  EXPECT_EQ(xor8(data.data(), data.size(), 1), 0x01 ^ 0x02 ^ 0x04);
  EXPECT_EQ(xor8(data.data(), data.size(), 99), 0x01 ^ 0xFF ^ 0x02 ^ 0x04);
}

TEST(Ecu, TransmitsOnItsCycle) {
  const core::Database db = load_db();
  Ecu ecu("ECM");
  TxMessage tx;
  tx.message = db.find_message("EngineData");
  ASSERT_NE(tx.message, nullptr);
  tx.cycle_ns = 10 * kMs;
  SignalBinding b;
  b.signal = "EngineSpeed";
  b.source = constant(1500.0);
  tx.bindings.push_back(std::move(b));
  ecu.add(std::move(tx));

  // Nothing before the first due time; then one frame per cycle.
  int count = 0;
  for (std::uint64_t t = 0; t <= 100 * kMs; t += kMs) {
    count += static_cast<int>(ecu.step(t, nullptr).size());
  }
  EXPECT_EQ(count, 11) << "t=0 plus one per 10 ms up to 100 ms";
}

TEST(Ecu, EncodesBoundSignals) {
  const core::Database db = load_db();
  const core::Message* m = db.find_message("EngineData");
  Ecu ecu("ECM");
  TxMessage tx;
  tx.message = m;
  tx.cycle_ns = 10 * kMs;
  for (const auto& [name, value] :
       std::vector<std::pair<std::string, double>>{{"EngineSpeed", 1500.0},
                                                   {"EngineCoolantTemp", 90.0},
                                                   {"ThrottlePosition", 40.0}}) {
    SignalBinding b;
    b.signal = name;
    b.source = constant(value);
    tx.bindings.push_back(std::move(b));
  }
  ecu.add(std::move(tx));

  const auto frames = ecu.step(0, nullptr);
  ASSERT_EQ(frames.size(), 1u);
  EXPECT_DOUBLE_EQ(m->find_signal("EngineSpeed")->decode(frames[0]), 1500.0);
  EXPECT_DOUBLE_EQ(m->find_signal("EngineCoolantTemp")->decode(frames[0]), 90.0);
  EXPECT_DOUBLE_EQ(m->find_signal("ThrottlePosition")->decode(frames[0]), 40.0);
}

TEST(Ecu, ReadsFromThePlant) {
  const core::Database db = load_db();
  const core::Message* m = db.find_message("EngineData");
  Vehicle vehicle{VehicleParams{}};
  vehicle.set_gear(1);
  vehicle.set_throttle(70.0);
  for (int i = 0; i < 500; ++i) {
    vehicle.step(10 * kMs);
  }

  Ecu ecu("ECM");
  TxMessage tx;
  tx.message = m;
  tx.cycle_ns = 10 * kMs;
  SignalBinding b;
  b.signal = "EngineSpeed";
  b.plant_field = "engine_rpm";
  tx.bindings.push_back(std::move(b));
  ecu.add(std::move(tx));

  const auto frames = ecu.step(0, &vehicle);
  ASSERT_EQ(frames.size(), 1u);
  EXPECT_NEAR(m->find_signal("EngineSpeed")->decode(frames[0]),
              vehicle.state().engine_rpm, 0.125)
      << "within one quantisation step of the plant value";
}

TEST(Ecu, RollingCounterWrapsAtTheSignalWidth) {
  const core::Database db = load_db();
  const core::Message* m = db.find_message("TransmissionData");
  ASSERT_NE(m, nullptr);
  Ecu ecu("TCM");
  TxMessage tx;
  tx.message = m;
  tx.cycle_ns = 10 * kMs;
  tx.counter_signal = "GearTarget";  // a 4-bit signal, so it wraps at 16
  ecu.add(std::move(tx));

  std::vector<double> seen;
  for (std::uint64_t t = 0; t < 200 * kMs; t += 10 * kMs) {
    for (const core::Frame& f : ecu.step(t, nullptr)) {
      seen.push_back(m->find_signal("GearTarget")->decode(f));
    }
  }
  ASSERT_GE(seen.size(), 18u);
  for (std::size_t i = 0; i < 16; ++i) {
    EXPECT_DOUBLE_EQ(seen[i], static_cast<double>(i)) << "index " << i;
  }
  EXPECT_DOUBLE_EQ(seen[16], 0.0) << "the counter must wrap, not saturate";
  EXPECT_DOUBLE_EQ(seen[17], 1.0);
}

TEST(Ecu, ChecksumCoversTheFrame) {
  const core::Database db = load_db();
  const core::Message* m = db.find_message("EngineData");
  Ecu ecu("ECM");
  TxMessage tx;
  tx.message = m;
  tx.cycle_ns = 10 * kMs;
  tx.checksum_signal = "EngineCoolantTemp";  // borrow an 8-bit signal
  tx.checksum = ChecksumKind::Crc8Sae;
  SignalBinding b;
  b.signal = "EngineSpeed";
  b.source = constant(2000.0);
  tx.bindings.push_back(std::move(b));
  ecu.add(std::move(tx));

  const auto frames = ecu.step(0, nullptr);
  ASSERT_EQ(frames.size(), 1u);
  // Recompute over the frame with the checksum byte zeroed, as a receiver
  // would.
  core::Frame probe = frames[0];
  const core::Signal* c = m->find_signal("EngineCoolantTemp");
  core::SignalLayout raw = c->layout();
  raw.minimum = 0.0;
  raw.maximum = 0.0;
  raw.encode_raw(0, probe.data());
  EXPECT_EQ(static_cast<std::uint8_t>(c->layout().decode_raw(frames[0].data())),
            crc8_sae_j1850(probe.data(), probe.size()));
}

TEST(Ecu, OnChangeSuppressesIdenticalFrames) {
  const core::Database db = load_db();
  Ecu ecu("ECM");
  TxMessage tx;
  tx.message = db.find_message("EngineData");
  tx.cycle_ns = 10 * kMs;
  tx.mode = TxMode::OnChange;
  SignalBinding b;
  b.signal = "EngineSpeed";
  b.source = constant(1000.0);
  tx.bindings.push_back(std::move(b));
  ecu.add(std::move(tx));

  int count = 0;
  for (std::uint64_t t = 0; t <= 200 * kMs; t += kMs) {
    count += static_cast<int>(ecu.step(t, nullptr).size());
  }
  EXPECT_EQ(count, 1) << "a constant signal should transmit exactly once";
}

TEST(Ecu, OnChangeTransmitsWhenTheValueMoves) {
  const core::Database db = load_db();
  Ecu ecu("ECM");
  TxMessage tx;
  tx.message = db.find_message("EngineData");
  tx.cycle_ns = 10 * kMs;
  tx.mode = TxMode::OnChange;
  SignalBinding b;
  b.signal = "EngineSpeed";
  b.source = ramp(0.0, 1000.0, 100 * kMs);
  tx.bindings.push_back(std::move(b));
  ecu.add(std::move(tx));

  int count = 0;
  for (std::uint64_t t = 0; t <= 200 * kMs; t += kMs) {
    count += static_cast<int>(ecu.step(t, nullptr).size());
  }
  EXPECT_GT(count, 5);
  EXPECT_LE(count, 21);
}

TEST(Ecu, EventModeOnlyFiresWhenTriggered) {
  const core::Database db = load_db();
  Ecu ecu("ECM");
  TxMessage tx;
  tx.message = db.find_message("EngineData");
  tx.mode = TxMode::Event;
  ecu.add(std::move(tx));

  EXPECT_TRUE(ecu.step(0, nullptr).empty());
  EXPECT_TRUE(ecu.step(100 * kMs, nullptr).empty());
  EXPECT_TRUE(ecu.trigger("EngineData"));
  EXPECT_EQ(ecu.step(101 * kMs, nullptr).size(), 1u);
  EXPECT_TRUE(ecu.step(102 * kMs, nullptr).empty()) << "one trigger, one frame";
  EXPECT_FALSE(ecu.trigger("NoSuchMessage"));
}

TEST(Ecu, StoppedNodeIsSilent) {
  const core::Database db = load_db();
  Ecu ecu("ECM");
  TxMessage tx;
  tx.message = db.find_message("EngineData");
  tx.cycle_ns = 10 * kMs;
  ecu.add(std::move(tx));

  EXPECT_EQ(ecu.step(0, nullptr).size(), 1u);
  ecu.set_running(false);
  for (std::uint64_t t = 10 * kMs; t < 100 * kMs; t += 10 * kMs) {
    EXPECT_TRUE(ecu.step(t, nullptr).empty());
  }
  ecu.set_running(true);
  EXPECT_EQ(ecu.step(100 * kMs, nullptr).size(), 1u);
}

TEST(Ecu, CoarseSteppingDoesNotProduceABurst) {
  // Stepping once after a long gap must not emit one frame per missed cycle:
  // a real node with a single mailbox sends the newest value and moves on.
  const core::Database db = load_db();
  Ecu ecu("ECM");
  TxMessage tx;
  tx.message = db.find_message("EngineData");
  tx.cycle_ns = 10 * kMs;
  ecu.add(std::move(tx));

  EXPECT_EQ(ecu.step(0, nullptr).size(), 1u);
  EXPECT_EQ(ecu.step(1000 * kMs, nullptr).size(), 1u);
}

TEST(Ecu, JitterStaysWithinItsBound) {
  const core::Database db = load_db();
  Ecu ecu("ECM");
  TxMessage tx;
  tx.message = db.find_message("EngineData");
  tx.cycle_ns = 10 * kMs;
  tx.jitter_ns = 2 * kMs;
  ecu.add(std::move(tx));

  std::vector<std::uint64_t> times;
  for (std::uint64_t t = 0; t <= 1000 * kMs; t += 100000ULL) {
    if (!ecu.step(t, nullptr).empty()) {
      times.push_back(t);
    }
  }
  ASSERT_GT(times.size(), 50u);
  for (std::size_t i = 1; i < times.size(); ++i) {
    const std::uint64_t gap = times[i] - times[i - 1];
    EXPECT_GE(gap, 8 * kMs - 100000ULL) << "gap " << i;
    EXPECT_LE(gap, 12 * kMs + 100000ULL) << "gap " << i;
  }
}

}  // namespace
}  // namespace canforge::sim
