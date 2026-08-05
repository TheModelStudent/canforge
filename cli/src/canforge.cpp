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
#include "canforge/uds/Uds.hpp"

namespace {

using canforge::core::Frame;
using canforge::transport::BusTiming;
using canforge::transport::IBus;

constexpr std::uint64_t kMs = 1000000ULL;

struct Args {
  std::string config;
  std::string bus = "vcan0";
  std::string database;
  std::string log;
  std::string download;
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
      "  dump     decode traffic from a bus or a log file\n"
      "  info     summarise a DBC database\n"
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
      "\n"
      "examples:\n"
      "  canforge sim --config vehicle.cfg --bus vcan0\n"
      "  canforge dash --config vehicle.cfg --virtual\n"
      "  canforge sim --config vehicle.cfg --virtual --duration 10\n"
      "  canforge uds --virtual --download firmware.bin\n"
      "  canforge info --database powertrain.dbc\n");
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
    } else if (matches(a, "--duration")) {
      args.duration_s = std::strtod(next("--duration"), nullptr);
    } else if (matches(a, "--speed")) {
      args.speed = std::strtod(next("--speed"), nullptr);
    } else if (matches(a, "--bitrate")) {
      args.bitrate =
          static_cast<std::uint32_t>(std::strtoul(next("--bitrate"), nullptr, 10));
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

  // The simulator runs on its own thread and feeds the view model; the
  // dashboard renders from snapshots. That is the arrangement TSan checks.
  std::atomic<bool> running{true};
  std::mutex tx_mutex;
  std::vector<std::pair<std::string, std::string>> periodic;

  std::thread producer([&] {
    auto medium = canforge::transport::VirtualMedium::create(timing);
    auto bus = medium->attach("dash");
    static_cast<void>(bus->open());
    std::uint64_t now = 0;
    const auto wall_start = std::chrono::steady_clock::now();
    while (running) {
      for (const Frame& frame : simulator.value()->step(now)) {
        model.ingest(frame.widen<64>());
      }
      {
        const std::lock_guard<std::mutex> lock(tx_mutex);
        for (const auto& [name, values] : periodic) {
          const auto* message = database->find_message(name);
          if (message == nullptr) {
            continue;
          }
          std::vector<std::pair<std::string, double>> pairs;
          std::istringstream in(values);
          std::string token;
          while (in >> token) {
            const std::size_t eq = token.find('=');
            if (eq != std::string::npos) {
              pairs.emplace_back(token.substr(0, eq),
                                 std::strtod(token.c_str() + eq + 1, nullptr));
            }
          }
          if (auto built = message->encode(pairs)) {
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
    auto built = message->encode(pairs);
    if (!built) {
      return built.error();
    }
    model.ingest(built.value().widen<64>());
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
  std::fprintf(stderr, "canforge: unknown command '%s'\n", command.c_str());
  usage(2);
}
