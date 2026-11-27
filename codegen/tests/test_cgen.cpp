// SPDX-License-Identifier: MIT
//
// Tests for gen-c. The unit tests pin the naming and typing rules. The proof
// test is the one that matters: it compiles the generated C with a real C
// compiler, feeds it 20 000 random value sets per message, and demands that
// every packed byte equals what Signal::encode produces for the same values.

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "canforge/codegen/CGen.hpp"
#include "canforge/core/Frame.hpp"
#include "canforge/dbc/Parser.hpp"

namespace {

namespace cg = canforge::codegen;
namespace core = canforge::core;
namespace fs = std::filesystem;

using core::ByteOrder;
using core::Signedness;

constexpr int kCasesPerMessage = 20000;

core::Database load(const std::string& path) {
  auto db = canforge::dbc::load(path);
  EXPECT_TRUE(static_cast<bool>(db)) << "cannot load " << path;
  return db ? db.value() : core::Database{};
}

core::Database parse(std::string_view text) {
  auto result = canforge::dbc::parse_string(text);
  EXPECT_TRUE(result.ok()) << result.diagnostics.format("<test>");
  return result.database;
}

core::SignalLayout layout(std::uint16_t start, std::uint8_t bits, ByteOrder order,
                          Signedness sign, double factor, double offset, double lo,
                          double hi) {
  core::SignalLayout l;
  l.start_bit = start;
  l.bit_length = bits;
  l.byte_order = order;
  l.signedness = sign;
  l.factor = factor;
  l.offset = offset;
  l.minimum = lo;
  l.maximum = hi;
  return l;
}

bool contains(const std::string& text, std::string_view needle) {
  return text.find(needle) != std::string::npos;
}

// Naming

TEST(CGenNames, SnakeCase) {
  EXPECT_EQ(cg::snake_case("BTMS_CoolantTemp"), "btms_coolant_temp");
  EXPECT_EQ(cg::snake_case("VCU_Cmd"), "vcu_cmd");
  EXPECT_EQ(cg::snake_case("CSN_CellTemp1"), "csn_cell_temp1");
  EXPECT_EQ(cg::snake_case("EngineSpeedJ1939"), "engine_speed_j1939");
  EXPECT_EQ(cg::snake_case("VCUCmd"), "vcu_cmd");
  EXPECT_EQ(cg::snake_case("Temp1Max"), "temp1_max");
  EXPECT_EQ(cg::snake_case("BTMS_CtrlP"), "btms_ctrl_p");
  EXPECT_EQ(cg::snake_case("already_snake"), "already_snake");
  EXPECT_EQ(cg::snake_case("ALLCAPS"), "allcaps");
}

TEST(CGenNames, AlwaysAnIdentifier) {
  EXPECT_EQ(cg::snake_case("2ndGear"), "_2nd_gear");
  EXPECT_EQ(cg::snake_case("Int"), "int_");       // a keyword once lower-cased
  EXPECT_EQ(cg::snake_case("a  b--c"), "a_b_c");  // runs of separators collapse
  EXPECT_EQ(cg::snake_case("Trailing_"), "trailing");
  EXPECT_EQ(cg::snake_case(""), "_");
}

// Types

TEST(CGenTypes, UnscaledSignalsKeepTheirRawInteger) {
  const auto u = [](std::uint8_t bits) {
    return cg::field_type(
        layout(0, bits, ByteOrder::Intel, Signedness::Unsigned, 1, 0, 0, 0));
  };
  const auto s = [](std::uint8_t bits) {
    return cg::field_type(
        layout(0, bits, ByteOrder::Intel, Signedness::Signed, 1, 0, 0, 0));
  };
  EXPECT_EQ(u(1), cg::CType::U8);
  EXPECT_EQ(u(8), cg::CType::U8);
  EXPECT_EQ(u(9), cg::CType::U16);
  EXPECT_EQ(u(16), cg::CType::U16);
  EXPECT_EQ(u(17), cg::CType::U32);
  EXPECT_EQ(u(32), cg::CType::U32);
  EXPECT_EQ(u(33), cg::CType::U64);
  EXPECT_EQ(u(64), cg::CType::U64);
  EXPECT_EQ(s(8), cg::CType::I8);
  EXPECT_EQ(s(13), cg::CType::I16);
  EXPECT_EQ(s(20), cg::CType::I32);
  EXPECT_EQ(s(40), cg::CType::I64);
}

TEST(CGenTypes, ScaledSignalsBecomeFloat) {
  // A factor, an offset, or a fractional limit each make a signal scaled.
  EXPECT_EQ(cg::field_type(layout(0, 8, ByteOrder::Intel, Signedness::Unsigned, 0.5,
                                  -40, -40, 87)),
            cg::CType::F32);
  EXPECT_EQ(cg::field_type(
                layout(0, 8, ByteOrder::Intel, Signedness::Unsigned, 1, -40, -40, 215)),
            cg::CType::F32);
  EXPECT_EQ(cg::field_type(
                layout(0, 8, ByteOrder::Intel, Signedness::Unsigned, 1, 0, 0, 99.5)),
            cg::CType::F32);
  EXPECT_EQ(cg::field_type(layout(0, 16, ByteOrder::Intel, Signedness::Signed, 0.01, 0,
                                  -327, 327)),
            cg::CType::F32);
}

TEST(CGenTypes, FloatIsNotTrustedBeyondItsPrecision) {
  // 24 bits scaled: one raw step is below what a float can resolve up there.
  EXPECT_EQ(cg::field_type(
                layout(0, 24, ByteOrder::Intel, Signedness::Unsigned, 0.001, 0, 0, 0)),
            cg::CType::F64);
  // Eight bits, but riding on a huge offset: same problem.
  EXPECT_EQ(cg::field_type(layout(0, 8, ByteOrder::Intel, Signedness::Unsigned, 0.001,
                                  1000, 0, 0)),
            cg::CType::F64);
}

// Rendering

constexpr std::string_view kSmallDbc = R"(VERSION ""
NS_ :
BS_:
BU_: ECU TOOL
BO_ 256 EcuStatus: 8 ECU
 SG_ CoolantTemp : 0|8@1+ (0.5,-40) [-40|87] "degC" TOOL
 SG_ PumpDuty : 16|7@1+ (1,0) [0|100] "%" TOOL
 SG_ Mode : 32|2@1+ (1,0) [0|2] "" TOOL
BO_ 512 ToolCmd: 8 TOOL
 SG_ Target : 0|16@1- (0.01,0) [-327|327] "" ECU
BO_ 2016 DiagRequest: 8 TOOL
BA_DEF_ BO_ "GenMsgCycleTime" INT 0 10000;
BA_DEF_DEF_ "GenMsgCycleTime" 0;
BA_ "GenMsgCycleTime" BO_ 256 100;
VAL_ 256 Mode 0 "INIT" 1 "NORMAL" 2 "SAFE" ;
)";

