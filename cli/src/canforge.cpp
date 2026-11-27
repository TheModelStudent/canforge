// SPDX-License-Identifier: MIT
//
// canforge command line entry point.
//
// Argument parsing is hand written: the tool has a handful of flags and a
// dependency for that would be absurd next to a project whose whole point is
// not having any.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "canforge/codegen/CGen.hpp"
#include "canforge/dbc/Parser.hpp"
#include "canforge/sim/Config.hpp"
#include "canforge/sim/Simulator.hpp"
#include "canforge/sim/UdsServer.hpp"
#include "canforge/transport/LogFormat.hpp"
#include "canforge/transport/SocketCanBus.hpp"
#include "canforge/transport/VirtualBus.hpp"
#if CANFORGE_HAVE_TUI
#include "canforge/tui/Dashboard.hpp"
#else
#include "canforge/tui/ViewModel.hpp"
#endif
#include "Flash.hpp"
#include "canforge/uds/BusLink.hpp"
#include "canforge/uds/DidFile.hpp"
#include "canforge/uds/Uds.hpp"

namespace {

using canforge::core::Frame;
using canforge::transport::BusTiming;
using canforge::transport::IBus;

constexpr std::uint64_t kMs = 1000000ULL;

/// One step of a `diag` run. They are carried out in the order they were given.
struct DiagAction {
  enum class Kind : std::uint8_t {
    Session,
    ReadDid,
    WriteDid,
    ReadDtc,
    ClearDtc,
    Unlock,
    Reset,
    Request,
    Wait,
  };
  Kind kind = Kind::Request;
  std::string text;  ///< The option's value, as typed.
};

struct Args {
  std::vector<DiagAction> diag;     ///< diag: what to do, in order
  std::string did_file;             ///< diag: DID descriptions for --read-did
  std::string mask = "FF";          ///< diag: status mask for --read-dtc
  std::string tx_id = "7E0";        ///< diag: identifier requests are sent on
  std::string rx_id = "7E8";        ///< diag: identifier the ECU answers on
  bool keepalive = true;            ///< diag: send TesterPresent while idle
  std::string package;              ///< flash: release package folder
  std::string secret = "A5C3F00D";  ///< flash: seed/key secret
  std::uint32_t p2_ms = 500;        ///< flash: how long to wait for an answer
  bool expect_reject = false;  ///< flash: succeed only if the ECU refuses the image
  bool vcu_standby = false;    ///< flash: play the VCU, mode STANDBY, meanwhile
  std::string config;
  std::string bus = "vcan0";
  std::string database;
  std::string log;
  std::string download;
  std::string node;         ///< gen-c: node the rx/tx tables are built for
  std::string prefix;       ///< gen-c: file and table prefix
  std::string out;          ///< gen-c: output directory
  bool raw = false;         ///< gen-c: raw integers only, no floating point
  double duration_s = 0.0;  ///< 0 means run until interrupted.
  double speed = 1.0;
  bool virtual_bus = false;
  bool quiet = false;
  std::uint32_t bitrate = 500000;
};

[[noreturn]] void usage(int code) {
  std::fprintf(
      stderr,
      "canforge -- a CAN bus toolkit\n"
      "\n"
      "usage: canforge <command> [options]\n"
      "\n"
      "commands:\n"
      "  sim      run the vehicle simulator on a bus\n"
      "  dash     live dashboard: trace, grouped, plot, statistics, transmit\n"
      "  uds      run a diagnostic session against a simulated ECU\n"
      "  diag     talk UDS to an ECU on a bus: read data, read and clear DTCs\n"
      "  dump     decode traffic from a bus or a log file\n"
      "  info     summarise a DBC database\n"
      "  gen-c    generate C pack/unpack code from a DBC database\n"
      "  flash    update an ECU's firmware over UDS: erase, download, check, restart\n"
      "\n"
      "options:\n"
      "  --config <file>    simulator configuration\n"
      "  --bus <name>       SocketCAN interface, default vcan0\n"
      "  --virtual          use the in-process bus; needs no vcan and no root\n"
      "  --database <file>  DBC file (overrides the one in the config)\n"
      "  --duration <s>     stop after this many seconds\n"
      "  --log <file>       write a candump log of everything transmitted\n"
      "  --download <file>  firmware image for `uds`\n"
      "  --bitrate <bps>    nominal bitrate for the timing model\n"
      "  --quiet            suppress the per-frame output\n"
      "  --node <name>      gen-c: node to build the rx/tx tables for\n"
      "  --prefix <name>    gen-c: names the files, <name>_com.h and <name>_com.c\n"
      "  --out <dir>        gen-c: where to write them, default .\n"
      "  --raw              gen-c: raw integer fields only, no floating point\n"
      "\n"
      "diag actions, carried out in the order given:\n"
      "  --session <name>   default, extended or programming\n"
      "  --read-did <hex>   ReadDataByIdentifier, e.g. F190\n"
      "  --write-did <hex>=<bytes>  WriteDataByIdentifier, e.g. 0200=3C\n"
      "  --read-dtc         ReadDTCInformation by status mask\n"
      "  --clear-dtc        ClearDiagnosticInformation, all groups\n"
      "  --unlock           SecurityAccess: request a seed, send the key\n"
      "  --reset            ECUReset, hard reset\n"
      "  --request <bytes>  any request, as hex bytes, e.g. \"3E 00\"\n"
      "  --wait <ms>        pause, keeping the session alive\n"
      "diag options:\n"
      "  --did-file <file>  JSON descriptions used to decode --read-did\n"
      "  --mask <hex>       status mask for --read-dtc, default FF\n"
      "  --tx-id <hex>      request identifier, default 7E0\n"
      "  --rx-id <hex>      response identifier, default 7E8\n"
      "  --no-keepalive     do not send TesterPresent between actions\n"
      "flash options:\n"
      "  --package <dir>    release folder with manifest.json, slotA.bin, slotB.bin\n"
      "  --secret <hex>     seed/key secret, default A5C3F00D\n"
      "  --p2 <ms>          how long the ECU may take to answer, default 500\n"
      "  --expect-reject    succeed only if the ECU refuses the image (for tests)\n"
      "  --vcu-standby      play the VCU meanwhile: 0x100, mode STANDBY (the BTMS\n"
      "                     ECU programs only when parked; stop canforge sim first)\n"
      "\n"
      "examples:\n"
      "  canforge sim --config vehicle.cfg --bus vcan0\n"
      "  canforge dash --config vehicle.cfg --virtual\n"
      "  canforge dash --config vehicle.cfg --bus vcan0\n"
      "  canforge sim --config vehicle.cfg --virtual --duration 10\n"
      "  canforge uds --virtual --download firmware.bin\n"
      "  canforge info --database powertrain.dbc\n"
      "  canforge gen-c --database btms.dbc --node BTMS --prefix btms --out src/com\n"
      "  canforge diag --bus vcan0 --read-did F190 --read-dtc\n"
      "  canforge diag --bus vcan0 --session extended --unlock --write-did 0200=3C\n"
      "  canforge flash --bus vcan0 --package dist/btms-1.1.0\n");
  std::exit(code);
}

bool matches(const char* arg, const char* name) {
  return std::strcmp(arg, name) == 0;
}

Args parse_args(int argc, char** argv) {
  Args args;
  for (int i = 2; i < argc; ++i) {
    const char* a = argv[i];
    const auto next = [&](const char* what) -> const char* {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "canforge: %s needs a value\n", what);
        std::exit(2);
      }
      return argv[++i];
    };
    if (matches(a, "--config")) {
      args.config = next("--config");
    } else if (matches(a, "--bus")) {
      args.bus = next("--bus");
    } else if (matches(a, "--database")) {
      args.database = next("--database");
    } else if (matches(a, "--log")) {
      args.log = next("--log");
    } else if (matches(a, "--download")) {
      args.download = next("--download");
    } else if (matches(a, "--node")) {
      args.node = next("--node");
    } else if (matches(a, "--prefix")) {
      args.prefix = next("--prefix");
    } else if (matches(a, "--out")) {
      args.out = next("--out");
    } else if (matches(a, "--raw")) {
      args.raw = true;
    } else if (matches(a, "--duration")) {
      args.duration_s = std::strtod(next("--duration"), nullptr);
    } else if (matches(a, "--speed")) {
      args.speed = std::strtod(next("--speed"), nullptr);
    } else if (matches(a, "--bitrate")) {
      args.bitrate =
          static_cast<std::uint32_t>(std::strtoul(next("--bitrate"), nullptr, 10));
    } else if (matches(a, "--session")) {
      args.diag.push_back({DiagAction::Kind::Session, next("--session")});
    } else if (matches(a, "--read-did")) {
      args.diag.push_back({DiagAction::Kind::ReadDid, next("--read-did")});
    } else if (matches(a, "--write-did")) {
      args.diag.push_back({DiagAction::Kind::WriteDid, next("--write-did")});
    } else if (matches(a, "--read-dtc")) {
      args.diag.push_back({DiagAction::Kind::ReadDtc, ""});
    } else if (matches(a, "--clear-dtc")) {
      args.diag.push_back({DiagAction::Kind::ClearDtc, ""});
    } else if (matches(a, "--unlock")) {
      args.diag.push_back({DiagAction::Kind::Unlock, ""});
    } else if (matches(a, "--reset")) {
      args.diag.push_back({DiagAction::Kind::Reset, ""});
    } else if (matches(a, "--request")) {
      args.diag.push_back({DiagAction::Kind::Request, next("--request")});
    } else if (matches(a, "--wait")) {
      args.diag.push_back({DiagAction::Kind::Wait, next("--wait")});
    } else if (matches(a, "--did-file")) {
      args.did_file = next("--did-file");
    } else if (matches(a, "--mask")) {
      args.mask = next("--mask");
    } else if (matches(a, "--tx-id")) {
      args.tx_id = next("--tx-id");
    } else if (matches(a, "--rx-id")) {
      args.rx_id = next("--rx-id");
    } else if (matches(a, "--no-keepalive")) {
      args.keepalive = false;
    } else if (matches(a, "--package")) {
      args.package = next("--package");
    } else if (matches(a, "--secret")) {
      args.secret = next("--secret");
    } else if (matches(a, "--p2")) {
      args.p2_ms = static_cast<std::uint32_t>(std::strtoul(next("--p2"), nullptr, 10));
    } else if (matches(a, "--expect-reject")) {
      args.expect_reject = true;
    } else if (matches(a, "--vcu-standby")) {
      args.vcu_standby = true;
    } else if (matches(a, "--virtual")) {
      args.virtual_bus = true;
    } else if (matches(a, "--quiet")) {
      args.quiet = true;
    } else if (matches(a, "-h") || matches(a, "--help")) {
      usage(0);
    } else {
      std::fprintf(stderr, "canforge: unknown option '%s'\n", a);
      usage(2);
    }
  }
  return args;
}

