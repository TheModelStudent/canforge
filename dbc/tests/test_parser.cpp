// SPDX-License-Identifier: MIT
#include "canforge/dbc/Parser.hpp"

#include <gtest/gtest.h>

#include <array>
#include <string>
#include <vector>

#include "canforge/core/Frame.hpp"

namespace canforge::dbc {
namespace {

using core::ByteOrder;
using core::CanId;
using core::Frame;
using core::Message;
using core::MultiplexRole;
using core::Signal;
using core::Signedness;
using core::ValueType;

std::string data_path(const char* name) {
  return std::string(CANFORGE_DBC_TEST_DATA) + "/" + name;
}

ParseResult load_ok(const char* name) {
  auto parsed = parse_file(data_path(name));
  EXPECT_TRUE(parsed.has_value()) << "cannot read " << name;
  ParseResult r = std::move(parsed).value();
  EXPECT_FALSE(r.diagnostics.has_errors()) << name << " should parse cleanly:\n"
                                           << r.diagnostics.format(name);
  return r;
}

TEST(Parser, PowertrainStructure) {
  const auto r = load_ok("powertrain.dbc");
  const auto& db = r.database;

  EXPECT_EQ(db.version(), "1.0 canforge powertrain example");
  EXPECT_EQ(db.comment(), "Small powertrain example database for canforge tests.");
  ASSERT_EQ(db.nodes().size(), 4u);
  EXPECT_EQ(db.nodes()[0].name, "ECM");
  EXPECT_EQ(db.nodes()[0].comment, "Engine control module");
  EXPECT_EQ(db.messages().size(), 4u);
  EXPECT_FALSE(db.new_symbols().empty()) << "the NS_ list must be preserved";
}

TEST(Parser, PowertrainSignalLayouts) {
  const auto r = load_ok("powertrain.dbc");
  const Message* m = r.database.find_message("EngineData");
  ASSERT_NE(m, nullptr);
  EXPECT_EQ(m->id(), CanId::standard(256).value());
  EXPECT_EQ(m->dlc(), 8u);
  EXPECT_EQ(m->transmitter(), "ECM");
  EXPECT_EQ(m->comment(), "Cyclic engine data, 10 ms");
  ASSERT_EQ(m->signals().size(), 6u);

  const Signal* speed = m->find_signal("EngineSpeed");
  ASSERT_NE(speed, nullptr);
  EXPECT_EQ(speed->layout().start_bit, 0u);
  EXPECT_EQ(speed->layout().bit_length, 16u);
  EXPECT_EQ(speed->layout().byte_order, ByteOrder::Intel);
  EXPECT_EQ(speed->layout().signedness, Signedness::Unsigned);
  EXPECT_DOUBLE_EQ(speed->layout().factor, 0.125);
  EXPECT_DOUBLE_EQ(speed->layout().offset, 0.0);
  EXPECT_DOUBLE_EQ(speed->layout().maximum, 8031.875);
  EXPECT_EQ(speed->unit(), "rpm");
  EXPECT_EQ(speed->comment(), "Crankshaft speed");
  ASSERT_EQ(speed->receivers().size(), 2u);
  EXPECT_EQ(speed->receivers()[0], "TCM");

  const Signal* torque = m->find_signal("EngineTorque");
  ASSERT_NE(torque, nullptr);
  EXPECT_EQ(torque->layout().signedness, Signedness::Signed);
  EXPECT_DOUBLE_EQ(torque->layout().minimum, -3276.8);
}

TEST(Parser, MotorolaSignalsAreParsedAsBigEndian) {
  const auto r = load_ok("powertrain.dbc");
  const Message* m = r.database.find_message("TransmissionData");
  ASSERT_NE(m, nullptr);
  const Signal* gear = m->find_signal("GearActual");
  ASSERT_NE(gear, nullptr);
  EXPECT_EQ(gear->layout().byte_order, ByteOrder::Motorola);
  EXPECT_EQ(gear->layout().start_bit, 7u);
  EXPECT_EQ(gear->layout().bit_length, 4u);

  // Start bit 7 with four bits is the high nibble of byte 0.
  const Frame f = Frame::make(m->id(), {0x30, 0, 0, 0, 0, 0, 0, 0}).value();
  EXPECT_DOUBLE_EQ(gear->decode(f), 3.0);
  EXPECT_DOUBLE_EQ(m->find_signal("GearTarget")->decode(f), 0.0);
}

TEST(Parser, ExtendedIdentifiersUseBitThirtyOne) {
  const auto r = load_ok("powertrain.dbc");
  const Message* m = r.database.find_message("WheelSpeeds");
  ASSERT_NE(m, nullptr);
  EXPECT_TRUE(m->id().is_extended());
  EXPECT_EQ(m->id().value(), 0x18FEF100u)
      << "the 0x80000000 marker must be stripped, not kept in the identifier";

  const Message* eec = r.database.find_message("EngineControl1");
  ASSERT_NE(eec, nullptr);
  EXPECT_TRUE(eec->id().is_extended());
  EXPECT_EQ(eec->id().value(), 0x0CF00400u);
}

TEST(Parser, DecodesAJ1939EngineSpeed) {
  const auto r = load_ok("powertrain.dbc");
  const Message* m = r.database.find_message("EngineControl1");
  ASSERT_NE(m, nullptr);
  // SPN 190 lives in bytes 4-5 (start bit 24), 0.125 rpm/bit.
  // 0x2EE0 = 12000 -> 1500 rpm.
  const Frame f = Frame::make(m->id(), {0, 0, 0, 0xE0, 0x2E, 0, 0, 0}).value();
  const auto decoded = m->decode(f);
  ASSERT_TRUE(decoded.has_value());
  bool found = false;
  for (const auto& d : *decoded) {
    if (d.name() == "EngineSpeedJ1939") {
      EXPECT_DOUBLE_EQ(d.value, 1500.0);
      found = true;
    }
  }
  EXPECT_TRUE(found);
}

TEST(Parser, Attributes) {
  const auto r = load_ok("powertrain.dbc");
  const auto& db = r.database;
  ASSERT_NE(db.find_attribute_definition("GenMsgCycleTime"), nullptr);
  EXPECT_EQ(db.find_attribute_definition("GenMsgCycleTime")->object,
            core::AttributeObject::Message);
  EXPECT_TRUE(db.find_attribute_definition("GenMsgCycleTime")->has_default);
  EXPECT_EQ(db.find_attribute_definition("GenMsgCycleTime")->default_value.as_int(),
            100);

  const Message* m = db.find_message("EngineData");
  ASSERT_NE(m, nullptr);
  const auto it = m->attributes().find("GenMsgCycleTime");
  ASSERT_NE(it, m->attributes().end());
  EXPECT_EQ(it->second.as_int(), 10);

  EXPECT_EQ(db.attributes().at("BusType").text(), "CAN");
  const Signal* s = m->find_signal("EngineSpeed");
  ASSERT_NE(s, nullptr);
  EXPECT_DOUBLE_EQ(s->attributes().at("GenSigStartValue").number(), 800.0);
}

TEST(Parser, AdditionalTransmitters) {
  const auto r = load_ok("powertrain.dbc");
  const Message* m = r.database.find_message("TransmissionData");
  ASSERT_NE(m, nullptr);
  ASSERT_EQ(m->additional_transmitters().size(), 2u);
  EXPECT_EQ(m->additional_transmitters()[0], "TCM");
}

TEST(Parser, ValidatesAndLintsCleanly) {
  const auto r = load_ok("powertrain.dbc");
  EXPECT_TRUE(r.database.validate().has_value());
  const auto problems = r.database.lint();
  EXPECT_TRUE(problems.empty())
      << "first lint finding: " << (problems.empty() ? "" : problems.front());
}

TEST(Parser, SimpleMultiplexing) {
  const auto r = load_ok("multiplexed.dbc");
  const Message* m = r.database.find_message("DiagnosticResponse");
  ASSERT_NE(m, nullptr);
  EXPECT_TRUE(m->is_multiplexed());
  ASSERT_NE(m->multiplexor(), nullptr);
  EXPECT_EQ(m->multiplexor()->name(), "ResponseKind");
  EXPECT_EQ(m->find_signal("VoltageA")->multiplex_role(), MultiplexRole::Multiplexed);
  EXPECT_EQ(m->find_signal("VoltageA")->multiplex_value(), 1u);
}

TEST(Parser, MultiplexedDecodeReturnsOnlyTheActiveSignals) {
  const auto r = load_ok("multiplexed.dbc");
  const Message* m = r.database.find_message("DiagnosticResponse");
  ASSERT_NE(m, nullptr);

  // Multiplexor = 1: the two voltages, plus the unmultiplexed signal.
  {
    const Frame f =
        Frame::make(m->id(), {0x01, 0xE8, 0x03, 0xD0, 0x07, 0, 0, 0x42}).value();
    const auto decoded = m->decode(f).value();
    std::vector<std::string> names;
    for (const auto& d : decoded) {
      names.emplace_back(d.name());
    }
    EXPECT_EQ(names, (std::vector<std::string>{"ResponseKind", "VoltageA", "VoltageB",
                                               "AlwaysPresent"}));
    EXPECT_DOUBLE_EQ(decoded[1].value, 1.0);  // 0x03E8 = 1000 * 0.001
    EXPECT_DOUBLE_EQ(decoded[2].value, 2.0);  // 0x07D0 = 2000 * 0.001
    EXPECT_DOUBLE_EQ(decoded[3].value, 66.0);
  }
  // Multiplexor = 2: the three temperatures instead.
  {
    const Frame f = Frame::make(m->id(), {0x02, 0x28, 0x50, 0x78, 0, 0, 0, 0}).value();
    const auto decoded = m->decode(f).value();
    std::vector<std::string> names;
    for (const auto& d : decoded) {
      names.emplace_back(d.name());
    }
    EXPECT_EQ(names,
              (std::vector<std::string>{"ResponseKind", "TemperatureA", "TemperatureB",
                                        "TemperatureC", "AlwaysPresent"}));
    EXPECT_DOUBLE_EQ(decoded[1].value, 0.0);   // 0x28 = 40, offset -40
    EXPECT_DOUBLE_EQ(decoded[2].value, 40.0);  // 0x50 = 80
  }
  // Multiplexor = 7: nothing is selected, only the plain signal remains.
  {
    const Frame f = Frame::make(m->id(), {0x07, 1, 2, 3, 4, 5, 6, 7}).value();
    const auto decoded = m->decode(f).value();
    ASSERT_EQ(decoded.size(), 2u);
    EXPECT_EQ(decoded[0].name(), "ResponseKind");
    EXPECT_EQ(decoded[1].name(), "AlwaysPresent");
  }
}

TEST(Parser, ExtendedMultiplexingRanges) {
  const auto r = load_ok("multiplexed.dbc");
  const Message* m = r.database.find_message("RangeMux");
  ASSERT_NE(m, nullptr);
  const Signal* high = m->find_signal("HighGroup");
  ASSERT_NE(high, nullptr);
  ASSERT_EQ(high->multiplex_ranges().size(), 2u);
  EXPECT_EQ(high->multiplex_ranges()[0].low, 10u);
  EXPECT_EQ(high->multiplex_ranges()[0].high, 20u);
  EXPECT_EQ(high->multiplex_ranges()[1].low, 30u);
  EXPECT_EQ(high->multiplexor_name(), "Selector");

  const auto active_names = [&](std::uint8_t selector) {
    const Frame f = Frame::make(m->id(), {selector, 0x11, 0x22, 0, 0, 0, 0, 0}).value();
    // The Result is bound to a named local first: in C++17 a temporary in the
    // range expression of a range-for is destroyed before the loop body runs,
    // so iterating m->decode(f).value() directly would dangle.
    const auto decoded = m->decode(f);
    std::vector<std::string> names;
    for (const auto& d : decoded.value()) {
      names.emplace_back(d.name());
    }
    return names;
  };
  EXPECT_EQ(active_names(0), (std::vector<std::string>{"Selector", "LowGroup"}));
  EXPECT_EQ(active_names(4), (std::vector<std::string>{"Selector", "LowGroup"}));
  EXPECT_EQ(active_names(5), (std::vector<std::string>{"Selector"}));
  EXPECT_EQ(active_names(15), (std::vector<std::string>{"Selector", "HighGroup"}));
  EXPECT_EQ(active_names(30), (std::vector<std::string>{"Selector", "HighGroup"}));
  EXPECT_EQ(active_names(31), (std::vector<std::string>{"Selector"}));
}

TEST(Parser, NestedMultiplexing) {
  const auto r = load_ok("multiplexed.dbc");
  const Message* m = r.database.find_message("NestedMux");
  ASSERT_NE(m, nullptr);
  EXPECT_EQ(m->find_signal("InnerMux")->multiplex_role(), MultiplexRole::Both)
      << "m1M means multiplexed and multiplexor at the same time";
  ASSERT_NE(m->multiplexor(), nullptr);
  EXPECT_EQ(m->multiplexor()->name(), "OuterMux");

  const auto active_names = [&](std::uint8_t outer, std::uint8_t inner) {
    const auto byte0 =
        static_cast<std::uint8_t>((outer & 0x0Fu) | ((inner & 0x0Fu) << 4u));
    const Frame f =
        Frame::make(m->id(), {byte0, 0x11, 0x22, 0x33, 0x44, 0, 0, 0}).value();
    const auto decoded = m->decode(f);
    std::vector<std::string> names;
    for (const auto& d : decoded.value()) {
      names.emplace_back(d.name());
    }
    return names;
  };

  // Outer 1 selects InnerMux; inner 0 then selects LeafOne.
  EXPECT_EQ(active_names(1, 0),
            (std::vector<std::string>{"OuterMux", "InnerMux", "LeafOne"}));
  // inner 1 selects LeafTwo instead.
  EXPECT_EQ(active_names(1, 1),
            (std::vector<std::string>{"OuterMux", "InnerMux", "LeafTwo"}));
  // inner 4 is in LeafOne's second range.
  EXPECT_EQ(active_names(1, 4),
            (std::vector<std::string>{"OuterMux", "InnerMux", "LeafOne"}));
  // inner 7 selects neither leaf.
  EXPECT_EQ(active_names(1, 7), (std::vector<std::string>{"OuterMux", "InnerMux"}));
  // Outer 2 does not select InnerMux at all, so no leaf can be present even
  // though the inner nibble still reads as a valid selector.
  EXPECT_EQ(active_names(2, 0), (std::vector<std::string>{"OuterMux", "OuterOnly"}));
  // Outer 0 selects nothing.
  EXPECT_EQ(active_names(0, 0), (std::vector<std::string>{"OuterMux"}));
}

TEST(Parser, ValueTables) {
  const auto r = load_ok("valuetables.dbc");
  const auto& db = r.database;
  ASSERT_EQ(db.value_tables().size(), 3u);
  const core::ValueTable* on_off = db.find_value_table("OnOff");
  ASSERT_NE(on_off, nullptr);
  ASSERT_EQ(on_off->values.size(), 2u);
  EXPECT_EQ(on_off->values[0].value, 1);
  EXPECT_EQ(on_off->values[0].text, "On");

  const Message* m = db.find_message("BodyStatus");
  ASSERT_NE(m, nullptr);
  const Signal* head = m->find_signal("HeadlightState");
  ASSERT_NE(head, nullptr);
  ASSERT_EQ(head->value_descriptions().size(), 4u);
  EXPECT_EQ(head->describe(2.0), "LowBeam");
  EXPECT_EQ(head->describe_raw(3), "HighBeam");
  EXPECT_TRUE(head->describe(9.0).empty());
}

TEST(Parser, ValueTableOnAScaledSignalKeysOnTheRawValue) {
  const auto r = load_ok("valuetables.dbc");
  const Signal* s = r.database.find_message("BodyStatus")->find_signal("ScaledEnum");
  ASSERT_NE(s, nullptr);
  EXPECT_DOUBLE_EQ(s->layout().factor, 2.0);
  EXPECT_EQ(s->describe(4.0), "TwoRaw") << "physical 4 is raw 2";
  EXPECT_EQ(s->describe(8.0), "FourRaw");
}

TEST(Parser, NegativeValueTableEntries) {
  const auto r = load_ok("valuetables.dbc");
  const Signal* s = r.database.find_message("BodyStatus")->find_signal("SignedEnum");
  ASSERT_NE(s, nullptr);
  EXPECT_EQ(s->describe(-1.0), "MinusOne");
  EXPECT_EQ(s->describe(-128.0), "Floor");
  EXPECT_EQ(s->describe(0.0), "Zero");
}

TEST(Parser, FloatAndDoubleSignals) {
  const auto r = load_ok("floats.dbc");
  const auto& db = r.database;

  const Message* pair = db.find_message("FloatPair");
  ASSERT_NE(pair, nullptr);
  EXPECT_EQ(pair->find_signal("PressureIntel")->layout().value_type,
            ValueType::Float32);
  EXPECT_EQ(pair->find_signal("PressureMotorola")->layout().value_type,
            ValueType::Float32);
  EXPECT_EQ(
      db.find_message("DoubleIntel")->find_signal("PreciseValue")->layout().value_type,
      ValueType::Float64);
  EXPECT_EQ(
      db.find_message("ScaledFloat")->find_signal("PlainInteger")->layout().value_type,
      ValueType::Integer);

  // 1.0f is 0x3F800000, little endian on the wire.
  const Frame f =
      Frame::make(pair->id(), {0x00, 0x00, 0x80, 0x3F, 0x3F, 0x80, 0x00, 0x00}).value();
  EXPECT_FLOAT_EQ(static_cast<float>(pair->find_signal("PressureIntel")->decode(f)),
                  1.0F);
  EXPECT_FLOAT_EQ(static_cast<float>(pair->find_signal("PressureMotorola")->decode(f)),
                  1.0F);
}

TEST(Parser, ScaledFloatAppliesFactorAndOffset) {
  const auto r = load_ok("floats.dbc");
  const Message* m = r.database.find_message("ScaledFloat");
  ASSERT_NE(m, nullptr);
  const Signal* s = m->find_signal("ScaledIeee");
  // 3.0f = 0x40400000 -> physical 3*2 + 1 = 7
  const Frame f = Frame::make(m->id(), {0x00, 0x00, 0x40, 0x40, 0, 0, 0, 0}).value();
  EXPECT_DOUBLE_EQ(s->decode(f), 7.0);

  auto out = Frame::make_empty(m->id(), 8).value();
  ASSERT_TRUE(s->encode(7.0, out).has_value());
  EXPECT_DOUBLE_EQ(s->decode(out), 7.0);
}

TEST(Parser, DoubleSignalsRoundTripBothByteOrders) {
  const auto r = load_ok("floats.dbc");
  for (const char* name : {"DoubleIntel", "DoubleMotorola"}) {
    const Message* m = r.database.find_message(name);
    ASSERT_NE(m, nullptr) << name;
    const Signal& s = m->signals().front();
    auto f = Frame::make_empty(m->id(), 8).value();
    for (const double v : {0.0, 1.0, -1.0, 3.141592653589793, 1e300, -2.5e-8}) {
      ASSERT_TRUE(s.encode(v, f).has_value()) << name;
      EXPECT_DOUBLE_EQ(s.decode(f), v) << name << " value " << v;
    }
  }
}

TEST(Parser, EveryAttributeType) {
  const auto r = load_ok("attributes.dbc");
  const auto& db = r.database;

  EXPECT_EQ(db.attributes().at("DbInt").as_int(), -42);
  EXPECT_EQ(db.attributes().at("DbInt").type(), core::AttributeType::Int);
  EXPECT_EQ(db.attributes().at("DbHex").as_int(), 48879);
  EXPECT_EQ(db.attributes().at("DbHex").type(), core::AttributeType::Hex);
  EXPECT_DOUBLE_EQ(db.attributes().at("DbFloat").number(), -1.25);
  EXPECT_EQ(db.attributes().at("DbString").text(), "a database attribute");
  // In BA_ an enum is written as an index; the name is resolved from BA_DEF_.
  EXPECT_EQ(db.attributes().at("DbEnum").as_int(), 2);
  EXPECT_EQ(db.attributes().at("DbEnum").text(), "Gamma");

  // In BA_DEF_DEF_ the same enum is written as a name.
  const auto* def = db.find_attribute_definition("DbEnum");
  ASSERT_NE(def, nullptr);
  ASSERT_EQ(def->enum_values.size(), 3u);
  EXPECT_EQ(def->default_value.text(), "Beta");
  EXPECT_EQ(def->default_value.as_int(), 1);

  const core::Node* a = db.find_node("NodeA");
  ASSERT_NE(a, nullptr);
  EXPECT_EQ(a->attributes.at("NodeInt").as_int(), 3);
  EXPECT_EQ(a->attributes.at("NodeEnum").text(), "Master");

  const Message* m = db.find_message("AttrMessage");
  ASSERT_NE(m, nullptr);
  EXPECT_EQ(m->attributes().at("GenMsgCycleTime").as_int(), 50);
  EXPECT_DOUBLE_EQ(m->attributes().at("MsgFloat").number(), 12.5);
  EXPECT_EQ(m->attributes().at("VFrameFormat").text(), "StandardCAN");
  EXPECT_DOUBLE_EQ(
      m->find_signal("AttrSignal")->attributes().at("GenSigStartValue").number(), 17.5);
  EXPECT_EQ(m->find_signal("AttrSignal")->attributes().at("SigString").text(),
            "signal attribute");
  EXPECT_EQ(m->find_signal("OtherSignal")->attributes().at("SigEnum").text(), "Cooked");
}

TEST(Parser, QuirkyFileParses) {
  auto parsed = parse_file(data_path("quirks.dbc"));
  ASSERT_TRUE(parsed.has_value());
  const ParseResult r = std::move(parsed).value();
  EXPECT_FALSE(r.diagnostics.has_errors()) << r.diagnostics.format("quirks.dbc");

  EXPECT_TRUE(r.had_crlf);
  EXPECT_TRUE(r.had_latin1);
  EXPECT_TRUE(r.had_no_final_newline);

  const auto& db = r.database;
  // Vector__XXX in the node list is a placeholder, not a node.
  EXPECT_EQ(db.find_node("Vector__XXX"), nullptr);
  EXPECT_NE(db.find_node("3PMS"), nullptr) << "node names may start with a digit";

  const Message* ext = db.find_message("ExtendedNoFlagCheck");
  ASSERT_NE(ext, nullptr);
  EXPECT_TRUE(ext->id().is_extended());
  EXPECT_EQ(ext->id().value(), 0x100u);

  // Missing transmitter is written as Vector__XXX and stored as empty.
  const Message* none = db.find_message("NoTransmitter");
  ASSERT_NE(none, nullptr);
  EXPECT_TRUE(none->transmitter().empty());

  // A comment without its trailing semicolon is still attached, and the
  // Latin-1 bytes came through as UTF-8.
  EXPECT_NE(none->comment().find("Grad"), std::string::npos);
  EXPECT_NE(none->comment().find("\xC3\xBC"), std::string::npos)
      << "the u-umlaut should be UTF-8 encoded";

  // SIG_VALTYPE_ without a colon or a semicolon.
  const Message* fd = db.find_message("FdMessage");
  ASSERT_NE(fd, nullptr);
  EXPECT_TRUE(fd->is_fd()) << "a 24 byte message can only be CAN FD";
  EXPECT_EQ(fd->dlc(), 24u);
  EXPECT_EQ(fd->find_signal("WideSignal")->layout().value_type, ValueType::Integer);
}

TEST(Parser, ByteOrderMarkIsStripped) {
  auto parsed = parse_file(data_path("bom.dbc"));
  ASSERT_TRUE(parsed.has_value());
  const ParseResult r = std::move(parsed).value();
  EXPECT_TRUE(r.had_bom);
  EXPECT_FALSE(r.diagnostics.has_errors()) << r.diagnostics.format("bom.dbc");
  EXPECT_EQ(r.database.version(), "1.0 bom");
  EXPECT_NE(r.database.find_message("M"), nullptr);
}

TEST(Parser, FdMessageDecodesThroughAnFdFrame) {
  auto parsed = parse_file(data_path("quirks.dbc"));
  ASSERT_TRUE(parsed.has_value());
  const ParseResult r = std::move(parsed).value();
  const Message* fd = r.database.find_message("FdMessage");
  ASSERT_NE(fd, nullptr);

  std::array<std::uint8_t, 24> payload{};
  payload[0] = 0xEF;
  payload[22] = 0x34;
  payload[23] = 0x12;
  const auto f = core::FdFrame::make(fd->id(), payload.data(), payload.size(),
                                     core::FrameFlags::Fd);
  ASSERT_TRUE(f.has_value()) << f.error().message();
  EXPECT_DOUBLE_EQ(fd->find_signal("WideSignal")->decode(f.value()), 239.0);
  EXPECT_DOUBLE_EQ(fd->find_signal("TailSignal")->decode(f.value()), 0x1234);
}

TEST(Parser, MessageEncodeByName) {
  const auto r = load_ok("powertrain.dbc");
  const Message* m = r.database.find_message("EngineData");
  ASSERT_NE(m, nullptr);
  const auto frame = m->encode({{"EngineSpeed", 1500.0},
                                {"EngineCoolantTemp", 90.0},
                                {"ThrottlePosition", 40.0}});
  ASSERT_TRUE(frame.has_value()) << frame.error().message();
  EXPECT_DOUBLE_EQ(m->find_signal("EngineSpeed")->decode(*frame), 1500.0);
  EXPECT_DOUBLE_EQ(m->find_signal("EngineCoolantTemp")->decode(*frame), 90.0);
  EXPECT_DOUBLE_EQ(m->find_signal("ThrottlePosition")->decode(*frame), 40.0);

  EXPECT_FALSE(m->encode({{"NoSuchSignal", 1.0}}).has_value());
}

TEST(Parser, DatabaseDecodeDispatchesOnIdentifier) {
  const auto r = load_ok("powertrain.dbc");
  const Frame f =
      Frame::make(CanId::standard(256).value(), {0x40, 0x1F, 0x82, 0, 0, 0, 0, 0})
          .value();
  const auto decoded = r.database.decode(f);
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(decoded->front().name(), "EngineSpeed");
  EXPECT_DOUBLE_EQ(decoded->front().value, 1000.0);  // 0x1F40 = 8000 * 0.125

  const Frame unknown = Frame::make(CanId::standard(0x7AB).value(), {0}).value();
  EXPECT_FALSE(r.database.decode(unknown).has_value());
}

}  // namespace
}  // namespace canforge::dbc