cg::CGenOptions options(std::string prefix, std::string node) {
  cg::CGenOptions opt;
  opt.prefix = std::move(prefix);
  opt.node = std::move(node);
  opt.source_name = "small.dbc";
  return opt;
}

TEST(CGenRender, HeaderHasMacrosStructAndPrototypes) {
  const core::Database db = parse(kSmallDbc);
  const auto out = cg::render_c(db, options("ecu", "ECU"));
  ASSERT_TRUE(static_cast<bool>(out));
  const std::string& h = out.value().header;

  EXPECT_TRUE(contains(h, "Generated by canforge gen-c from small.dbc for node ECU."));
  EXPECT_TRUE(contains(h, "#ifndef ECU_COM_H"));
  EXPECT_TRUE(contains(h, "#define ECU_STATUS_ID 0x100u"));
  EXPECT_TRUE(contains(h, "#define ECU_STATUS_DLC 8u"));
  EXPECT_TRUE(contains(h, "#define ECU_STATUS_CYCLE_MS 100u"));
  EXPECT_TRUE(contains(h, "#define TOOL_CMD_CYCLE_MS 0u"));
  EXPECT_TRUE(contains(h, "#define ECU_STATUS_COOLANT_TEMP_MIN (-40.0f)"));
  EXPECT_TRUE(contains(h, "#define ECU_STATUS_COOLANT_TEMP_MAX 87.0f"));
  EXPECT_TRUE(contains(h, "#define ECU_STATUS_PUMP_DUTY_MAX 100u"));
  EXPECT_TRUE(contains(h, "#define ECU_STATUS_MODE_VAL_SAFE 2u"));
  EXPECT_TRUE(contains(h, "    float coolant_temp;"));
  EXPECT_TRUE(contains(h, "    uint8_t pump_duty;"));
  EXPECT_TRUE(contains(h, "} ecu_status_t;"));
  EXPECT_TRUE(
      contains(h, "void ecu_status_pack(const ecu_status_t *m, uint8_t data[8]);"));
  EXPECT_TRUE(
      contains(h, "void ecu_status_unpack(ecu_status_t *m, const uint8_t data[8]);"));
  EXPECT_TRUE(out.value().skipped.empty());
}