/// Resolve a path mentioned in a config file relative to that config file.
std::string resolve_relative(const std::string& base, const std::string& path) {
  if (path.empty() || path.front() == '/') {
    return path;
  }
  const std::size_t slash = base.find_last_of('/');
  if (slash == std::string::npos) {
    return path;
  }
  return base.substr(0, slash + 1) + path;
}

int load_database(const std::string& path, canforge::core::Database& out) {
  auto parsed = canforge::dbc::parse_file(path);
  if (!parsed) {
    std::fprintf(stderr, "canforge: cannot read %s: %.*s\n", path.c_str(),
                 static_cast<int>(parsed.error().message().size()),
                 parsed.error().message().data());
    return 1;
  }
  const auto& result = parsed.value();
  if (result.diagnostics.warning_count() != 0 || result.diagnostics.has_errors()) {
    std::fputs(result.diagnostics.format(path).c_str(), stderr);
  }
  if (result.diagnostics.has_errors()) {
    return 1;
  }
  out = result.database;
  return 0;
}

int command_sim(const Args& args) {
  if (args.config.empty()) {
    std::fprintf(stderr, "canforge sim: --config is required\n");
    return 2;
  }
  auto parsed = canforge::sim::parse_config_file(args.config);
  if (!parsed) {
    std::fprintf(stderr, "canforge: cannot read %s\n", args.config.c_str());
    return 1;
  }
  const auto& config_result = parsed.value();
  if (!config_result.diagnostics.items().empty()) {
    std::fputs(config_result.diagnostics.format(args.config).c_str(), stderr);
  }
  if (config_result.diagnostics.has_errors()) {
    return 1;
  }

  std::string database_path = args.database;
  if (database_path.empty()) {
    database_path = resolve_relative(args.config, config_result.config.database_path);
  }
  if (database_path.empty()) {
    std::fprintf(stderr, "canforge sim: no database given\n");
    return 2;
  }
  canforge::core::Database database;
  if (load_database(database_path, database) != 0) {
    return 1;
  }

  auto simulator = canforge::sim::Simulator::build(config_result.config, database);
  if (!simulator) {
    std::fprintf(stderr, "canforge sim: %.*s\n",
                 static_cast<int>(simulator.error().message().size()),
                 simulator.error().message().data());
    return 1;
  }

  const BusTiming timing{config_result.config.nominal_bitrate,
                         config_result.config.data_bitrate};

  // Two very different clocks behind one loop: the virtual bus advances its
  // own time as fast as the CPU allows, while SocketCAN runs against the wall.
  std::shared_ptr<canforge::transport::VirtualMedium> medium;
  std::unique_ptr<IBus> bus;
  if (args.virtual_bus) {
    medium = canforge::transport::VirtualMedium::create(timing);
    bus = medium->attach("canforge-sim");
  } else {
    canforge::transport::SocketCanOptions options;
    options.timing = timing;
    bus = std::make_unique<canforge::transport::SocketCanBus>(args.bus, options);
  }
  if (const auto opened = bus->open(); !opened) {
    std::fprintf(stderr, "canforge: cannot open %s: %.*s\n",
                 args.virtual_bus ? "the virtual bus" : args.bus.c_str(),
                 static_cast<int>(opened.error().message().size()),
                 opened.error().message().data());
    return 1;
  }

  std::unique_ptr<canforge::transport::LogWriter> log;
  if (!args.log.empty()) {
    auto writer = canforge::transport::open_writer(
        args.log, canforge::transport::LogFormat::Candump);
    if (!writer) {
      std::fprintf(stderr, "canforge: cannot write %s\n", args.log.c_str());
      return 1;
    }
    log = std::move(writer).value();
  }

  std::fprintf(stderr, "canforge sim: %zu nodes on %s at %u bit/s%s\n",
               simulator.value()->nodes().size(),
               args.virtual_bus ? "the virtual bus" : args.bus.c_str(),
               timing.nominal_bitrate,
               args.duration_s > 0.0 ? "" : " (ctrl-c to stop)");

  const std::uint64_t limit_ns = args.duration_s > 0.0
                                     ? static_cast<std::uint64_t>(args.duration_s * 1e9)
                                     : ~std::uint64_t{0};
  const auto started = std::chrono::steady_clock::now();
  std::uint64_t now_ns = 0;
  std::uint64_t sent = 0;

  while (now_ns < limit_ns) {
    for (const Frame& frame : simulator.value()->step(now_ns)) {
      if (const auto st = bus->send(frame); !st) {
        std::fprintf(stderr, "canforge: send failed: %.*s\n",
                     static_cast<int>(st.error().message().size()),
                     st.error().message().data());
      } else {
        ++sent;
      }
      if (log) {
        canforge::transport::LogRecord record;
        record.frame = frame.widen<64>();
        record.channel = args.bus;
        static_cast<void>(log->write(record));
      }
      if (!args.quiet) {
        const auto* message = database.find_message(frame.id());
        std::printf("%10.6f  %03X  [%zu] %s\n", static_cast<double>(now_ns) / 1e9,
                    frame.id().value(), frame.size(),
                    message != nullptr ? message->name().c_str() : "?");
      }
    }

    if (args.virtual_bus) {
      medium->advance(std::chrono::milliseconds(1));
      now_ns += kMs;
    } else {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
      now_ns = static_cast<std::uint64_t>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(
              std::chrono::steady_clock::now() - started)
              .count());
    }
  }

  if (log) {
    static_cast<void>(log->finish());
  }
  std::fprintf(stderr, "canforge sim: %llu frames in %.1f s, bus load %.1f%%\n",
               static_cast<unsigned long long>(sent), static_cast<double>(now_ns) / 1e9,
               bus->bus_load() * 100.0);
  return 0;
}

