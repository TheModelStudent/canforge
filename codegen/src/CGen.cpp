// SPDX-License-Identifier: MIT
#include "canforge/codegen/CGen.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <set>
#include <system_error>

namespace canforge::codegen {
namespace {

using core::ByteOrder;
using core::SignalLayout;
using core::Signedness;

// A float has 24 bits of mantissa. Packing does three rounded operations on a
// value (the conversion to float, the offset, the division), each off by up to
// half a unit in the last place. Keeping |physical / factor| below 2^17 keeps
// the accumulated error under a hundredth of one raw step, so a float field
// always rounds to the raw value a double would give. Above that, use double.
constexpr double kFloatMagnitudeLimit = 131072.0;

// Character classes

constexpr bool is_upper(char c) noexcept {
  return c >= 'A' && c <= 'Z';
}
constexpr bool is_lower(char c) noexcept {
  return c >= 'a' && c <= 'z';
}
constexpr bool is_digit(char c) noexcept {
  return c >= '0' && c <= '9';
}
constexpr bool is_alnum(char c) noexcept {
  return is_upper(c) || is_lower(c) || is_digit(c);
}
constexpr char to_lower(char c) noexcept {
  return is_upper(c) ? static_cast<char>(c - 'A' + 'a') : c;
}
constexpr char to_upper(char c) noexcept {
  return is_lower(c) ? static_cast<char>(c - 'a' + 'A') : c;
}

std::string upper(std::string s) {
  for (char& c : s) {
    c = to_upper(c);
  }
  return s;
}

bool is_c_keyword(std::string_view s) {
  static const char* const kKeywords[] = {
      "auto",    "break",  "case",     "char",   "const",    "continue", "default",
      "do",      "double", "else",     "enum",   "extern",   "float",    "for",
      "goto",    "if",     "inline",   "int",    "long",     "register", "restrict",
      "return",  "short",  "signed",   "sizeof", "static",   "struct",   "switch",
      "typedef", "union",  "unsigned", "void",   "volatile", "while",    "bool",
      "true",    "false"};
  for (const char* k : kKeywords) {
    if (s == k) {
      return true;
    }
  }
  return false;
}

bool is_identifier(std::string_view s) {
  if (s.empty() || is_digit(s.front())) {
    return false;
  }
  for (char c : s) {
    if (!is_alnum(c) && c != '_') {
      return false;
    }
  }
  return true;
}

// Literals

std::string hex_digits(std::uint64_t v) {
  static const char kDigits[] = "0123456789ABCDEF";
  std::string digits;
  do {
    digits.insert(digits.begin(), kDigits[v & 0xFu]);
    v >>= 4u;
  } while (v != 0u);
  return "0x" + digits;
}

std::string hex_literal(std::uint64_t v) {
  return hex_digits(v) + "u";
}

std::string unsigned_literal(std::uint64_t v, bool wide) {
  return std::to_string(v) + (wide ? "ull" : "u");
}

std::string signed_literal(std::int64_t v, bool wide) {
  // The most negative value has no literal of its own in C: -2147483648 is the
  // unary minus applied to a constant that does not fit an int.
  if (wide) {
    if (v == std::numeric_limits<std::int64_t>::min()) {
      return "(-9223372036854775807ll - 1)";
    }
    return v < 0 ? "(" + std::to_string(v) + "ll)" : std::to_string(v) + "ll";
  }
  if (v == std::numeric_limits<std::int32_t>::min()) {
    return "(-2147483647 - 1)";
  }
  return v < 0 ? "(" + std::to_string(v) + ")" : std::to_string(v);
}

/// Shortest decimal that reads back as exactly the same float or double.
/// Plain notation is preferred over an exponent where both are exact: `-40.0f`
/// reads better than `-4e+01f`, which is what the shortest %g form would be.
std::string real_literal(double value, bool is_double) {
  const float as_float = static_cast<float>(value);
  const double target = is_double ? value : static_cast<double>(as_float);
  const int max_precision = is_double ? 17 : 9;
  const auto exact = [&](const char* text) {
    return is_double ? std::strtod(text, nullptr) == value
                     : std::strtof(text, nullptr) == as_float;
  };

  char buf[64] = {};
  std::string s;
  for (int pass = 0; pass < 2 && s.empty(); ++pass) {
    for (int precision = 1; precision <= max_precision; ++precision) {
      std::snprintf(buf, sizeof(buf), "%.*g", precision, target);
      const bool has_exponent =
          std::string_view(buf).find('e') != std::string_view::npos;
      if (exact(buf) && (pass == 1 || !has_exponent)) {
        s = buf;
        break;
      }
    }
  }
  if (s.empty()) {
    s = buf;  // max_precision digits always round-trip; not reached in practice
  }
  if (s.find_first_of(".e") == std::string::npos) {
    s += ".0";
  }
  if (!is_double) {
    s += "f";
  }
  return value < 0.0 ? "(" + s + ")" : s;
}

std::string compact(double v) {
  char buf[64] = {};
  std::snprintf(buf, sizeof(buf), "%g", v);
  return buf;
}

// Signal analysis

constexpr bool is_float(CType t) noexcept {
  return t == CType::F32 || t == CType::F64;
}

bool is_whole(double v) noexcept {
  return std::isfinite(v) && std::floor(v) == v;
}

bool is_scaled(const SignalLayout& l) noexcept {
  return !(l.factor == 1.0 && l.offset == 0.0 && is_whole(l.minimum) &&
           is_whole(l.maximum));
}

std::uint64_t unsigned_type_max(CType t) noexcept {
  switch (t) {
    case CType::U8:
      return 0xFFu;
    case CType::U16:
      return 0xFFFFu;
    case CType::U32:
      return 0xFFFFFFFFu;
    default:
      return ~std::uint64_t{0};
  }
}

std::int64_t signed_type_max(CType t) noexcept {
  switch (t) {
    case CType::I8:
      return 0x7F;
    case CType::I16:
      return 0x7FFF;
    case CType::I32:
      return 0x7FFFFFFF;
    default:
      return std::numeric_limits<std::int64_t>::max();
  }
}

std::int64_t signed_type_min(CType t) noexcept {
  return -signed_type_max(t) - 1;
}

/// Raw values a signal may carry: its declared [min|max] if it has one,
/// otherwise everything its bits can hold. Computed with the runtime codec, so
/// an off-grid limit rounds here exactly as it does in Signal::encode.
struct RawRange {
  std::uint64_t ulo = 0;
  std::uint64_t uhi = 0;
  std::int64_t slo = 0;
  std::int64_t shi = 0;
};

RawRange raw_range(const SignalLayout& l) noexcept {
  RawRange r;
  const std::uint64_t mask = core::bit_mask(l.bit_length);
  if (l.signedness == Signedness::Signed) {
    r.shi = static_cast<std::int64_t>(mask >> 1u);
    r.slo = -r.shi - 1;
    if (l.has_range()) {
      const std::int64_t a =
          core::sign_extend(l.physical_to_raw(l.minimum), l.bit_length);
      const std::int64_t b =
          core::sign_extend(l.physical_to_raw(l.maximum), l.bit_length);
      r.slo = std::min(a, b);
      r.shi = std::max(a, b);
    }
  } else {
    r.uhi = mask;
    if (l.has_range()) {
      const std::uint64_t a = l.physical_to_raw(l.minimum);
      const std::uint64_t b = l.physical_to_raw(l.maximum);
      r.ulo = std::min(a, b);
      r.uhi = std::max(a, b);
    }
  }
  return r;
}

/// One byte's share of a signal: bits [pos, pos + nbits) of data[byte] hold
/// bits [shift, shift + nbits) of the raw value.
struct ByteSlice {
  std::size_t byte = 0;
  std::size_t pos = 0;
  std::size_t nbits = 0;
  std::size_t shift = 0;
};

std::vector<ByteSlice> byte_slices(const SignalLayout& l) {
  // In canonical bit space (see core/src/Signal.cpp) both byte orders are a
  // contiguous run [q0, q1). They differ only in which end of the run is the
  // least significant bit and in how a position maps to a bit within its byte.
  const std::size_t q0 = l.canonical_start();
  const std::size_t len = l.bit_length;
  const std::size_t q1 = q0 + len;
  std::vector<ByteSlice> out;
  for (std::size_t b = q0 / 8u; b <= (q1 - 1u) / 8u; ++b) {
    const std::size_t lo = std::max(q0, b * 8u);
    const std::size_t hi = std::min(q1, b * 8u + 8u);
    ByteSlice s;
    s.byte = b;
    s.nbits = hi - lo;
    if (l.byte_order == ByteOrder::Intel) {
      s.pos = lo - b * 8u;  // the run ascends from the signal's LSB
      s.shift = lo - q0;
    } else {
      s.pos = b * 8u + 8u - hi;  // the run ascends from the signal's MSB
      s.shift = len - (hi - q0);
    }
    out.push_back(s);
  }
  return out;
}

// Plans: everything decided before a line of C is written

struct FieldPlan {
  const core::Signal* signal = nullptr;
  std::string name;        ///< struct field, snake_case
  std::string macro_stem;  ///< MESSAGE_SIGNAL
  CType type = CType::U8;
  bool scaled = false;
  bool raw_signed = false;
  bool wide = false;  ///< the raw value needs 64 bits
  bool clamp = true;  ///< pack clamps to the declared range (not with --raw)
  RawRange range;
};

struct MessagePlan {
  const core::Message* message = nullptr;
  std::string snake;
  std::string macro;
  std::uint32_t cycle_ms = 0;
  bool has_struct = false;
  std::vector<FieldPlan> fields;
};

std::uint32_t cycle_time_ms(const core::Database& db, const core::Message& m) {
  static const char kAttribute[] = "GenMsgCycleTime";
  const auto it = m.attributes().find(kAttribute);
  std::int64_t value = 0;
  if (it != m.attributes().end()) {
    value = it->second.as_int();
  } else if (const auto* def = db.find_attribute_definition(kAttribute);
             def != nullptr && def->has_default) {
    value = def->default_value.as_int();
  }
  return value > 0 ? static_cast<std::uint32_t>(value) : 0u;
}

bool sent_by(const core::Message& m, std::string_view node) {
  if (m.transmitter() == node) {
    return true;
  }
  const auto& extra = m.additional_transmitters();
  return std::find(extra.begin(), extra.end(), node) != extra.end();
}

bool received_by(const core::Message& m, std::string_view node) {
  for (const core::Signal& s : m.signals()) {
    const auto& rx = s.receivers();
    if (std::find(rx.begin(), rx.end(), node) != rx.end()) {
      return true;
    }
  }
  return false;
}

std::string comment_safe(std::string text) {
  // Database text is copied into C comments; it must not be able to end one.
  std::size_t at = 0;
  while ((at = text.find("*/", at)) != std::string::npos) {
    text.replace(at, 2u, "* /");
  }
  std::replace(text.begin(), text.end(), '\n', ' ');
  std::replace(text.begin(), text.end(), '\r', ' ');
  return text;
}

/// Free text from a VAL_ entry as a macro suffix: `Not available` -> `NOT_AVAILABLE`.
std::string macro_suffix(std::string_view text) {
  std::string out;
  for (char c : text) {
    if (is_alnum(c)) {
      out += to_upper(c);
    } else if (!out.empty() && out.back() != '_') {
      out += '_';
    }
  }
  while (!out.empty() && out.back() == '_') {
    out.pop_back();
  }
  return out;
}

std::string signal_summary(const core::Signal& s) {
  const SignalLayout& l = s.layout();
  std::string text = std::to_string(l.start_bit) + "|" + std::to_string(l.bit_length) +
                     "@" + (l.byte_order == ByteOrder::Intel ? "1" : "0") +
                     (l.signedness == Signedness::Signed ? "-" : "+") + " (" +
                     compact(l.factor) + "," + compact(l.offset) + ") [" +
                     compact(l.minimum) + "|" + compact(l.maximum) + "]";
  if (!s.unit().empty()) {
    text += " " + s.unit();
  }
  return comment_safe(text);
}

// Emission

struct Helpers {
  bool round_u32 = false;
  bool round_s32 = false;
  bool round_u32_f64 = false;
  bool round_s32_f64 = false;
};

const char* raw_type(const FieldPlan& f) {
  return f.wide ? "uint64_t" : "uint32_t";
}

const char* value_type(const FieldPlan& f) {
  return f.wide ? "int64_t" : "int32_t";
}

std::string data_param(const core::Message& m) {
  return "data[" + std::to_string(unsigned{m.dlc()}) + "]";
}

std::string min_literal(const FieldPlan& f) {
  const SignalLayout& l = f.signal->layout();
  if (is_float(f.type)) {
    const double v =
        l.has_range() ? std::min(l.minimum, l.maximum) : l.physical_floor();
    return real_literal(v, f.type == CType::F64);
  }
  return f.raw_signed ? signed_literal(f.range.slo, f.wide)
                      : unsigned_literal(f.range.ulo, f.wide);
}

std::string max_literal(const FieldPlan& f) {
  const SignalLayout& l = f.signal->layout();
  if (is_float(f.type)) {
    const double v =
        l.has_range() ? std::max(l.minimum, l.maximum) : l.physical_ceiling();
    return real_literal(v, f.type == CType::F64);
  }
  return f.raw_signed ? signed_literal(f.range.shi, f.wide)
                      : unsigned_literal(f.range.uhi, f.wide);
}

void emit_message_header(std::string& h, const MessagePlan& p, bool raw) {
  const core::Message& m = *p.message;
  const std::string bytes = std::to_string(unsigned{m.dlc()});

  h += "/* " + comment_safe(m.name()) + ": " + hex_digits(m.id().value()) + ", " +
       bytes + " bytes";
  if (!m.transmitter().empty() && m.transmitter() != core::kNoNode) {
    h += ", sent by " + comment_safe(m.transmitter());
  }
  if (p.cycle_ms != 0u) {
    h += " every " + std::to_string(p.cycle_ms) + " ms";
  }
  h += ".";
  if (!m.comment().empty()) {
    h += "\n   " + comment_safe(m.comment());
  }
  h += " */\n";
  h += "#define " + p.macro + "_ID " + hex_literal(m.id().value()) + "\n";
  h += "#define " + p.macro + "_IS_EXTENDED " + (m.id().is_extended() ? "1" : "0") +
       "\n";
  h += "#define " + p.macro + "_DLC " + bytes + "u\n";
  h += "#define " + p.macro + "_CYCLE_MS " + std::to_string(p.cycle_ms) + "u\n";

  if (!p.has_struct) {
    h += "\n";
    return;
  }

  h += "\n";
  std::set<std::string> defined;
  for (const FieldPlan& f : p.fields) {
    h += "#define " + f.macro_stem + "_MIN " + min_literal(f) + "\n";
    h += "#define " + f.macro_stem + "_MAX " + max_literal(f) + "\n";
    defined.insert(f.macro_stem + "_MIN");
    defined.insert(f.macro_stem + "_MAX");
  }

  // Named values (VAL_), for unscaled signals only: a name for a raw value means
  // nothing next to a float field. They get a _VAL_ infix of their own, because
  // real databases do name values "Min" and "Max", and those would collide with
  // the limit macros above.
  bool any_value = false;
  for (const FieldPlan& f : p.fields) {
    if (is_float(f.type)) {
      continue;
    }
    for (const core::ValueDescription& v : f.signal->value_descriptions()) {
      const std::string suffix = macro_suffix(v.text);
      const std::string name = f.macro_stem + "_VAL_" + suffix;
      if (suffix.empty() || !defined.insert(name).second) {
        continue;
      }
      if (!any_value) {
        h += "\n";
        any_value = true;
      }
      h += "#define " + name + " " +
           (f.raw_signed
                ? signed_literal(v.value, f.wide)
                : unsigned_literal(static_cast<std::uint64_t>(v.value), f.wide)) +
           "\n";
    }
  }

  h += "\ntypedef struct {\n";
  for (const FieldPlan& f : p.fields) {
    h += std::string("    ") + to_string(f.type) + " " + f.name + "; /* " +
         signal_summary(*f.signal) + " */\n";
  }
  h += "} " + p.snake + "_t;\n\n";

  if (raw) {
    h += "/* Writes the " + bytes +
         " payload bytes. Fields are raw values, each cut to its\n"
         "   signal's bits and not clamped to the limits above. */\n";
  } else {
    h += "/* Writes the " + bytes +
         " payload bytes. A value outside its signal's range is\n"
         "   clamped to the nearest limit; a float is rounded to the nearest raw step. "
         "*/\n";
  }
  h += "void " + p.snake + "_pack(const " + p.snake + "_t *m, uint8_t " +
       data_param(m) + ");\n";
  h += "/* Reads the " + bytes + " payload bytes into *m. */\n";
  h += "void " + p.snake + "_unpack(" + p.snake + "_t *m, const uint8_t " +
       data_param(m) + ");\n\n";
}

std::string pack_term(const ByteSlice& s) {
  std::string term = "raw";
  if (s.shift != 0u) {
    term = "(raw >> " + std::to_string(s.shift) + ")";
  }
  term = "(" + term + " & " +
         hex_literal(core::bit_mask(static_cast<std::uint8_t>(s.nbits))) + ")";
  if (s.pos != 0u) {
    term = "(" + term + " << " + std::to_string(s.pos) + ")";
  }
  return term;
}

std::string unpack_term(const ByteSlice& s, const char* type) {
  std::string byte = "data[" + std::to_string(s.byte) + "]";
  if (s.pos != 0u) {
    byte = "(" + byte + " >> " + std::to_string(s.pos) + ")";
  }
  std::string term = std::string("(") + type + ")(" + byte + " & " +
                     hex_literal(core::bit_mask(static_cast<std::uint8_t>(s.nbits))) +
                     ")";
  if (s.shift != 0u) {
    term = "(" + term + " << " + std::to_string(s.shift) + ")";
  }
  return term;
}

void emit_clamp(std::string& c, const std::string& var, const std::string& op,
                const std::string& limit) {
  c += "        if (" + var + " " + op + " " + limit + ") {\n";
  c += "            " + var + " = " + limit + ";\n";
  c += "        }\n";
}

void emit_pack_signal(std::string& c, const FieldPlan& f, const std::string& prefix,
                      Helpers& helpers) {
  const SignalLayout& l = f.signal->layout();
  c += "    /* " + comment_safe(f.signal->name()) + ": " + signal_summary(*f.signal) +
       " */\n";
  c += "    {\n";

  if (f.scaled) {
    const bool dbl = f.type == CType::F64;
    std::string expr = "m->" + f.name;
    if (l.offset != 0.0) {
      expr = "(" + expr + " - " + real_literal(l.offset, dbl) + ")";
    }
    if (l.factor != 1.0) {
      expr += " / " + real_literal(l.factor, dbl);
    }
    std::string helper = prefix + (f.raw_signed ? "_round_s32" : "_round_u32");
    if (dbl) {
      helper += "_f64";
    }
    if (f.raw_signed) {
      (dbl ? helpers.round_s32_f64 : helpers.round_s32) = true;
      c += "        const uint32_t raw = (uint32_t)" + helper + "(" + expr + ", " +
           real_literal(static_cast<double>(f.range.slo), dbl) + ", " +
           real_literal(static_cast<double>(f.range.shi), dbl) + ");\n";
    } else {
      (dbl ? helpers.round_u32_f64 : helpers.round_u32) = true;
      c += "        const uint32_t raw = " + helper + "(" + expr + ", " +
           real_literal(static_cast<double>(f.range.ulo), dbl) + ", " +
           real_literal(static_cast<double>(f.range.uhi), dbl) + ");\n";
    }
  } else if (f.raw_signed) {
    const bool clamp_hi = f.clamp && f.range.shi < signed_type_max(f.type);
    const bool clamp_lo = f.clamp && f.range.slo > signed_type_min(f.type);
    c += std::string("        ") + ((clamp_hi || clamp_lo) ? "" : "const ") +
         value_type(f) + " value = m->" + f.name + ";\n";
    if (clamp_hi) {
      emit_clamp(c, "value", ">", signed_literal(f.range.shi, f.wide));
    }
    if (clamp_lo) {
      emit_clamp(c, "value", "<", signed_literal(f.range.slo, f.wide));
    }
    c += std::string("        const ") + raw_type(f) + " raw = (" + raw_type(f) +
         ")value;\n";
  } else {
    const bool clamp_hi = f.clamp && f.range.uhi < unsigned_type_max(f.type);
    const bool clamp_lo = f.clamp && f.range.ulo > 0u;
    c += std::string("        ") + ((clamp_hi || clamp_lo) ? "" : "const ") +
         raw_type(f) + " raw = m->" + f.name + ";\n";
    if (clamp_hi) {
      emit_clamp(c, "raw", ">", unsigned_literal(f.range.uhi, f.wide));
    }
    if (clamp_lo) {
      emit_clamp(c, "raw", "<", unsigned_literal(f.range.ulo, f.wide));
    }
  }

  for (const ByteSlice& s : byte_slices(l)) {
    const std::string byte = "data[" + std::to_string(s.byte) + "]";
    c += "        " + byte + " = (uint8_t)(" + byte + " | (uint8_t)" + pack_term(s) +
         ");\n";
  }
  c += "    }\n";
}

void emit_unpack_signal(std::string& c, const FieldPlan& f) {
  const SignalLayout& l = f.signal->layout();
  c += "    /* " + comment_safe(f.signal->name()) + " */\n";
  c += "    {\n";

  const std::vector<ByteSlice> slices = byte_slices(l);
  c += std::string("        const ") + raw_type(f) + " raw =";
  for (std::size_t i = 0; i < slices.size(); ++i) {
    c += (slices.size() == 1u) ? " " : "\n            ";
    c += unpack_term(slices[i], raw_type(f));
    c += (i + 1u < slices.size()) ? " |" : ";\n";
  }

  if (f.raw_signed) {
    // Sign extension without relying on how a compiler shifts negative numbers:
    // flipping the sign bit turns the raw value into an offset-binary number in
    // [0, 2^len), and subtracting 2^(len-1) moves it to [-2^(len-1), 2^(len-1)).
    const std::size_t width = f.wide ? 64u : 32u;
    if (l.bit_length < width) {
      const std::uint64_t sign = std::uint64_t{1} << (l.bit_length - 1u);
      c += std::string("        const ") + value_type(f) + " value = (" +
           value_type(f) + ")(raw ^ " + hex_literal(sign) + ") - " +
           signed_literal(static_cast<std::int64_t>(sign), f.wide) + ";\n";
    } else {
      c += std::string("        const ") + value_type(f) + " value = (" +
           value_type(f) + ")raw;\n";
    }
  }

  const std::string number = f.raw_signed ? "value" : "raw";
  if (f.scaled) {
    const bool dbl = f.type == CType::F64;
    std::string expr = std::string("(") + (dbl ? "double" : "float") + ")" + number;
    if (l.factor != 1.0) {
      expr = "(" + expr + " * " + real_literal(l.factor, dbl) + ")";
    }
    if (l.offset != 0.0) {
      expr += " + " + real_literal(l.offset, dbl);
    }
    c += "        m->" + f.name + " = " + expr + ";\n";
  } else {
    c += "        m->" + f.name + " = (" + to_string(f.type) + ")" + number + ";\n";
  }
  c += "    }\n";
}

void emit_round_helper(std::string& c, const std::string& prefix, bool is_signed,
                       bool dbl) {
  const std::string real = dbl ? "double" : "float";
  const std::string ret = is_signed ? "int32_t" : "uint32_t";
  const std::string half = dbl ? "0.5" : "0.5f";
  const std::string zero = dbl ? "0.0" : "0.0f";
  const std::string name =
      prefix + (is_signed ? "_round_s32" : "_round_u32") + (dbl ? "_f64" : "");

  c +=
      "/* Nearest integer, halves away from zero, limited to [lo, hi]. The limits are\n"
      "   applied before the conversion, so it can never overflow. NaN gives 0. */\n";
  c += "static " + ret + " " + name + "(" + real + " x, " + real + " lo, " + real +
       " hi)\n{\n";
  c += "    " + ret + " result = 0;\n\n";
  c += "    if (x != x) {\n        result = 0;\n";
  c += "    } else if (x <= lo) {\n        result = (" + ret + ")lo;\n";
  c += "    } else if (x >= hi) {\n        result = (" + ret + ")hi;\n";
  if (is_signed) {
    c += "    } else if (x >= " + zero + ") {\n        result = (" + ret + ")(x + " +
         half + ");\n";
    c += "    } else {\n        result = (" + ret + ")(x - " + half + ");\n";
  } else {
    c += "    } else {\n        result = (" + ret + ")(x + " + half + ");\n";
  }
  c += "    }\n    return result;\n}\n\n";
}

}  // namespace

const char* to_string(CType type) noexcept {
  switch (type) {
    case CType::U8:
      return "uint8_t";
    case CType::U16:
      return "uint16_t";
    case CType::U32:
      return "uint32_t";
    case CType::U64:
      return "uint64_t";
    case CType::I8:
      return "int8_t";
    case CType::I16:
      return "int16_t";
    case CType::I32:
      return "int32_t";
    case CType::I64:
      return "int64_t";
    case CType::F32:
      return "float";
    case CType::F64:
      return "double";
  }
  return "uint8_t";
}

std::string snake_case(std::string_view name) {
  std::string out;
  for (std::size_t i = 0; i < name.size(); ++i) {
    const char c = name[i];
    if (!is_alnum(c)) {
      if (!out.empty() && out.back() != '_') {
        out += '_';
      }
      continue;
    }
    if (is_upper(c) && i > 0u) {
      // A word starts at a capital that follows a lower-case letter or a digit
      // (coolantTemp, Temp1Max), and at the last capital of a run when a
      // lower-case letter follows (VCUCmd -> vcu_cmd).
      const char prev = name[i - 1u];
      const bool next_lower = i + 1u < name.size() && is_lower(name[i + 1u]);
      if (is_lower(prev) || is_digit(prev) || (is_upper(prev) && next_lower)) {
        if (!out.empty() && out.back() != '_') {
          out += '_';
        }
      }
    }
    out += to_lower(c);
  }
  while (!out.empty() && out.back() == '_') {
    out.pop_back();
  }
  if (out.empty() || is_digit(out.front())) {
    out.insert(out.begin(), '_');
  }
  if (is_c_keyword(out)) {
    out += '_';
  }
  return out;
}

CType raw_field_type(const SignalLayout& l) noexcept {
  const bool is_signed = l.signedness == Signedness::Signed;
  if (l.bit_length <= 8u) {
    return is_signed ? CType::I8 : CType::U8;
  }
  if (l.bit_length <= 16u) {
    return is_signed ? CType::I16 : CType::U16;
  }
  if (l.bit_length <= 32u) {
    return is_signed ? CType::I32 : CType::U32;
  }
  return is_signed ? CType::I64 : CType::U64;
}

CType field_type(const SignalLayout& l) noexcept {
  if (!is_scaled(l)) {
    return raw_field_type(l);
  }
  const double magnitude =
      std::max(std::fabs(l.physical_floor()), std::fabs(l.physical_ceiling())) /
      std::fabs(l.factor);
  return magnitude <= kFloatMagnitudeLimit ? CType::F32 : CType::F64;
}

std::string unsupported_reason(const core::Message& m) {
  if (m.is_multiplexed()) {
    return "it has multiplexed signals";
  }
  std::set<std::string> names;
  for (const core::Signal& s : m.signals()) {
    const SignalLayout& l = s.layout();
    if (l.value_type != core::ValueType::Integer) {
      return "signal '" + s.name() + "' is an IEEE float (SIG_VALTYPE_)";
    }
    if (!l.validate(m.dlc())) {
      return "signal '" + s.name() + "' has an invalid layout";
    }
    if (is_scaled(l) && l.bit_length > 32u) {
      return "signal '" + s.name() + "' is scaled and wider than 32 bits";
    }
    if (!names.insert(snake_case(s.name())).second) {
      return "two of its signals become the field '" + snake_case(s.name()) + "'";
    }
  }
  return {};
}

core::Result<CGenOutput> render_c(const core::Database& db, const CGenOptions& opt) {
  if (!is_identifier(opt.prefix)) {
    return core::Error(core::ErrorCode::InvalidArgument,
                       "the prefix must be a C identifier");
  }
  if (!opt.node.empty() && db.find_node(opt.node) == nullptr) {
    return core::Error(core::ErrorCode::InvalidArgument,
                       "the node is not in the database");
  }

  CGenOutput out;
  std::vector<MessagePlan> plans;
  std::set<std::string> message_names;
  for (const core::Message& m : db.messages()) {
    const std::string reason = unsupported_reason(m);
    if (!reason.empty()) {
      out.skipped.push_back(m.name() + ": " + reason);
      continue;
    }
    MessagePlan p;
    p.message = &m;
    p.snake = snake_case(m.name());
    p.macro = upper(p.snake);
    p.cycle_ms = cycle_time_ms(db, m);
    p.has_struct = !m.signals().empty();
    if (!message_names.insert(p.snake).second) {
      return core::Error(core::ErrorCode::InvalidArgument,
                         "two messages would get the same C name");
    }
    for (const core::Signal& s : m.signals()) {
      const SignalLayout& l = s.layout();
      FieldPlan f;
      f.signal = &s;
      f.name = snake_case(s.name());
      f.macro_stem = p.macro + "_" + upper(f.name);
      f.type = opt.raw ? raw_field_type(l) : field_type(l);
      f.scaled = is_float(f.type);
      f.clamp = !opt.raw;
      f.raw_signed = l.signedness == Signedness::Signed;
      f.wide = l.bit_length > 32u;
      f.range = raw_range(l);
      p.fields.push_back(std::move(f));
    }
    plans.push_back(std::move(p));
  }

  const std::string guard = upper(opt.prefix) + "_COM_H";
  const std::string file_h = opt.prefix + "_com.h";

  std::string banner = "/*\n * Generated by canforge gen-c";
  if (!opt.source_name.empty()) {
    banner += " from " + comment_safe(opt.source_name);
  }
  if (!opt.node.empty()) {
    banner += " for node " + comment_safe(opt.node);
  }
  banner += ".\n";
  if (opt.raw) {
    banner +=
        " * Raw integers only (--raw). A field holds the raw value; the physical\n"
        " * value is raw * factor + offset, as each field's comment gives them.\n";
  }
  banner += " * Do not edit: change the database and generate again.\n */\n";

  // Header
  std::string& h = out.header;
  h += banner;
  h += "#ifndef " + guard + "\n#define " + guard + "\n\n";
  h += "#include <stdbool.h>\n#include <stdint.h>\n\n";
  h += "#ifdef __cplusplus\nextern \"C\" {\n#endif\n\n";
  for (const MessagePlan& p : plans) {
    emit_message_header(h, p, opt.raw);
  }

  std::vector<const MessagePlan*> rx;
  std::vector<const MessagePlan*> tx;
  if (!opt.node.empty()) {
    for (const MessagePlan& p : plans) {
      if (sent_by(*p.message, opt.node)) {
        tx.push_back(&p);
      } else if (p.has_struct && received_by(*p.message, opt.node)) {
        rx.push_back(&p);
      }
    }
    const std::string P = upper(opt.prefix);
    h += "/* What node " + comment_safe(opt.node) +
         " receives and sends, in database order. */\n";
    h += "#define " + P + "_RX_COUNT " + std::to_string(rx.size()) + "u\n";
    h += "#define " + P + "_TX_COUNT " + std::to_string(tx.size()) + "u\n\n";
    h += "typedef struct {\n    uint32_t id;\n    uint8_t dlc;\n"
         "    /* msg must point to the struct of this message, e.g. a " +
         (rx.empty() ? std::string("message") : rx.front()->snake + "_t") +
         " */\n"
         "    void (*unpack)(void *msg, const uint8_t *data);\n} " +
         opt.prefix + "_rx_entry_t;\n\n";
    h += "typedef struct {\n    uint32_t id;\n    uint8_t dlc;\n"
         "    uint32_t cycle_ms; /* 0: sent on demand, not cyclically */\n} " +
         opt.prefix + "_tx_entry_t;\n\n";
    if (!rx.empty()) {
      h += "extern const " + opt.prefix + "_rx_entry_t " + opt.prefix + "_rx_table[" +
           P + "_RX_COUNT];\n";
    }
    if (!tx.empty()) {
      h += "extern const " + opt.prefix + "_tx_entry_t " + opt.prefix + "_tx_table[" +
           P + "_TX_COUNT];\n";
    }
    h += "\n";
  }
  h += "#ifdef __cplusplus\n}\n#endif\n\n#endif /* " + guard + " */\n";

  // Source. The bodies come first so that only the rounding helpers that are
  // actually used get emitted: an unused static function is a warning, and the
  // generated code must compile with -Werror.
  Helpers helpers;
  std::string body;
  for (const MessagePlan& p : plans) {
    if (!p.has_struct) {
      continue;
    }
    const core::Message& m = *p.message;
    body += "void " + p.snake + "_pack(const " + p.snake + "_t *m, uint8_t " +
            data_param(m) + ")\n{\n";
    for (unsigned i = 0; i < m.dlc(); ++i) {
      body += "    data[" + std::to_string(i) + "] = 0u;\n";
    }
    body += "\n";
    for (const FieldPlan& f : p.fields) {
      emit_pack_signal(body, f, opt.prefix, helpers);
    }
    body += "}\n\n";

    body += "void " + p.snake + "_unpack(" + p.snake + "_t *m, const uint8_t " +
            data_param(m) + ")\n{\n";
    for (const FieldPlan& f : p.fields) {
      emit_unpack_signal(body, f);
    }
    body += "}\n\n";
  }

  std::string& c = out.source;
  c += banner;
  c += "#include \"" + file_h + "\"\n\n";
  if (helpers.round_u32) {
    emit_round_helper(c, opt.prefix, false, false);
  }
  if (helpers.round_s32) {
    emit_round_helper(c, opt.prefix, true, false);
  }
  if (helpers.round_u32_f64) {
    emit_round_helper(c, opt.prefix, false, true);
  }
  if (helpers.round_s32_f64) {
    emit_round_helper(c, opt.prefix, true, true);
  }
  c += body;

  if (!rx.empty()) {
    for (const MessagePlan* p : rx) {
      c += "static void " + p->snake +
           "_unpack_any(void *msg, const uint8_t *data)\n{\n";
      c += "    " + p->snake + "_unpack((" + p->snake + "_t *)msg, data);\n}\n\n";
    }
    c += "const " + opt.prefix + "_rx_entry_t " + opt.prefix + "_rx_table[" +
         upper(opt.prefix) + "_RX_COUNT] = {\n";
    for (const MessagePlan* p : rx) {
      c += "    {" + p->macro + "_ID, " + p->macro + "_DLC, " + p->snake +
           "_unpack_any},\n";
    }
    c += "};\n\n";
  }
  if (!tx.empty()) {
    c += "const " + opt.prefix + "_tx_entry_t " + opt.prefix + "_tx_table[" +
         upper(opt.prefix) + "_TX_COUNT] = {\n";
    for (const MessagePlan* p : tx) {
      c += "    {" + p->macro + "_ID, " + p->macro + "_DLC, " + p->macro +
           "_CYCLE_MS},\n";
    }
    c += "};\n\n";
  }
  while (c.size() >= 2u && c[c.size() - 1u] == '\n' && c[c.size() - 2u] == '\n') {
    c.pop_back();
  }

  return out;
}

core::Status generate_c(const core::Database& db, const CGenOptions& opt) {
  auto rendered = render_c(db, opt);
  if (!rendered) {
    return rendered.error();
  }

  const std::filesystem::path dir = opt.out_dir.empty() ? "." : opt.out_dir;
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  if (ec) {
    return core::Error(core::ErrorCode::ParseIoError,
                       "cannot create the output directory");
  }

  const auto write = [&dir](const std::string& name, const std::string& text) {
    std::ofstream file(dir / name, std::ios::binary | std::ios::trunc);
    file << text;
    return static_cast<bool>(file.flush());
  };
  if (!write(opt.prefix + "_com.h", rendered.value().header) ||
      !write(opt.prefix + "_com.c", rendered.value().source)) {
    return core::Error(core::ErrorCode::ParseIoError,
                       "cannot write the generated files");
  }
  return core::ok();
}

}  // namespace canforge::codegen