TEST(CGenRender, ANamedValueCalledMaxDoesNotCollideWithTheLimit) {
  const core::Database db = parse(R"(VERSION ""
NS_ :
BS_:
BU_: N
BO_ 1 Cmd: 8 N
 SG_ Mode : 0|2@1+ (1,0) [0|3] "" N
VAL_ 1 Mode 0 "AUTO" 1 "MAX" 2 "Min" 3 "Not available" ;
)");
  const auto out = cg::render_c(db, options("x", ""));
  ASSERT_TRUE(static_cast<bool>(out));
  const std::string& h = out.value().header;
  EXPECT_TRUE(contains(h, "#define CMD_MODE_MAX 3u"));  // the limit
  EXPECT_TRUE(contains(h, "#define CMD_MODE_MIN 0u"));
  EXPECT_TRUE(contains(h, "#define CMD_MODE_VAL_AUTO 0u"));
  EXPECT_TRUE(contains(h, "#define CMD_MODE_VAL_MAX 1u"));  // the named value
  EXPECT_TRUE(contains(h, "#define CMD_MODE_VAL_MIN 2u"));
  EXPECT_TRUE(contains(h, "#define CMD_MODE_VAL_NOT_AVAILABLE 3u"));
}

TEST(CGenRender, MessageWithoutSignalsGetsMacrosButNoStruct) {
  const core::Database db = parse(kSmallDbc);
  const auto out = cg::render_c(db, options("ecu", "ECU"));
  ASSERT_TRUE(static_cast<bool>(out));
  EXPECT_TRUE(contains(out.value().header, "#define DIAG_REQUEST_ID 0x7E0u"));
  EXPECT_FALSE(contains(out.value().header, "diag_request_t"));
  EXPECT_FALSE(contains(out.value().source, "diag_request_pack"));
}

TEST(CGenRender, TablesFollowTheNode) {
  const core::Database db = parse(kSmallDbc);

  const auto ecu = cg::render_c(db, options("ecu", "ECU"));
  ASSERT_TRUE(static_cast<bool>(ecu));
  EXPECT_TRUE(contains(ecu.value().header, "#define ECU_RX_COUNT 1u"));
  EXPECT_TRUE(contains(ecu.value().header, "#define ECU_TX_COUNT 1u"));
  EXPECT_TRUE(contains(ecu.value().source,
                       "{TOOL_CMD_ID, TOOL_CMD_DLC, tool_cmd_unpack_any},"));
  EXPECT_TRUE(contains(ecu.value().source,
                       "{ECU_STATUS_ID, ECU_STATUS_DLC, ECU_STATUS_CYCLE_MS},"));

  const auto tool = cg::render_c(db, options("tool", "TOOL"));
  ASSERT_TRUE(static_cast<bool>(tool));
  EXPECT_TRUE(contains(tool.value().header, "#define TOOL_RX_COUNT 1u"));
  EXPECT_TRUE(contains(tool.value().header, "#define TOOL_TX_COUNT 2u"));

  const auto none = cg::render_c(db, options("any", ""));
  ASSERT_TRUE(static_cast<bool>(none));
  EXPECT_FALSE(contains(none.value().header, "_RX_COUNT"));
  EXPECT_FALSE(contains(none.value().source, "_rx_table"));
}

TEST(CGenRender, OnlyTheIncludesAFirmwareCanAfford) {
  const core::Database db = parse(kSmallDbc);
  const auto out = cg::render_c(db, options("ecu", "ECU"));
  ASSERT_TRUE(static_cast<bool>(out));
  const std::string both = out.value().header + out.value().source;
  EXPECT_FALSE(contains(both, "<math.h>"));
  EXPECT_FALSE(contains(both, "<string.h>"));
  EXPECT_FALSE(contains(both, "<stdlib.h>"));
  EXPECT_FALSE(contains(both, "malloc"));
  EXPECT_FALSE(contains(both, "for ("));
  EXPECT_FALSE(contains(both, "while ("));
}

TEST(CGenRender, IsDeterministic) {
  const core::Database db = parse(kSmallDbc);
  const auto a = cg::render_c(db, options("ecu", "ECU"));
  const auto b = cg::render_c(db, options("ecu", "ECU"));
  ASSERT_TRUE(static_cast<bool>(a));
  ASSERT_TRUE(static_cast<bool>(b));
  EXPECT_EQ(a.value().header, b.value().header);
  EXPECT_EQ(a.value().source, b.value().source);
}

// --raw: integers only, for 8-bit targets

cg::CGenOptions raw_options(std::string prefix, std::string node) {
  cg::CGenOptions opt = options(std::move(prefix), std::move(node));
  opt.raw = true;
  return opt;
}