int command_uds(const Args& args) {
  using canforge::isotp::Address;
  using canforge::uds::Client;
  using canforge::uds::ClientConfig;
  using canforge::uds::DtcReportType;
  using canforge::uds::RoutineControlType;
  using canforge::uds::SessionType;

  if (!args.virtual_bus) {
    std::fprintf(stderr,
                 "canforge uds: only --virtual is implemented; a real ECU "
                 "needs the SocketCAN pump, which is not wired up yet\n");
    return 2;
  }

  canforge::isotp::Config client_transport;
  client_transport.address =
      Address::normal(canforge::core::CanId::standard(0x7E0).value(),
                      canforge::core::CanId::standard(0x7E8).value());
  canforge::isotp::Config server_transport;
  server_transport.address =
      Address::normal(canforge::core::CanId::standard(0x7E8).value(),
                      canforge::core::CanId::standard(0x7E0).value());

  ClientConfig client_config;
  client_config.transport = client_transport;
  Client client(client_config);

  auto server_config = canforge::sim::default_server_config(server_transport);
  server_config.erase_duration_ns = 300 * kMs;  // long enough to need P2*
  canforge::sim::UdsServer server(server_config);

  std::uint64_t now = 0;
  const auto pump = [&]() {
    for (int i = 0; i < 2000000 && client.busy(); ++i) {
      for (const auto& f : client.poll(now)) {
        server.on_frame(f, now);
      }
      for (const auto& f : server.poll(now)) {
        client.on_frame(f, now);
      }
      now += 200000ULL;
    }
    return !client.busy() && client.state() == canforge::uds::RequestState::Complete;
  };
  const auto step = [&](const char* label) {
    if (!pump()) {
      std::fprintf(stderr, "%-34s FAILED (%s)\n", label,
                   client.state() == canforge::uds::RequestState::Failed
                       ? std::string(client.failure().message()).c_str()
                       : "timed out");
      return false;
    }
    if (!client.response().positive) {
      std::fprintf(stderr, "%-34s negative: %s\n", label,
                   canforge::uds::describe_nrc(client.response().nrc).c_str());
      return false;
    }
    std::printf("%-34s ok%s\n", label,
                client.response().pending_count != 0 ? " (after responsePending)" : "");
    return true;
  };

  static_cast<void>(client.read_data_by_identifier(0xF190, now));
  if (!step("ReadDataByIdentifier VIN")) {
    return 1;
  }
  {
    const auto& d = client.response().data;
    const std::string vin(d.begin() + 2, d.end());
    std::printf("%-34s %s\n", "  VIN", vin.c_str());
  }

  static_cast<void>(
      client.read_dtc_information(DtcReportType::ReportDtcByStatusMask, 0xFF, now));
  if (!step("ReadDTCInformation")) {
    return 1;
  }
  for (const auto& dtc : Client::parse_dtc_list(client.response().data)) {
    std::printf("%-34s %s  %s\n", "  DTC", canforge::uds::format_dtc(dtc.code).c_str(),
                canforge::uds::describe_dtc_status(dtc.status).c_str());
  }

  if (args.download.empty()) {
    std::printf("\nno --download given; stopping before the flash sequence\n");
    return 0;
  }

  std::ifstream image(args.download, std::ios::binary);
  if (!image) {
    std::fprintf(stderr, "canforge uds: cannot read %s\n", args.download.c_str());
    return 1;
  }
  const std::vector<std::uint8_t> firmware((std::istreambuf_iterator<char>(image)),
                                           std::istreambuf_iterator<char>());
  std::printf("\nflashing %zu bytes from %s\n", firmware.size(), args.download.c_str());

  static_cast<void>(client.diagnostic_session_control(SessionType::Programming, now));
  if (!step("DiagnosticSessionControl prog")) {
    return 1;
  }
  static_cast<void>(client.security_access_request_seed(0x01, now));
  if (!step("SecurityAccess requestSeed")) {
    return 1;
  }
  std::uint32_t seed = 0;
  for (std::size_t i = 1; i <= 4 && i < client.response().data.size(); ++i) {
    seed = (seed << 8u) | client.response().data[i];
  }
  const std::uint32_t key =
      canforge::sim::toy_key_from_seed(seed, server_config.security_secret);
  static_cast<void>(
      client.security_access_send_key(0x02,
                                      {static_cast<std::uint8_t>((key >> 24u) & 0xFFu),
                                       static_cast<std::uint8_t>((key >> 16u) & 0xFFu),
                                       static_cast<std::uint8_t>((key >> 8u) & 0xFFu),
                                       static_cast<std::uint8_t>(key & 0xFFu)},
                                      now));
  if (!step("SecurityAccess sendKey")) {
    return 1;
  }

  static_cast<void>(
      client.routine_control(RoutineControlType::StartRoutine, 0xFF00, {}, now));
  if (!step("RoutineControl eraseMemory")) {
    return 1;
  }
  static_cast<void>(client.request_download(
      0x08000000u, static_cast<std::uint32_t>(firmware.size()), 4, 4, now));
  if (!step("RequestDownload")) {
    return 1;
  }

  std::uint8_t block = 1;
  std::size_t offset = 0;
  while (offset < firmware.size()) {
    const std::size_t take = std::min<std::size_t>(256, firmware.size() - offset);
    static_cast<void>(client.transfer_data(
        block,
        std::vector<std::uint8_t>(
            firmware.begin() + static_cast<std::ptrdiff_t>(offset),
            firmware.begin() + static_cast<std::ptrdiff_t>(offset + take)),
        now));
    if (!pump() || !client.response().positive) {
      std::fprintf(stderr, "TransferData block %u failed\n", unsigned{block});
      return 1;
    }
    offset += take;
    block = static_cast<std::uint8_t>(block + 1u);
    if (block == 0) {
      block = 1;
    }
    std::printf("\rTransferData %zu / %zu bytes", offset, firmware.size());
    std::fflush(stdout);
  }
  std::printf("\n");

  static_cast<void>(client.request_transfer_exit(now));
  if (!step("RequestTransferExit")) {
    return 1;
  }
  static_cast<void>(
      client.routine_control(RoutineControlType::StartRoutine, 0xFF01, {}, now));
  if (!step("RoutineControl checksum")) {
    return 1;
  }

  std::uint32_t reported = 0;
  for (std::size_t i = 3; i < 7 && i < client.response().data.size(); ++i) {
    reported = (reported << 8u) | client.response().data[i];
  }
  std::uint32_t expected = 0;
  for (const std::uint8_t b : firmware) {
    expected += b;
  }
  std::printf("checksum from ECU 0x%08X, expected 0x%08X: %s\n", reported, expected,
              reported == expected ? "match" : "MISMATCH");
  return reported == expected ? 0 : 1;
}

