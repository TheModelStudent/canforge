// SPDX-License-Identifier: MIT
#include "canforge/dbc/Writer.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace canforge::dbc {
namespace {

using core::AttributeDefinition;
using core::AttributeObject;
using core::AttributeType;
using core::AttributeValue;
using core::ByteOrder;
using core::Database;
using core::Message;
using core::MultiplexRole;
using core::MuxRange;
using core::Node;
using core::Signal;
using core::Signedness;
using core::ValueType;

constexpr std::uint32_t kDbcExtendedFlag = 0x80000000u;

std::uint32_t wire_id(core::CanId id) {
  return id.is_extended() ? (id.value() | kDbcExtendedFlag) : id.value();
}

/// A string as DBC spells it: quoted, with embedded quotes and backslashes
/// escaped the way CANdb++ escapes them.
std::string quoted(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 2);
  out.push_back('"');
  for (const char c : s) {
    if (c == '"' || c == '\\') {
      out.push_back('\\');
    }
    out.push_back(c);
  }
  out.push_back('"');
  return out;
}

const char* kDefaultNewSymbols[] = {
    "NS_DESC_",   "CM_",          "BA_DEF_",       "BA_",
    "VAL_",       "CAT_DEF_",     "CAT_",          "FILTER",
    "BA_DEF_DEF_", "EV_DATA_",    "ENVVAR_DATA_",  "SGTYPE_",
    "SGTYPE_VAL_", "BA_DEF_SGTYPE_", "BA_SGTYPE_", "SIG_TYPE_REF_",
    "VAL_TABLE_", "SIG_GROUP_",   "SIG_VALTYPE_",  "SIGTYPE_VALTYPE_",
    "BO_TX_BU_",  "BA_DEF_REL_",  "BA_REL_",       "BA_DEF_DEF_REL_",
    "BU_SG_REL_", "BU_EV_REL_",   "BU_BO_REL_",    "SG_MUL_VAL_"};

}  // namespace

std::string format_number(double v) {
  if (std::isnan(v)) {
    return "0";
  }
  if (std::isinf(v)) {
    return v > 0 ? "1e308" : "-1e308";
  }
  // An exact integer is written without a decimal point, the way every
  // tool does and what keeps a factor of 1 from becoming "1.0000000000000000".
  if (v == std::floor(v) && std::fabs(v) < 1e15) {
    std::ostringstream os;
    os << static_cast<long long>(v);
    return os.str();
  }
  // Otherwise find the shortest precision that reads back bit-identically, so
  // that 0.1 stays "0.1" and not become 0.10000000000000001.
  for (int precision = 6; precision <= 17; ++precision) {
    std::ostringstream os;
    os.precision(precision);
    os << v;
    const std::string candidate = os.str();
    if (std::strtod(candidate.c_str(), nullptr) == v) {
      return candidate;
    }
  }
  std::ostringstream os;
  os.precision(17);
  os << v;
  return os.str();
}

namespace {

std::string attribute_literal(const AttributeValue& value,
                              const AttributeDefinition* def, bool as_default) {
  switch (value.type()) {
    case AttributeType::String:
      return quoted(value.text());
    case AttributeType::Enum:
      // BA_DEF_DEF_ spells an enum value as its name; BA_ spells it as the
      // zero-based index. Getting this backwards is the classic way to write a
      // DBC that CANdb++ refuses to open.
      if (as_default) {
        return quoted(value.text());
      }
      return format_number(value.number());
    case AttributeType::Hex:
    case AttributeType::Int:
      return format_number(std::floor(value.number()));
    case AttributeType::Float:
    default:
      static_cast<void>(def);
      return format_number(value.number());
  }
}

void write_signal(std::ostringstream& os, const Signal& s) {
  const auto& l = s.layout();
  os << " SG_ " << s.name();
  switch (s.multiplex_role()) {
    case MultiplexRole::Multiplexor:
      os << " M";
      break;
    case MultiplexRole::Multiplexed:
      os << " m" << s.multiplex_value();
      break;
    case MultiplexRole::Both:
      os << " m" << s.multiplex_value() << 'M';
      break;
    case MultiplexRole::None:
    default:
      break;
  }
  os << " : " << l.start_bit << '|' << unsigned{l.bit_length} << '@'
     << (l.byte_order == ByteOrder::Intel ? '1' : '0')
     << (l.signedness == Signedness::Signed ? '-' : '+') << " ("
     << format_number(l.factor) << ',' << format_number(l.offset) << ") ["
     << format_number(l.minimum) << '|' << format_number(l.maximum) << "] "
     << quoted(s.unit()) << ' ';
  if (s.receivers().empty()) {
    os << core::kNoNode;
  } else {
    for (std::size_t i = 0; i < s.receivers().size(); ++i) {
      if (i != 0) {
        os << ',';
      }
      os << s.receivers()[i];
    }
  }
  os << '\n';
}

}  // namespace