TEST(CGenRaw, EveryFieldGetsItsRawInteger) {
  EXPECT_EQ(cg::raw_field_type(layout(0, 8, ByteOrder::Intel, Signedness::Unsigned, 0.5,
                                      -40, -40, 87)),
            cg::CType::U8);
  EXPECT_EQ(cg::raw_field_type(layout(0, 16, ByteOrder::Intel, Signedness::Signed, 0.01,
                                      0, -327, 327)),
            cg::CType::I16);
  EXPECT_EQ(cg::raw_field_type(
                layout(0, 24, ByteOrder::Intel, Signedness::Unsigned, 0.001, 0, 0, 0)),
            cg::CType::U32);
}

TEST(CGenRaw, NoFloatingPointAnywhere) {
  const core::Database db = parse(kSmallDbc);
  const auto out = cg::render_c(db, raw_options("ecu", "ECU"));
  ASSERT_TRUE(static_cast<bool>(out));
  const std::string& h = out.value().header;
  const std::string both = h + out.value().source;
  EXPECT_FALSE(contains(both, "float"));
  EXPECT_FALSE(contains(both, "double"));
  EXPECT_FALSE(contains(both, "_round_"));
  EXPECT_TRUE(contains(h, "Raw integers only (--raw)"));
  EXPECT_TRUE(
      contains(h, "    uint8_t coolant_temp; /* 0|8@1+ (0.5,-40) [-40|87] degC */"));
  EXPECT_TRUE(contains(h, "    int16_t target;"));
  // The limits are the raw range.
  EXPECT_TRUE(contains(h, "#define ECU_STATUS_COOLANT_TEMP_MIN 0u"));
  EXPECT_TRUE(contains(h, "#define ECU_STATUS_COOLANT_TEMP_MAX 254u"));
  EXPECT_TRUE(contains(h, "#define TOOL_CMD_TARGET_MIN (-32700)"));
  EXPECT_TRUE(contains(h, "#define TOOL_CMD_TARGET_MAX 32700"));
  EXPECT_TRUE(contains(h, "#define ECU_STATUS_MODE_VAL_SAFE 2u"));
  EXPECT_TRUE(contains(h, "not clamped to the limits above"));
}

TEST(CGenRaw, PackDoesNotClampToTheDeclaredRange) {
  // 255 is outside [-40|87] and is exactly what a node sends for a broken
  // sensor: it must reach the wire. Only the bit length limits a raw value.
  const core::Database db = parse(kSmallDbc);
  const auto out = cg::render_c(db, raw_options("ecu", "ECU"));
  ASSERT_TRUE(static_cast<bool>(out));
  const std::string& c = out.value().source;
  EXPECT_FALSE(contains(c, "if (raw >"));
  EXPECT_FALSE(contains(c, "if (raw <"));
  EXPECT_FALSE(contains(c, "if (value"));
}

TEST(CGenRaw, LeavesTheDefaultOutputAlone) {
  const core::Database db = parse(kSmallDbc);
  const auto plain = cg::render_c(db, options("ecu", "ECU"));
  const auto raw = cg::render_c(db, raw_options("ecu", "ECU"));
  ASSERT_TRUE(static_cast<bool>(plain));
  ASSERT_TRUE(static_cast<bool>(raw));
  EXPECT_NE(plain.value().header, raw.value().header);
  EXPECT_TRUE(contains(plain.value().header, "    float coolant_temp;"));
  EXPECT_FALSE(contains(plain.value().header, "--raw"));
}

TEST(CGenRender, RejectsBadOptions) {
  const core::Database db = parse(kSmallDbc);
  EXPECT_FALSE(static_cast<bool>(cg::render_c(db, options("", "ECU"))));
  EXPECT_FALSE(static_cast<bool>(cg::render_c(db, options("9lives", "ECU"))));
  EXPECT_FALSE(static_cast<bool>(cg::render_c(db, options("has space", "ECU"))));
  EXPECT_FALSE(static_cast<bool>(cg::render_c(db, options("ecu", "NOBODY"))));
}

TEST(CGenRender, SkipsWhatItCannotGenerateAndSaysSo) {
  const core::Database mux = load(CANFORGE_DBC_TEST_DATA "/multiplexed.dbc");
  const auto out = cg::render_c(mux, options("mux", ""));
  ASSERT_TRUE(static_cast<bool>(out));
  ASSERT_FALSE(out.value().skipped.empty());
  EXPECT_TRUE(contains(out.value().skipped.front(), "multiplexed"));

  const core::Database floats = load(CANFORGE_DBC_TEST_DATA "/floats.dbc");
  const auto fout = cg::render_c(floats, options("flt", ""));
  ASSERT_TRUE(static_cast<bool>(fout));
  bool mentions_float = false;
  for (const std::string& line : fout.value().skipped) {
    mentions_float = mentions_float || contains(line, "SIG_VALTYPE_");
  }
  EXPECT_TRUE(mentions_float);
}