int command_dump(const Args& args) {
  canforge::core::Database database;
  const bool have_database =
      !args.database.empty() && load_database(args.database, database) == 0;
  if (!args.database.empty() && !have_database) {
    return 1;
  }
  if (args.log.empty()) {
    std::fprintf(stderr, "canforge dump: --log <file> is required for now\n");
    return 2;
  }
  auto reader = canforge::transport::open_reader(args.log);
  if (!reader) {
    std::fprintf(stderr, "canforge: cannot read %s: %.*s\n", args.log.c_str(),
                 static_cast<int>(reader.error().message().size()),
                 reader.error().message().data());
    return 1;
  }
  auto records = reader.value()->read_all();
  if (!records) {
    std::fprintf(stderr, "canforge: %.*s\n",
                 static_cast<int>(records.error().message().size()),
                 records.error().message().data());
    return 1;
  }
  for (const auto& record : records.value()) {
    std::printf("%14.6f  %s  %0*X  [%zu]",
                static_cast<double>(record.frame.timestamp_ns()) / 1e9,
                record.channel.c_str(), record.frame.id().is_extended() ? 8 : 3,
                record.frame.id().value(), record.frame.size());
    if (have_database) {
      const auto* message = database.find_message(record.frame.id());
      if (message != nullptr) {
        std::printf("  %s", message->name().c_str());
        const auto decoded = message->decode(record.frame);
        if (decoded) {
          for (const auto& signal : decoded.value()) {
            std::printf("\n      %-24.*s %12.4f %s",
                        static_cast<int>(signal.name().size()), signal.name().data(),
                        signal.value,
                        signal.signal != nullptr ? signal.signal->unit().c_str() : "");
          }
        }
      }
    }
    std::printf("\n");
  }
  std::printf("%zu frames\n", records.value().size());
  return 0;
}

int command_info(const Args& args) {
  if (args.database.empty()) {
    std::fprintf(stderr, "canforge info: --database is required\n");
    return 2;
  }
  canforge::core::Database database;
  if (load_database(args.database, database) != 0) {
    return 1;
  }
  std::printf("version   %s\n", database.version().c_str());
  std::printf("nodes     %zu\n", database.nodes().size());
  std::printf("messages  %zu\n", database.messages().size());
  std::size_t signals = 0;
  for (const auto& message : database.messages()) {
    signals += message.signals().size();
  }
  std::printf("signals   %zu\n\n", signals);

  for (const auto& message : database.messages()) {
    std::printf("%0*X  %-24s %2u bytes  %s\n", message.id().is_extended() ? 8 : 3,
                message.id().value(), message.name().c_str(), unsigned{message.dlc()},
                message.transmitter().c_str());
    for (const auto& signal : message.signals()) {
      std::printf(
          "    %-26s %3u|%-2u@%d%c  (%g,%g) [%g|%g] %s\n", signal.name().c_str(),
          signal.layout().start_bit, unsigned{signal.layout().bit_length},
          signal.layout().byte_order == canforge::core::ByteOrder::Intel ? 1 : 0,
          signal.layout().signedness == canforge::core::Signedness::Signed ? '-' : '+',
          signal.layout().factor, signal.layout().offset, signal.layout().minimum,
          signal.layout().maximum, signal.unit().c_str());
    }
  }
  const auto problems = database.lint();
  if (!problems.empty()) {
    std::printf("\n%zu lint findings:\n", problems.size());
    for (const auto& problem : problems) {
      std::printf("  %s\n", problem.c_str());
    }
  }
  return 0;
}