std::string write_string(const Database& db, const WriteOptions& opt) {
  std::ostringstream os;

  os << "VERSION " << quoted(db.version()) << "\n\n\n";

  os << "NS_ : \n";
  if (!db.new_symbols().empty()) {
    for (const std::string& s : db.new_symbols()) {
      os << '\t' << s << '\n';
    }
  } else if (opt.synthesise_new_symbols) {
    for (const char* s : kDefaultNewSymbols) {
      os << '\t' << s << '\n';
    }
  }
  os << '\n';

  os << "BS_:";
  if (db.bit_timing().has_value()) {
    const auto& bt = *db.bit_timing();
    os << ' ' << bt.baudrate << " : " << bt.btr1 << ',' << bt.btr2;
  }
  os << "\n\n";

  os << "BU_:";
  for (const Node& n : db.nodes()) {
    os << ' ' << n.name;
  }
  os << "\n\n";

  for (const core::ValueTable& t : db.value_tables()) {
    os << "VAL_TABLE_ " << t.name;
    for (const core::ValueDescription& v : t.values) {
      os << ' ' << v.value << ' ' << quoted(v.text);
    }
    os << " ;\n";
  }
  if (!db.value_tables().empty()) {
    os << '\n';
  }

  for (const Message& m : db.messages()) {
    os << "BO_ " << wire_id(m.id()) << ' ' << m.name() << ": "
       << unsigned{m.dlc()} << ' '
       << (m.transmitter().empty() ? std::string(core::kNoNode) : m.transmitter())
       << '\n';
    for (const Signal& s : m.signals()) {
      write_signal(os, s);
    }
    os << '\n';
  }

  for (const Message& m : db.messages()) {
    if (m.additional_transmitters().empty()) {
      continue;
    }
    os << "BO_TX_BU_ " << wire_id(m.id()) << " :";
    for (std::size_t i = 0; i < m.additional_transmitters().size(); ++i) {
      os << (i == 0 ? " " : ",") << m.additional_transmitters()[i];
    }
    os << ";\n";
  }

  if (!db.comment().empty()) {
    os << "CM_ " << quoted(db.comment()) << ";\n";
  }
  for (const Node& n : db.nodes()) {
    if (!n.comment.empty()) {
      os << "CM_ BU_ " << n.name << ' ' << quoted(n.comment) << ";\n";
    }
  }
  for (const Message& m : db.messages()) {
    if (!m.comment().empty()) {
      os << "CM_ BO_ " << wire_id(m.id()) << ' ' << quoted(m.comment()) << ";\n";
    }
    for (const Signal& s : m.signals()) {
      if (!s.comment().empty()) {
        os << "CM_ SG_ " << wire_id(m.id()) << ' ' << s.name() << ' '
           << quoted(s.comment()) << ";\n";
      }
    }
  }

  for (const AttributeDefinition& d : db.attribute_definitions()) {
    os << "BA_DEF_ ";
    switch (d.object) {
      case AttributeObject::Node:    os << "BU_ "; break;
      case AttributeObject::Message: os << "BO_ "; break;
      case AttributeObject::Signal:  os << "SG_ "; break;
      case AttributeObject::EnvVar:  os << "EV_ "; break;
      case AttributeObject::Database:
      default:
        break;
    }
    os << quoted(d.name) << ' ' << core::to_string(d.type);
    switch (d.type) {
      case AttributeType::Int:
      case AttributeType::Hex:
        os << ' ' << format_number(std::floor(d.minimum)) << ' '
           << format_number(std::floor(d.maximum));
        break;
      case AttributeType::Float:
        os << ' ' << format_number(d.minimum) << ' ' << format_number(d.maximum);
        break;
      case AttributeType::Enum:
        for (std::size_t i = 0; i < d.enum_values.size(); ++i) {
          os << (i == 0 ? " " : ",") << quoted(d.enum_values[i]);
        }
        break;
      case AttributeType::String:
      default:
        os << ' ';
        break;
    }
    os << ";\n";
  }
  for (const AttributeDefinition& d : db.attribute_definitions()) {
    if (d.has_default) {
      os << "BA_DEF_DEF_ " << quoted(d.name) << ' '
         << attribute_literal(d.default_value, &d, true) << ";\n";
    }
  }

  const auto emit_values = [&](const core::AttributeMap& map,
                               const std::string& target) {
    for (const auto& [name, value] : map) {
      const AttributeDefinition* def = db.find_attribute_definition(name);
      os << "BA_ " << quoted(name);
      if (!target.empty()) {
        os << ' ' << target;
      }
      os << ' ' << attribute_literal(value, def, false) << ";\n";
    }
  };
  emit_values(db.attributes(), "");
  for (const Node& n : db.nodes()) {
    emit_values(n.attributes, "BU_ " + n.name);
  }
  for (const Message& m : db.messages()) {
    std::ostringstream target;
    target << "BO_ " << wire_id(m.id());
    emit_values(m.attributes(), target.str());
    for (const Signal& s : m.signals()) {
      std::ostringstream sig_target;
      sig_target << "SG_ " << wire_id(m.id()) << ' ' << s.name();
      emit_values(s.attributes(), sig_target.str());
    }
  }

  for (const Message& m : db.messages()) {
    for (const Signal& s : m.signals()) {
      if (s.value_descriptions().empty()) {
        continue;
      }
      os << "VAL_ " << wire_id(m.id()) << ' ' << s.name();
      for (const core::ValueDescription& v : s.value_descriptions()) {
        os << ' ' << v.value << ' ' << quoted(v.text);
      }
      os << " ;\n";
    }
  }

  for (const Message& m : db.messages()) {
    for (const Signal& s : m.signals()) {
      if (s.layout().value_type == ValueType::Integer) {
        continue;
      }
      os << "SIG_VALTYPE_ " << wire_id(m.id()) << ' ' << s.name() << " : "
         << (s.layout().value_type == ValueType::Float32 ? 1 : 2) << ";\n";
    }
  }

  for (const Message& m : db.messages()) {
    for (const Signal& s : m.signals()) {
      if (s.multiplex_ranges().empty()) {
        continue;
      }
      os << "SG_MUL_VAL_ " << wire_id(m.id()) << ' ' << s.name() << ' '
         << s.multiplexor_name();
      for (std::size_t i = 0; i < s.multiplex_ranges().size(); ++i) {
        const MuxRange& r = s.multiplex_ranges()[i];
        os << (i == 0 ? " " : ", ") << r.low << '-' << r.high;
      }
      os << ";\n";
    }
  }

  std::string text = os.str();
  if (opt.crlf) {
    std::string crlf;
    crlf.reserve(text.size() + text.size() / 20u);
    for (const char c : text) {
      if (c == '\n') {
        crlf.push_back('\r');
      }
      crlf.push_back(c);
    }
    return crlf;
  }
  return text;
}

core::Status write_file(const Database& db, const std::string& path,
                        const WriteOptions& opt) {
  std::ofstream out(path, std::ios::binary);
  if (!out) {
    return core::Error(core::ErrorCode::ParseIoError,
                       "cannot open the file for writing");
  }
  const std::string text = write_string(db, opt);
  out.write(text.data(), static_cast<std::streamsize>(text.size()));
  if (!out) {
    return core::Error(core::ErrorCode::ParseIoError, "write failed");
  }
  return core::ok();
}

}  // namespace canforge::dbc