TEST(CGenRender, TwoMessagesWithOneCNameAreAnError) {
  const core::Database db = parse(R"(VERSION ""
NS_ :
BS_:
BU_: N
BO_ 1 EngineData: 8 N
 SG_ A : 0|8@1+ (1,0) [0|0] "" N
BO_ 2 Engine_Data: 8 N
 SG_ B : 0|8@1+ (1,0) [0|0] "" N
)");
  EXPECT_FALSE(static_cast<bool>(cg::render_c(db, options("x", ""))));
}

// The proof: generated C against the runtime codec

bool is_float(cg::CType t) {
  return t == cg::CType::F32 || t == cg::CType::F64;
}

bool is_signed(cg::CType t) {
  return t == cg::CType::I8 || t == cg::CType::I16 || t == cg::CType::I32 ||
         t == cg::CType::I64;
}

std::int64_t int_min(cg::CType t) {
  switch (t) {
    case cg::CType::I8:
      return std::numeric_limits<std::int8_t>::min();
    case cg::CType::I16:
      return std::numeric_limits<std::int16_t>::min();
    case cg::CType::I32:
      return std::numeric_limits<std::int32_t>::min();
    default:
      return std::numeric_limits<std::int64_t>::min();
  }
}

std::int64_t int_max(cg::CType t) {
  switch (t) {
    case cg::CType::I8:
      return std::numeric_limits<std::int8_t>::max();
    case cg::CType::I16:
      return std::numeric_limits<std::int16_t>::max();
    case cg::CType::I32:
      return std::numeric_limits<std::int32_t>::max();
    default:
      return std::numeric_limits<std::int64_t>::max();
  }
}

std::uint64_t uint_max(cg::CType t) {
  switch (t) {
    case cg::CType::U8:
      return std::numeric_limits<std::uint8_t>::max();
    case cg::CType::U16:
      return std::numeric_limits<std::uint16_t>::max();
    case cg::CType::U32:
      return std::numeric_limits<std::uint32_t>::max();
    default:
      return std::numeric_limits<std::uint64_t>::max();
  }
}

/// One input value for one struct field. Exactly one member is meaningful,
/// selected by the field's C type.
struct Value {
  double real = 0.0;
  std::int64_t sint = 0;
  std::uint64_t uint = 0;
};

/// A physical value for a scaled signal. The mix is deliberate: values on the
/// raw grid, values between grid points, values far outside, and the limits
/// themselves. Off-grid values keep 0.1 raw steps away from a rounding
/// boundary, so float and double arithmetic must round them the same way.
double pick_physical(std::mt19937_64& rng, const core::SignalLayout& l) {
  const auto raw_lo = static_cast<std::int64_t>(l.raw_minimum());
  const auto raw_hi = static_cast<std::int64_t>(l.raw_maximum());
  std::uniform_int_distribution<std::int64_t> any_raw(raw_lo, raw_hi);
  std::uniform_real_distribution<double> unit(0.0, 1.0);
  const double floor = l.physical_floor();
  const double ceiling = l.physical_ceiling();
  const double span = ceiling - floor;

  switch (rng() % 8u) {
    case 0:
    case 1:
    case 2:
    case 3:
      return static_cast<double>(any_raw(rng)) * l.factor + l.offset;
    case 4:
    case 5:
      return (static_cast<double>(any_raw(rng)) + (unit(rng) * 0.8 - 0.4)) * l.factor +
             l.offset;
    case 6:
      return (rng() % 2u) == 0u ? floor - unit(rng) * 2.0 * span
                                : ceiling + unit(rng) * 2.0 * span;
    default:
      if (l.has_range()) {
        return (rng() % 2u) == 0u ? l.minimum : l.maximum;
      }
      return (rng() % 2u) == 0u ? floor : ceiling;
  }
}

Value pick(std::mt19937_64& rng, const core::SignalLayout& l, cg::CType type) {
  Value v;
  if (type == cg::CType::F32) {
    // What the C side will hold after reading the text back: a float.
    v.real = static_cast<double>(static_cast<float>(pick_physical(rng, l)));
  } else if (type == cg::CType::F64) {
    v.real = pick_physical(rng, l);
  } else if (is_signed(type)) {
    std::uniform_int_distribution<std::int64_t> any(int_min(type), int_max(type));
    switch (rng() % 8u) {
      case 0:
        v.sint = int_min(type);
        break;
      case 1:
        v.sint = int_max(type);
        break;
      case 2:
        v.sint = static_cast<std::int64_t>(rng() % 33u) - 16;
        break;
      default:
        v.sint = any(rng);
        break;
    }
  } else {
    std::uniform_int_distribution<std::uint64_t> any(0u, uint_max(type));
    switch (rng() % 8u) {
      case 0:
        v.uint = uint_max(type);
        break;
      case 1:
        v.uint = rng() % 17u;
        break;
      default:
        v.uint = any(rng);
        break;
    }
  }
  return v;
}