int command_dash(const Args& args) {
#if !CANFORGE_HAVE_TUI
  static_cast<void>(args);
  std::fprintf(stderr,
               "canforge: this build was configured with CANFORGE_BUILD_TUI=OFF\n");
  return 2;
#else
  if (args.config.empty()) {
    std::fprintf(stderr, "canforge dash: --config is required\n");
    return 2;
  }
  auto parsed = canforge::sim::parse_config_file(args.config);
  if (!parsed) {
    std::fprintf(stderr, "canforge: cannot read %s\n", args.config.c_str());
    return 1;
  }
  if (parsed.value().diagnostics.has_errors()) {
    std::fputs(parsed.value().diagnostics.format(args.config).c_str(), stderr);
    return 1;
  }
  std::string database_path = args.database;
  if (database_path.empty()) {
    database_path = resolve_relative(args.config, parsed.value().config.database_path);
  }
  auto database = std::make_unique<canforge::core::Database>();
  if (load_database(database_path, *database) != 0) {
    return 1;
  }
  auto simulator = canforge::sim::Simulator::build(parsed.value().config, *database);
  if (!simulator) {
    std::fprintf(stderr, "canforge dash: %.*s\n",
                 static_cast<int>(simulator.error().message().size()),
                 simulator.error().message().data());
    return 1;
  }

  const BusTiming timing{parsed.value().config.nominal_bitrate,
                         parsed.value().config.data_bitrate};
  canforge::tui::ViewModel model(database.get(), timing);

  // Two sources of frames behind one dashboard.
  //   --virtual   the simulator from the config runs in-process, on its own
  //               thread, and feeds the view model. Needs no vcan and no root.
  //   otherwise   the dashboard listens on a real bus and shows whatever is on
  //               the wire. The config then only names the database and the
  //               bitrate; the nodes in it are not run. Start `canforge sim`
  //               next to it to put them on the bus.
  // Either way the dashboard renders from snapshots, which is the arrangement
  // TSan checks, and only the producer thread ever touches the bus.
  std::unique_ptr<IBus> live_bus;
  if (!args.virtual_bus) {
    canforge::transport::SocketCanOptions options;
    options.timing = timing;
    live_bus = std::make_unique<canforge::transport::SocketCanBus>(args.bus, options);
    if (const auto opened = live_bus->open(); !opened) {
      std::fprintf(stderr,
                   "canforge dash: cannot open %s: %.*s\n"
                   "  (--virtual runs the simulation in-process, without a bus)\n",
                   args.bus.c_str(), static_cast<int>(opened.error().message().size()),
                   opened.error().message().data());
      return 1;
    }
  }

  std::atomic<bool> running{true};
  std::mutex tx_mutex;
  std::vector<std::pair<std::string, std::string>> periodic;
  std::vector<Frame> outbox;  // live bus: frames the transmit panel wants sent

  // "Signal=number Signal=number" for a named message, as typed into the
  // transmit panel.
  const auto build_frame =
      [&database](const std::string& name,
                  const std::string& values) -> canforge::core::Result<Frame> {
    const auto* message = database->find_message(name);
    if (message == nullptr) {
      return canforge::core::Error(canforge::core::ErrorCode::CodecUnknownSignal,
                                   "no message with that name");
    }
    std::vector<std::pair<std::string, double>> pairs;
    std::istringstream in(values);
    std::string token;
    while (in >> token) {
      const std::size_t eq = token.find('=');
      if (eq == std::string::npos) {
        return canforge::core::Error(canforge::core::ErrorCode::InvalidArgument,
                                     "values must be written Signal=number");
      }
      pairs.emplace_back(token.substr(0, eq),
                         std::strtod(token.c_str() + eq + 1, nullptr));
    }
    return message->encode(pairs);
  };

  std::thread producer([&] {
    if (live_bus) {
      // Periodic transmit-panel messages go out every 100 ms, the cycle time of
      // most body and powertrain messages.
      constexpr auto kPeriodicInterval = std::chrono::milliseconds(100);
      auto next_periodic = std::chrono::steady_clock::now();
      while (running) {
        // The first receive waits, so an idle bus does not spin a core; the
        // rest drain whatever else has arrived.
        auto got = live_bus->receive(std::chrono::milliseconds(5));
        while (got) {
          model.ingest(got.value());
          got = live_bus->receive(std::chrono::nanoseconds(0));
        }

        std::vector<Frame> to_send;
        {
          const std::lock_guard<std::mutex> lock(tx_mutex);
          to_send.swap(outbox);
          if (std::chrono::steady_clock::now() >= next_periodic) {
            next_periodic += kPeriodicInterval;
            for (const auto& [name, values] : periodic) {
              if (auto built = build_frame(name, values)) {
                to_send.push_back(built.value());
              }
            }
          }
        }
        for (Frame& frame : to_send) {
          // The kernel stamps received frames with the wall clock; a socket
          // does not hear its own transmissions, so they are stamped the same
          // way and shown by hand.
          frame.set_timestamp_ns(static_cast<std::uint64_t>(
              std::chrono::duration_cast<std::chrono::nanoseconds>(
                  std::chrono::system_clock::now().time_since_epoch())
                  .count()));
          if (live_bus->send(frame)) {
            model.ingest(frame.widen<64>());
          }
        }
      }
      return;
    }

    std::uint64_t now = 0;
    const auto wall_start = std::chrono::steady_clock::now();
    while (running) {
      for (const Frame& frame : simulator.value()->step(now)) {
        model.ingest(frame.widen<64>());
      }
      {
        const std::lock_guard<std::mutex> lock(tx_mutex);
        for (const auto& [name, values] : periodic) {
          if (auto built = build_frame(name, values)) {
            auto frame = built.value();
            frame.set_timestamp_ns(now);
            model.ingest(frame.widen<64>());
          }
        }
      }
      // Track the wall clock so the dashboard shows real rates and not
      // running the simulation as fast as the CPU allows.
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
      now = static_cast<std::uint64_t>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(
              std::chrono::steady_clock::now() - wall_start)
              .count());
    }
  });

  auto transmit = [&](const std::string& name, const std::string& values,
                      bool repeat) -> canforge::core::Status {
    if (name.empty()) {
      const std::lock_guard<std::mutex> lock(tx_mutex);
      periodic.clear();
      return canforge::core::ok();
    }
    auto built = build_frame(name, values);
    if (!built) {
      return built.error();
    }
    if (live_bus) {
      const std::lock_guard<std::mutex> lock(tx_mutex);
      outbox.push_back(built.value());  // the producer thread sends and shows it
    } else {
      model.ingest(built.value().widen<64>());
    }
    if (repeat) {
      const std::lock_guard<std::mutex> lock(tx_mutex);
      periodic.emplace_back(name, values);
    }
    return canforge::core::ok();
  };

  canforge::tui::Dashboard dashboard(model, database.get(), transmit);
  const int code = dashboard.run();
  running = false;
  producer.join();
  return code;
#endif
}

// --- diag -------------------------------------------------------------------

/// Hexadecimal digits to a number. Nothing but digits is accepted: "7E0", not
/// "0x7E0" and not "7E0h".
bool parse_hex(const std::string& text, std::uint32_t max, std::uint32_t& out) {
  if (text.empty() || text.size() > 8) {
    return false;
  }
  std::uint32_t value = 0;
  for (const char c : text) {
    std::uint32_t digit = 0;
    if (c >= '0' && c <= '9') {
      digit = static_cast<std::uint32_t>(c - '0');
    } else if (c >= 'a' && c <= 'f') {
      digit = static_cast<std::uint32_t>(c - 'a') + 10u;
    } else if (c >= 'A' && c <= 'F') {
      digit = static_cast<std::uint32_t>(c - 'A') + 10u;
    } else {
      return false;
    }
    value = (value << 4u) | digit;
  }
  out = value;
  return value <= max;
}

/// "3C", "3E 00" or "3E00" to bytes. Spaces may separate whole bytes.
bool parse_hex_bytes(const std::string& text, std::vector<std::uint8_t>& out) {
  out.clear();
  std::string digits;
  for (const char c : text) {
    if (c == ' ') {
      if (digits.size() % 2 != 0) {
        return false;
      }
      continue;
    }
    digits.push_back(c);
  }
  if (digits.empty() || digits.size() % 2 != 0) {
    return false;
  }
  for (std::size_t i = 0; i < digits.size(); i += 2) {
    std::uint32_t byte = 0;
    if (!parse_hex(digits.substr(i, 2), 0xFFu, byte)) {
      return false;
    }
    out.push_back(static_cast<std::uint8_t>(byte));
  }
  return true;
}

std::string hex_bytes(const std::uint8_t* data, std::size_t size) {
  std::string out;
  char buffer[4];
  for (std::size_t i = 0; i < size; ++i) {
    std::snprintf(buffer, sizeof(buffer), i == 0 ? "%02X" : " %02X",
                  static_cast<unsigned>(data[i]));
    out += buffer;
  }
  return out;
}

/// What one diag run needs everywhere.
struct DiagSession {
  canforge::uds::Client& client;
  canforge::uds::BusLink& link;
  const canforge::uds::DidCatalog* catalog;  ///< May be null.
  bool all_positive = true;

  /// Carries the request just queued on the client to its answer and prints the
  /// outcome unless it was positive. Returns the response only if it was.
  const canforge::uds::Response* finish(const std::string& label) {
    const auto result = link.run(std::chrono::seconds(30));
    if (!result) {
      std::printf("%-26s FAILED: %.*s\n", label.c_str(),
                  static_cast<int>(result.error().message().size()),
                  result.error().message().data());
      all_positive = false;
      return nullptr;
    }
    if (!result.value().positive) {
      std::printf("%-26s negative: %s\n", label.c_str(),
                  canforge::uds::describe_nrc(result.value().nrc).c_str());
      all_positive = false;
      return nullptr;
    }
    return &client.response();
  }

  static const char* pending_note(const canforge::uds::Response& response) {
    return response.pending_count != 0 ? " (after responsePending)" : "";
  }
};

void diag_session(DiagSession& d, const std::string& name) {
  using canforge::uds::SessionType;
  SessionType type = SessionType::Default;
  if (name == "extended") {
    type = SessionType::ExtendedDiagnostic;
  } else if (name == "programming") {
    type = SessionType::Programming;
  } else if (name != "default") {
    std::printf("%-26s FAILED: use default, extended or programming\n",
                ("session " + name).c_str());
    d.all_positive = false;
    return;
  }
  const std::string label = "session " + name;
  static_cast<void>(d.client.diagnostic_session_control(type, d.link.now_ns()));
  if (const auto* response = d.finish(label)) {
    // Sub-function echo, then P2 in ms and P2* in units of 10 ms.
    if (response->data.size() >= 5) {
      const unsigned p2 =
          (static_cast<unsigned>(response->data[1]) << 8u) | response->data[2];
      const unsigned p2_star =
          ((static_cast<unsigned>(response->data[3]) << 8u) | response->data[4]) * 10u;
      std::printf("%-26s ok   P2 %u ms, P2* %u ms\n", label.c_str(), p2, p2_star);
    } else {
      std::printf("%-26s ok\n", label.c_str());
    }
  }
}

void diag_read_did(DiagSession& d, const std::string& text) {
  std::uint32_t did = 0;
  const std::string label = "read " + text;
  if (!parse_hex(text, 0xFFFFu, did)) {
    std::printf("%-26s FAILED: a DID is up to four hex digits\n", label.c_str());
    d.all_positive = false;
    return;
  }
  static_cast<void>(d.client.read_data_by_identifier(static_cast<std::uint16_t>(did),
                                                     d.link.now_ns()));
  const auto* response = d.finish(label);
  if (response == nullptr) {
    return;
  }
  if (response->data.size() < 2) {
    std::printf("%-26s FAILED: the response has no identifier\n", label.c_str());
    d.all_positive = false;
    return;
  }
  const std::uint8_t* bytes = response->data.data() + 2;
  const std::size_t size = response->data.size() - 2;
  const canforge::uds::DidEntry* entry =
      d.catalog != nullptr ? d.catalog->find(static_cast<std::uint16_t>(did)) : nullptr;
  if (entry == nullptr) {
    std::printf("%-26s %s\n", label.c_str(), hex_bytes(bytes, size).c_str());
    return;
  }
  const auto decoded = d.catalog->decode(*entry, bytes, size);
  if (!decoded) {
    // The ECU and the DID file disagree about this identifier. Say so, and
    // show what actually arrived.
    std::printf("%-26s %s\n", (label + " " + entry->name).c_str(),
                hex_bytes(bytes, size).c_str());
    std::printf("  FAILED: %zu bytes received, the DID file describes %zu\n", size,
                entry->length);
    d.all_positive = false;
    return;
  }
  std::printf("%s %s\n", label.c_str(), entry->name.c_str());
  for (const auto& field : decoded.value()) {
    std::printf("  %-24s %s\n", field.name.c_str(), field.text.c_str());
  }
}