/// What the runtime codec puts on the wire for one field value.
void encode_expected(const core::SignalLayout& l, cg::CType type, const Value& v,
                     std::uint8_t* data, bool raw) {
  if (raw) {
    // --raw: the value is cut to the signal's bits, never clamped.
    const std::uint64_t bits =
        is_signed(type) ? static_cast<std::uint64_t>(v.sint) : v.uint;
    l.encode_raw(bits & core::bit_mask(l.bit_length), data);
    return;
  }
  if (is_float(type)) {
    ASSERT_TRUE(static_cast<bool>(l.encode(v.real, data)));
    return;
  }
  if (l.bit_length <= 32u) {
    // Exact in a double, so the codec's own clamp and saturation apply.
    const double physical =
        is_signed(type) ? static_cast<double>(v.sint) : static_cast<double>(v.uint);
    ASSERT_TRUE(static_cast<bool>(l.encode(physical, data)));
    return;
  }
  // Wider than a double's mantissa: saturate in the integer domain, the way
  // physical_to_raw does, and write the bits directly.
  ASSERT_FALSE(l.has_range()) << "wide test signals are declared without a range";
  const std::uint64_t mask = core::bit_mask(l.bit_length);
  if (is_signed(type)) {
    const auto hi = static_cast<std::int64_t>(mask >> 1u);
    const std::int64_t lo = -hi - 1;
    const std::int64_t clamped = std::min(std::max(v.sint, lo), hi);
    l.encode_raw(static_cast<std::uint64_t>(clamped), data);
  } else {
    l.encode_raw(std::min(v.uint, mask), data);
  }
}

std::string format_value(cg::CType type, const Value& v) {
  char buf[64] = {};
  if (type == cg::CType::F32) {
    std::snprintf(buf, sizeof(buf), "%.9g", v.real);
  } else if (type == cg::CType::F64) {
    std::snprintf(buf, sizeof(buf), "%.17g", v.real);
  } else if (is_signed(type)) {
    std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(v.sint));
  } else {
    std::snprintf(buf, sizeof(buf), "%llu", static_cast<unsigned long long>(v.uint));
  }
  return buf;
}

struct Generated {
  const core::Message* message = nullptr;
  std::string snake;
  std::vector<cg::CType> types;
};

/// A C program that reads "<message index> <one value per field>" lines, packs
/// each with the generated code, and prints the bytes followed by the values
/// the generated unpack reads back out of them.
std::string harness_source(const std::string& prefix,
                           const std::vector<Generated>& messages) {
  std::string c = "#include <stdio.h>\n#include \"" + prefix + "_com.h\"\n\n";
  for (std::size_t i = 0; i < messages.size(); ++i) {
    const Generated& g = messages[i];
    const auto& signals = g.message->signals();
    const std::string n = std::to_string(unsigned{g.message->dlc()});
    c += "static int run_" + std::to_string(i) + "(void)\n{\n";
    c += "    " + g.snake + "_t m;\n    " + g.snake + "_t r;\n";
    c += "    uint8_t data[" + n + "];\n    unsigned k;\n";
    std::string format;
    std::string args;
    for (std::size_t s = 0; s < signals.size(); ++s) {
      const std::string var = "v" + std::to_string(s);
      const cg::CType t = g.types[s];
      if (t == cg::CType::F32) {
        c += "    float " + var + ";\n";
        format += " %f";
      } else if (t == cg::CType::F64) {
        c += "    double " + var + ";\n";
        format += " %lf";
      } else if (is_signed(t)) {
        c += "    long long " + var + ";\n";
        format += " %lld";
      } else {
        c += "    unsigned long long " + var + ";\n";
        format += " %llu";
      }
      args += ", &" + var;
    }
    c += "    if (scanf(\"" + format + "\"" + args +
         ") != " + std::to_string(signals.size()) + ") {\n        return 0;\n    }\n";
    for (std::size_t s = 0; s < signals.size(); ++s) {
      const std::string field = cg::snake_case(signals[s].name());
      c += "    m." + field + " = (" + cg::to_string(g.types[s]) + ")v" +
           std::to_string(s) + ";\n";
    }
    c += "    " + g.snake + "_pack(&m, data);\n";
    c += "    " + g.snake + "_unpack(&r, data);\n";
    c += "    for (k = 0u; k < " + n +
         "u; ++k) {\n"
         "        printf(\"%02X\", (unsigned)data[k]);\n    }\n";
    for (std::size_t s = 0; s < signals.size(); ++s) {
      const std::string field = "r." + cg::snake_case(signals[s].name());
      const cg::CType t = g.types[s];
      if (t == cg::CType::F32) {
        c += "    printf(\" %.9g\", (double)" + field + ");\n";
      } else if (t == cg::CType::F64) {
        c += "    printf(\" %.17g\", " + field + ");\n";
      } else if (is_signed(t)) {
        c += "    printf(\" %lld\", (long long)" + field + ");\n";
      } else {
        c += "    printf(\" %llu\", (unsigned long long)" + field + ");\n";
      }
    }
    c += "    printf(\"\\n\");\n    return 1;\n}\n\n";
  }
  c += "int main(void)\n{\n    int index;\n"
       "    while (scanf(\"%d\", &index) == 1) {\n"
       "        int ok = 0;\n        switch (index) {\n";
  for (std::size_t i = 0; i < messages.size(); ++i) {
    c += "        case " + std::to_string(i) + ":\n            ok = run_" +
         std::to_string(i) + "();\n            break;\n";
  }
  c +=
      "        default:\n            break;\n        }\n"
      "        if (!ok) {\n            return 1;\n        }\n    }\n    return 0;\n}\n";
  return c;
}