void diag_write_did(DiagSession& d, const std::string& text) {
  const std::size_t eq = text.find('=');
  std::uint32_t did = 0;
  std::vector<std::uint8_t> value;
  const std::string label = "write " + text;
  if (eq == std::string::npos || !parse_hex(text.substr(0, eq), 0xFFFFu, did) ||
      !parse_hex_bytes(text.substr(eq + 1), value)) {
    std::printf("%-26s FAILED: write as <did>=<hex bytes>, e.g. 0200=3C\n",
                label.c_str());
    d.all_positive = false;
    return;
  }
  static_cast<void>(d.client.write_data_by_identifier(static_cast<std::uint16_t>(did),
                                                      value, d.link.now_ns()));
  if (const auto* response = d.finish(label)) {
    std::printf("%-26s ok%s\n", label.c_str(), DiagSession::pending_note(*response));
  }
}

void diag_read_dtc(DiagSession& d, const std::string& mask_text) {
  std::uint32_t mask = 0;
  const std::string label = "read-dtc mask " + mask_text;
  if (!parse_hex(mask_text, 0xFFu, mask)) {
    std::printf("%-26s FAILED: the mask is one byte in hex\n", label.c_str());
    d.all_positive = false;
    return;
  }
  static_cast<void>(
      d.client.read_dtc_information(canforge::uds::DtcReportType::ReportDtcByStatusMask,
                                    static_cast<std::uint8_t>(mask), d.link.now_ns()));
  const auto* response = d.finish(label);
  if (response == nullptr) {
    return;
  }
  const auto dtcs = canforge::uds::Client::parse_dtc_list(response->data);
  std::printf("%-26s %zu DTC%s\n", label.c_str(), dtcs.size(),
              dtcs.size() == 1 ? "" : "s");
  for (const auto& dtc : dtcs) {
    std::printf("  %s  0x%02X  %s\n", canforge::uds::format_dtc(dtc.code).c_str(),
                static_cast<unsigned>(dtc.status),
                canforge::uds::describe_dtc_status(dtc.status).c_str());
  }
}

void diag_unlock(DiagSession& d) {
  static_cast<void>(d.client.security_access_request_seed(0x01, d.link.now_ns()));
  const auto* seed_response = d.finish("unlock");
  if (seed_response == nullptr) {
    return;
  }
  if (seed_response->data.size() < 5) {
    std::printf("%-26s FAILED: the seed is shorter than four bytes\n", "unlock");
    d.all_positive = false;
    return;
  }
  std::uint32_t seed = 0;
  for (std::size_t i = 1; i <= 4; ++i) {
    seed = (seed << 8u) | seed_response->data[i];
  }
  if (seed == 0) {
    std::printf("%-26s ok   already unlocked\n", "unlock");
    return;
  }
  // The same deliberately weak transform the simulated ECU uses; see
  // toy_key_from_seed. An ECU with a real algorithm needs a real tester.
  const std::uint32_t key = canforge::sim::toy_key_from_seed(
      seed, canforge::sim::UdsServerConfig{}.security_secret);
  static_cast<void>(d.client.security_access_send_key(
      0x02,
      {static_cast<std::uint8_t>((key >> 24u) & 0xFFu),
       static_cast<std::uint8_t>((key >> 16u) & 0xFFu),
       static_cast<std::uint8_t>((key >> 8u) & 0xFFu),
       static_cast<std::uint8_t>(key & 0xFFu)},
      d.link.now_ns()));
  if (d.finish("unlock") != nullptr) {
    std::printf("%-26s ok\n", "unlock");
  }
}

void diag_request(DiagSession& d, const std::string& text) {
  std::vector<std::uint8_t> request;
  const std::string label = "request " + text;
  if (!parse_hex_bytes(text, request)) {
    std::printf("%-26s FAILED: give the request as hex bytes, e.g. \"3E 00\"\n",
                label.c_str());
    d.all_positive = false;
    return;
  }
  static_cast<void>(d.client.request(request, d.link.now_ns()));
  if (const auto* response = d.finish(label)) {
    // `data` is what follows the response SID; put the SID back for display.
    std::vector<std::uint8_t> whole = {response->service};
    whole.insert(whole.end(), response->data.begin(), response->data.end());
    std::printf("%-26s %s%s\n", label.c_str(),
                hex_bytes(whole.data(), whole.size()).c_str(),
                DiagSession::pending_note(*response));
  }
}

int command_diag(const Args& args) {
  if (args.diag.empty()) {
    std::fprintf(stderr,
                 "canforge diag: nothing to do; give at least one action, e.g. "
                 "--read-did F190\n");
    return 2;
  }
  if (args.virtual_bus) {
    std::fprintf(stderr,
                 "canforge diag: talks to an ECU on a bus; for the simulated ECU "
                 "use `canforge uds --virtual`\n");
    return 2;
  }
  std::uint32_t tx = 0;
  std::uint32_t rx = 0;
  if (!parse_hex(args.tx_id, canforge::core::kStandardIdMax, tx) ||
      !parse_hex(args.rx_id, canforge::core::kStandardIdMax, rx)) {
    std::fprintf(stderr,
                 "canforge diag: --tx-id and --rx-id are 11-bit identifiers in hex\n");
    return 2;
  }

  canforge::uds::DidCatalog catalog;
  if (!args.did_file.empty()) {
    auto loaded = canforge::uds::DidCatalog::load(args.did_file);
    if (!loaded) {
      std::fprintf(stderr, "canforge diag: %s: %.*s (at %u)\n", args.did_file.c_str(),
                   static_cast<int>(loaded.error().message().size()),
                   loaded.error().message().data(), loaded.error().detail().a);
      return 1;
    }
    catalog = std::move(loaded).value();
  }

  canforge::transport::SocketCanBus bus(args.bus);
  if (const auto opened = bus.open(); !opened) {
    std::fprintf(stderr, "canforge diag: cannot open %s: %.*s\n", args.bus.c_str(),
                 static_cast<int>(opened.error().message().size()),
                 opened.error().message().data());
    return 1;
  }

  canforge::uds::ClientConfig config;
  config.transport.address =
      canforge::isotp::Address::normal(canforge::core::CanId::standard(tx).value(),
                                       canforge::core::CanId::standard(rx).value());
  config.tester_present_keepalive = args.keepalive;
  canforge::uds::Client client(config);
  canforge::uds::BusLink link(bus, client);
  if (const auto filtered = link.install_filter(); !filtered) {
    std::fprintf(stderr, "canforge diag: cannot set the receive filter on %s\n",
                 args.bus.c_str());
    return 1;
  }

  DiagSession d{client, link, args.did_file.empty() ? nullptr : &catalog};
  for (const DiagAction& action : args.diag) {
    switch (action.kind) {
      case DiagAction::Kind::Session:
        diag_session(d, action.text);
        break;
      case DiagAction::Kind::ReadDid:
        diag_read_did(d, action.text);
        break;
      case DiagAction::Kind::WriteDid:
        diag_write_did(d, action.text);
        break;
      case DiagAction::Kind::ReadDtc:
        diag_read_dtc(d, args.mask);
        break;
      case DiagAction::Kind::ClearDtc:
        static_cast<void>(
            d.client.clear_diagnostic_information(0xFFFFFFu, d.link.now_ns()));
        if (const auto* response = d.finish("clear-dtc")) {
          std::printf("%-26s ok%s\n", "clear-dtc",
                      DiagSession::pending_note(*response));
        }
        break;
      case DiagAction::Kind::Unlock:
        diag_unlock(d);
        break;
      case DiagAction::Kind::Reset:
        static_cast<void>(
            d.client.ecu_reset(canforge::uds::ResetType::HardReset, d.link.now_ns()));
        if (d.finish("reset") != nullptr) {
          std::printf("%-26s ok\n", "reset");
        }
        break;
      case DiagAction::Kind::Request:
        diag_request(d, action.text);
        break;
      case DiagAction::Kind::Wait: {
        char* end = nullptr;
        const long ms = std::strtol(action.text.c_str(), &end, 10);
        const std::string label = "wait " + action.text + " ms";
        if (action.text.empty() || *end != '\0' || ms < 0 || ms > 3600000) {
          std::printf("%-26s FAILED: give the pause in milliseconds\n", label.c_str());
          d.all_positive = false;
          break;
        }
        if (const auto idled = link.idle(std::chrono::milliseconds(ms)); !idled) {
          std::printf("%-26s FAILED: the bus went away\n", label.c_str());
          d.all_positive = false;
          break;
        }
        std::printf("%-26s ok\n", label.c_str());
        break;
      }
    }
    std::fflush(stdout);
  }
  return d.all_positive ? 0 : 1;
}

int command_flash(const Args& args) {
  canforge::cli::FlashOptions options;
  options.bus = args.bus;
  options.package = args.package;
  options.expect_reject = args.expect_reject;
  options.vcu_standby = args.vcu_standby;
  options.p2_ms = args.p2_ms;
  std::uint32_t tx = 0;
  std::uint32_t rx = 0;
  std::string secret = args.secret;
  if (secret.size() > 2 && secret[0] == '0' && (secret[1] == 'x' || secret[1] == 'X')) {
    secret = secret.substr(2);
  }
  if (!parse_hex(args.tx_id, canforge::core::kStandardIdMax, tx) ||
      !parse_hex(args.rx_id, canforge::core::kStandardIdMax, rx) ||
      !parse_hex(secret, 0xFFFFFFFFu, options.secret) || args.p2_ms == 0) {
    std::fprintf(
        stderr,
        "canforge flash: --secret is up to 8 hex digits, --tx-id and --rx-id are "
        "11-bit identifiers in hex, --p2 is a number of ms\n");
    return 2;
  }
  options.tx_id = tx;
  options.rx_id = rx;
  return canforge::cli::run_flash(options);
}

int command_gen_c(const Args& args) {
  if (args.database.empty() || args.prefix.empty()) {
    std::fprintf(stderr, "canforge gen-c: --database and --prefix are required\n");
    return 2;
  }
  canforge::core::Database database;
  if (load_database(args.database, database) != 0) {
    return 1;
  }

  canforge::codegen::CGenOptions options;
  options.prefix = args.prefix;
  options.node = args.node;
  options.out_dir = args.out.empty() ? "." : args.out;
  options.raw = args.raw;
  // The banner names the database by file name only: generated code is
  // committed, and it must not differ between two people's checkouts.
  const std::size_t slash = args.database.find_last_of('/');
  options.source_name =
      slash == std::string::npos ? args.database : args.database.substr(slash + 1);

  // Rendered once more than strictly needed, so the skipped messages can be
  // reported; generating code is far too cheap for that to matter.
  const auto rendered = canforge::codegen::render_c(database, options);
  if (!rendered) {
    std::fprintf(stderr, "canforge gen-c: %.*s\n",
                 static_cast<int>(rendered.error().message().size()),
                 rendered.error().message().data());
    return 1;
  }
  for (const std::string& skipped : rendered.value().skipped) {
    std::fprintf(stderr, "canforge gen-c: skipped %s\n", skipped.c_str());
  }
  const auto written = canforge::codegen::generate_c(database, options);
  if (!written) {
    std::fprintf(stderr, "canforge gen-c: %.*s\n",
                 static_cast<int>(written.error().message().size()),
                 written.error().message().data());
    return 1;
  }
  std::string dir = options.out_dir;
  if (dir.back() != '/') {
    dir += '/';
  }
  std::printf("wrote %s%s_com.h\nwrote %s%s_com.c\n", dir.c_str(),
              options.prefix.c_str(), dir.c_str(), options.prefix.c_str());
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    usage(2);
  }
  const std::string command = argv[1];
  if (command == "-h" || command == "--help" || command == "help") {
    usage(0);
  }
  const Args args = parse_args(argc, argv);

  if (command == "sim") {
    return command_sim(args);
  }
  if (command == "dash") {
    return command_dash(args);
  }
  if (command == "uds") {
    return command_uds(args);
  }
  if (command == "dump") {
    return command_dump(args);
  }
  if (command == "info") {
    return command_info(args);
  }
  if (command == "gen-c") {
    return command_gen_c(args);
  }
  if (command == "diag") {
    return command_diag(args);
  }
  if (command == "flash") {
    return command_flash(args);
  }
  std::fprintf(stderr, "canforge: unknown command '%s'\n", command.c_str());
  usage(2);
}