std::string c_compiler() {
  const char* env = std::getenv("CC");
  return (env != nullptr && *env != '\0') ? env : "cc";
}

int run(const std::string& command) {
  return std::system(command.c_str());
}

std::string hex(const std::uint8_t* data, std::size_t n) {
  static const char kDigits[] = "0123456789ABCDEF";
  std::string out;
  for (std::size_t i = 0; i < n; ++i) {
    out += kDigits[data[i] >> 4u];
    out += kDigits[data[i] & 0xFu];
  }
  return out;
}

void prove(const std::string& dbc_path, const std::string& prefix,
           const std::string& node, std::uint64_t seed, bool raw_mode = false) {
  const std::string cc = c_compiler();
  if (run(cc + " --version > /dev/null 2>&1") != 0) {
    GTEST_SKIP() << "no C compiler ('" << cc << "') on this machine";
  }

  const core::Database db = load(dbc_path);
  ASSERT_FALSE(db.messages().empty());

  cg::CGenOptions opt;
  opt.prefix = prefix;
  opt.node = node;
  opt.source_name = fs::path(dbc_path).filename().string();
  opt.raw = raw_mode;
  const fs::path dir = fs::temp_directory_path() /
                       ("canforge_cgen_" + prefix + "_" + std::to_string(seed));
  fs::remove_all(dir);
  opt.out_dir = dir.string();
  ASSERT_TRUE(static_cast<bool>(cg::generate_c(db, opt)));

  std::vector<Generated> messages;
  for (const core::Message& m : db.messages()) {
    if (m.signals().empty() || !cg::unsupported_reason(m).empty()) {
      continue;
    }
    Generated g;
    g.message = &m;
    g.snake = cg::snake_case(m.name());
    for (const core::Signal& s : m.signals()) {
      g.types.push_back(raw_mode ? cg::raw_field_type(s.layout())
                                 : cg::field_type(s.layout()));
    }
    messages.push_back(std::move(g));
  }
  ASSERT_FALSE(messages.empty());

  {
    std::ofstream(dir / "harness.c") << harness_source(prefix, messages);
  }

  // The firmware's own warning set, and then some: generated code that only
  // compiles with warnings off is not worth generating.
  const std::string compile =
      cc +
      " -std=c11 -pedantic -Wall -Wextra -Werror -Wshadow -Wconversion "
      "-Wdouble-promotion -O1 -o '" +
      (dir / "harness").string() + "' '" + (dir / "harness.c").string() + "' '" +
      (dir / (prefix + "_com.c")).string() + "' > '" + (dir / "compile.log").string() +
      "' 2>&1";
  if (run(compile) != 0) {
    std::ifstream log(dir / "compile.log");
    std::stringstream text;
    text << log.rdbuf();
    FAIL() << "the generated C does not compile:\n" << text.str();
  }

  // Inputs, and what the runtime codec says the answers are.
  std::mt19937_64 rng(seed);
  std::vector<std::string> want_bytes;
  std::vector<std::vector<Value>> inputs;
  {
    std::ofstream in(dir / "input.txt");
    for (std::size_t i = 0; i < messages.size(); ++i) {
      const Generated& g = messages[i];
      const auto& signals = g.message->signals();
      for (int n = 0; n < kCasesPerMessage; ++n) {
        std::vector<std::uint8_t> data(g.message->dlc(), 0u);
        std::vector<Value> values;
        in << i;
        for (std::size_t s = 0; s < signals.size(); ++s) {
          const Value v = pick(rng, signals[s].layout(), g.types[s]);
          encode_expected(signals[s].layout(), g.types[s], v, data.data(), raw_mode);
          in << ' ' << format_value(g.types[s], v);
          values.push_back(v);
        }
        in << '\n';
        want_bytes.push_back(hex(data.data(), data.size()));
        inputs.push_back(std::move(values));
      }
    }
  }

  ASSERT_EQ(
      run("'" + (dir / "harness").string() + "' < '" + (dir / "input.txt").string() +
          "' > '" + (dir / "output.txt").string() + "'"),
      0);

  std::ifstream out(dir / "output.txt");
  std::string line;
  std::size_t row = 0;
  int failures = 0;
  for (std::size_t i = 0; i < messages.size() && failures < 10; ++i) {
    const Generated& g = messages[i];
    const auto& signals = g.message->signals();
    for (int n = 0; n < kCasesPerMessage && failures < 10; ++n, ++row) {
      ASSERT_TRUE(static_cast<bool>(std::getline(out, line)))
          << "the harness stopped after " << row << " rows";
      std::istringstream fields(line);
      std::string got_bytes;
      fields >> got_bytes;

      std::string given;
      for (std::size_t s = 0; s < signals.size(); ++s) {
        given +=
            " " + signals[s].name() + "=" + format_value(g.types[s], inputs[row][s]);
      }
      if (got_bytes != want_bytes[row]) {
        ++failures;
        ADD_FAILURE() << g.message->name() << " packed wrong.\n  given:" << given
                      << "\n  generated C:   " << got_bytes
                      << "\n  Signal::encode: " << want_bytes[row];
        continue;
      }

      // Unpack: decode the expected bytes with the runtime codec and compare.
      std::vector<std::uint8_t> data(g.message->dlc(), 0u);
      for (std::size_t b = 0; b < data.size(); ++b) {
        data[b] = static_cast<std::uint8_t>(
            std::stoul(want_bytes[row].substr(b * 2u, 2u), nullptr, 16));
      }
      for (std::size_t s = 0; s < signals.size(); ++s) {
        const core::SignalLayout& l = signals[s].layout();
        std::string token;
        fields >> token;
        const std::uint64_t raw = l.decode_raw(data.data());
        bool same = false;
        if (is_float(g.types[s])) {
          const double want = l.raw_to_physical(raw);
          const double got = std::strtod(token.c_str(), nullptr);
          // Rounding of the field's own type only: a few units in the last
          // place of a float or a double. Half a raw step would be allowed;
          // this is orders of magnitude tighter and must hold for every case.
          const double ulps = g.types[s] == cg::CType::F32 ? 4e-7 : 4e-15;
          const double tolerance =
              (std::fabs(want - l.offset) + std::fabs(l.offset)) * ulps + 1e-12;
          EXPECT_LT(tolerance, 0.5 * l.resolution());
          same = std::fabs(got - want) <= tolerance;
        } else if (is_signed(g.types[s])) {
          same = std::strtoll(token.c_str(), nullptr, 10) ==
                 core::sign_extend(raw, l.bit_length);
        } else {
          same = std::strtoull(token.c_str(), nullptr, 10) == raw;
        }
        if (!same) {
          ++failures;
          ADD_FAILURE() << g.message->name() << "." << signals[s].name()
                        << " unpacked wrong from " << want_bytes[row] << ": got "
                        << token << ", raw is " << raw;
        }
      }
    }
  }
  EXPECT_EQ(failures, 0);
  if (failures == 0 && !::testing::Test::HasFailure()) {
    fs::remove_all(dir);  // kept on failure, for a look at the generated code
  }
}

TEST(CGenProof, PowertrainMatchesTheRuntimeCodec) {
  prove(CANFORGE_DBC_TEST_DATA "/powertrain.dbc", "pt", "ECM", 20261113u);
}

TEST(CGenProof, MotorolaAndMixedLayoutsMatchTheRuntimeCodec) {
  prove(CANFORGE_CODEGEN_TEST_DATA "/motorola.dbc", "moto", "B", 4711u);
}

// --raw: every field an integer over its type's whole range, which covers each
// signal's full bit range, values outside [min|max] included.
TEST(CGenProof, RawPowertrainMatchesTheRuntimeRawCodec) {
  prove(CANFORGE_DBC_TEST_DATA "/powertrain.dbc", "ptraw", "ECM", 20261115u, true);
}

TEST(CGenProof, RawMotorolaMatchesTheRuntimeRawCodec) {
  prove(CANFORGE_CODEGEN_TEST_DATA "/motorola.dbc", "motoraw", "B", 815u, true);
}

}  // namespace
