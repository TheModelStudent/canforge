// SPDX-License-Identifier: MIT
#include "Flash.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>  // std::size
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "canforge/sim/Ecu.hpp"
#include "canforge/sim/UdsServer.hpp"
#include "canforge/text/Json.hpp"
#include "canforge/transport/SocketCanBus.hpp"
#include "canforge/uds/BusLink.hpp"
#include "canforge/uds/Uds.hpp"

namespace canforge::cli {
namespace {

using Bytes = std::vector<std::uint8_t>;
using std::chrono::milliseconds;
using std::chrono::seconds;

constexpr std::uint64_t kMs = 1000000ULL;
constexpr std::size_t kSlotInfoLen = 12;
constexpr std::uint8_t kTrialConfirmed = 3;    // trial state in F1A0, byte 9
constexpr std::uint8_t kAddrLenFormat = 0x44;  // 4-byte address, 4-byte size
constexpr auto kResetSettle = milliseconds(300);
constexpr auto kBootloaderWait = seconds(3);
constexpr auto kStartupWait = seconds(2);
constexpr auto kConfirmWait = seconds(15);
// A transfer block is sent at most this often; between two sends the tester
// waits for the server's N_Cr to run out (1000 ms in the BTMS ECU).
constexpr unsigned kBlockTries = 3;
constexpr auto kResendPause = milliseconds(1100);

/// One slot of a package, as the manifest describes it and as read from disk.
struct SlotFile {
  std::string file;
  std::uint32_t base = 0;
  Bytes data;
};

struct Package {
  std::string dir;
  std::string version;
  std::string git;
  SlotFile slots[2];  ///< A, B
};

bool fail_package(const std::string& message) {
  std::fprintf(stderr, "canforge flash: %s\n", message.c_str());
  return false;
}

bool read_file(const std::string& path, Bytes& out) {
  // seekg/read, not istreambuf_iterator: GCC 13 at -O2 reports a null dereference
  // inside the iterator (-Wnull-dereference), and this build treats it as an error.
  std::ifstream in(path, std::ios::binary | std::ios::ate);
  if (!in) {
    return false;
  }
  const std::streamoff size = in.tellg();
  if (size < 0) {
    return false;
  }
  out.resize(static_cast<std::size_t>(size));
  in.seekg(0);
  return static_cast<bool>(in.read(reinterpret_cast<char*>(out.data()), size));
}

bool parse_address(const std::string& text, std::uint32_t& out) {
  if (text.size() < 3 || text[0] != '0' || (text[1] != 'x' && text[1] != 'X')) {
    return false;
  }
  char* end = nullptr;
  const unsigned long value = std::strtoul(text.c_str() + 2, &end, 16);
  if (*end != '\0' || value > 0xFFFFFFFFul) {
    return false;
  }
  out = static_cast<std::uint32_t>(value);
  return true;
}

/// Reads manifest.json and both slot files, and checks they agree.
bool load_package(const std::string& dir, Package& pkg) {
  pkg.dir = dir;
  Bytes text;
  const std::string manifest_path = dir + "/manifest.json";
  if (!read_file(manifest_path, text)) {
    return fail_package("cannot read " + manifest_path);
  }
  const auto parsed = text::parse_json(
      std::string_view(reinterpret_cast<const char*>(text.data()), text.size()));
  if (!parsed) {
    return fail_package(manifest_path + ": not valid JSON (at byte " +
                        std::to_string(parsed.error().detail().a) + ")");
  }
  const text::Json& root = parsed.value();
  const text::Json* version = root.member("version");
  const text::Json* slots = root.member("slots");
  if (version == nullptr || version->type != text::Json::Type::String ||
      slots == nullptr || slots->type != text::Json::Type::Object) {
    return fail_package(manifest_path + ": needs \"version\" and \"slots\"");
  }
  pkg.version = version->string;
  if (const text::Json* git = root.member("git");
      git != nullptr && git->type == text::Json::Type::String) {
    pkg.git = git->string;
  }
  const char* names[2] = {"A", "B"};
  for (int i = 0; i < 2; ++i) {
    const text::Json* slot = slots->member(names[i]);
    const text::Json* file = slot != nullptr ? slot->member("file") : nullptr;
    const text::Json* base = slot != nullptr ? slot->member("base") : nullptr;
    const text::Json* size = slot != nullptr ? slot->member("size") : nullptr;
    if (file == nullptr || file->type != text::Json::Type::String || base == nullptr ||
        base->type != text::Json::Type::String || size == nullptr ||
        size->type != text::Json::Type::Number) {
      return fail_package(manifest_path + ": slot " + names[i] +
                          " needs \"file\", \"base\" and \"size\"");
    }
    SlotFile& s = pkg.slots[i];
    s.file = file->string;
    if (!parse_address(base->string, s.base)) {
      return fail_package(manifest_path + ": slot " + names[i] +
                          ": base must look like 0x08040000");
    }
    if (!read_file(dir + "/" + s.file, s.data)) {
      return fail_package("cannot read " + dir + "/" + s.file);
    }
    if (static_cast<double>(s.data.size()) != size->number || s.data.empty()) {
      return fail_package(s.file + " is not the size the manifest gives");
    }
  }
  return true;
}

void push_be32(Bytes& out, std::uint32_t v) {
  for (int shift = 24; shift >= 0; shift -= 8) {
    out.push_back(static_cast<std::uint8_t>(v >> shift));
  }
}

/// The result byte of routine FF01, by the names of the ECU's image check.
std::string image_result_name(std::uint8_t result) {
  static const char* const names[] = {
      "ok",           "IMG_ERR_MAGIC", "IMG_ERR_HDR_CRC", "IMG_ERR_SLOT",
      "IMG_ERR_SIZE", "IMG_ERR_CRC",   "IMG_ERR_VECTOR",  "IMG_ERR_SIG"};
  if (static_cast<std::size_t>(result) < std::size(names)) {
    return names[result];
  }
  return "result " + std::to_string(result);
}

class Flasher {
 public:
  Flasher(uds::Client& client, uds::BusLink& link) : client_(client), link_(link) {}

  /// Sends `request` and waits for the final answer. On a positive one returns
  /// it; otherwise prints why under `label` and returns null. Prints nothing
  /// on success: the caller says what it means.
  const uds::Response* ask(const std::string& label, Bytes request,
                           milliseconds timeout = seconds(60)) {
    if (const auto started = client_.request(std::move(request), link_.now_ns());
        !started) {
      print_failure(label, started.error().message());
      return nullptr;
    }
    const auto result = link_.run(timeout);
    if (!result) {
      print_failure(label, result.error().message());
      return nullptr;
    }
    if (!result.value().positive) {
      std::printf("%-22s negative: %s\n", label.c_str(),
                  uds::describe_nrc(result.value().nrc).c_str());
      std::fflush(stdout);
      return nullptr;
    }
    return &client_.response();
  }

  /// For TransferData: like ask(), except that a server that does not answer
  /// in time is not a failure yet. Then it returns null with `silent` set and
  /// prints nothing, so the caller can send the block again.
  const uds::Response* ask_block(const std::string& label, Bytes request,
                                 bool& silent) {
    silent = false;
    if (const auto started = client_.request(std::move(request), link_.now_ns());
        !started) {
      print_failure(label, started.error().message());
      return nullptr;
    }
    const auto result = link_.run(seconds(60));
    if (!result) {
      if (result.error().code() == core::ErrorCode::TransportTimeout) {
        silent = true;
      } else {
        print_failure(label, result.error().message());
      }
      return nullptr;
    }
    if (!result.value().positive) {
      std::printf("%-22s negative: %s\n", label.c_str(),
                  uds::describe_nrc(result.value().nrc).c_str());
      std::fflush(stdout);
      return nullptr;
    }
    return &client_.response();
  }

  /// The same, quietly: for polling, where a failure is expected and retried.
  const uds::Response* try_ask(Bytes request, milliseconds timeout) {
    if (!client_.request(std::move(request), link_.now_ns())) {
      return nullptr;
    }
    const auto result = link_.run(timeout);
    if (!result || !result.value().positive) {
      return nullptr;
    }
    return &client_.response();
  }

  void idle(milliseconds duration) { static_cast<void>(link_.idle(duration)); }
  std::uint64_t now_ns() const { return link_.now_ns(); }

  static void ok(const std::string& label, const std::string& note = "") {
    std::printf("%-22s ok%s%s\n", label.c_str(), note.empty() ? "" : "   ",
                note.c_str());
    std::fflush(stdout);
  }
  static void print_failure(const std::string& label, std::string_view why) {
    std::printf("%-22s FAILED: %.*s\n", label.c_str(), static_cast<int>(why.size()),
                why.data());
    std::fflush(stdout);
  }

 private:
  uds::Client& client_;
  uds::BusLink& link_;
};

std::string pending_note(const uds::Response& r) {
  return r.pending_count == 0
             ? ""
             : "(after " + std::to_string(r.pending_count) + " responsePending)";
}

std::string ascii(const Bytes& data, std::size_t from) {
  std::string out;
  for (std::size_t i = from; i < data.size() && data[i] != 0; ++i) {
    out.push_back(static_cast<char>(data[i]));
  }
  return out;
}

double seconds_between(std::uint64_t from_ns, std::uint64_t to_ns) {
  return static_cast<double>(to_ns - from_ns) / 1e9;
}

/// --vcu-standby: the vehicle controller's command, STANDBY, every 100 ms on
/// its own socket and thread, from start() until the object goes away.
class VcuStandby {
 public:
  static constexpr std::uint32_t kId = 0x100;      // VCU_Cmd
  static constexpr std::uint8_t kTargetRaw = 60;   // 30 degC in 0.5 degC
  static constexpr std::uint8_t kModeStandby = 2;  // VCU_CoolingMode
  static constexpr auto kPeriod = milliseconds(100);
  static constexpr auto kLead = milliseconds(300);    // before 10 02
  static constexpr auto kListen = milliseconds(350);  // for another sender

  explicit VcuStandby(std::string bus) : bus_(std::move(bus)) {}
  VcuStandby(const VcuStandby&) = delete;
  VcuStandby& operator=(const VcuStandby&) = delete;
  ~VcuStandby() { stop(); }

  /// Opens the bus, makes sure nobody else sends 0x100, then starts sending.
  bool start() {
    if (const auto opened = socket_.open(); !opened) {
      std::printf("%-22s FAILED: cannot open %s\n", "vcu standby", bus_.c_str());
      return false;
    }
    const auto until = std::chrono::steady_clock::now() + kListen;
    while (std::chrono::steady_clock::now() < until) {
      const auto frame = socket_.receive(milliseconds(20));
      if (frame && !frame.value().id().is_extended() &&
          frame.value().id().value() == kId) {
        std::printf("%-22s FAILED: another node sends 0x100; stop canforge sim first\n",
                    "vcu standby");
        std::fflush(stdout);
        return false;
      }
    }
    thread_ = std::thread([this] { run(); });
    std::printf("%-22s ok   0x100 every 100 ms, mode STANDBY, until the update ends\n",
                "vcu standby");
    std::fflush(stdout);
    std::this_thread::sleep_for(kLead);
    return true;
  }

  void stop() {
    {
      const std::lock_guard<std::mutex> lock(mutex_);
      stopping_ = true;
    }
    wake_.notify_all();
    if (thread_.joinable()) {
      thread_.join();
    }
  }

  std::uint64_t sent() const { return sent_.load(); }

 private:
  void run() {
    std::uint8_t counter = 0;
    auto next = std::chrono::steady_clock::now();
    std::unique_lock<std::mutex> lock(mutex_);
    while (!stopping_) {
      std::uint8_t data[8] = {};  // VCU_Cmd: target, mode, counter in byte 6, CRC in 7
      data[0] = kTargetRaw;
      data[1] = kModeStandby;
      data[6] = static_cast<std::uint8_t>(counter & 0x0Fu);
      data[7] = sim::crc8_sae_j1850(data, sizeof(data));
      if (const auto frame = core::Frame::make(core::CanId::make_standard<kId>(), data,
                                               sizeof(data))) {
        if (socket_.send(frame.value())) {
          sent_.fetch_add(1);
        }
      }
      counter = static_cast<std::uint8_t>((counter + 1u) & 0x0Fu);
      next += kPeriod;
      wake_.wait_until(lock, next, [this] { return stopping_; });
    }
  }

  std::string bus_;
  transport::SocketCanBus socket_{bus_};
  std::thread thread_;
  std::mutex mutex_;
  std::condition_variable wake_;
  bool stopping_ = false;
  std::atomic<std::uint64_t> sent_{0};
};

/// Steps 1-4, in the application: quiet the ECU, then send it to its bootloader.
bool enter_programming(Flasher& f) {
  if (f.ask("session extended", {0x10, 0x03}) == nullptr) {
    return false;
  }
  Flasher::ok("session extended");
  if (f.ask("dtc setting off", {0x85, 0x02}) == nullptr) {
    return false;
  }
  Flasher::ok("dtc setting off");
  if (f.ask("communication off", {0x28, 0x03, 0x01}) == nullptr) {
    return false;
  }
  Flasher::ok("communication off");
  if (f.ask("session programming", {0x10, 0x02}) == nullptr) {
    // Refused (the BTMS ECU, for one, programs only while the vehicle is parked):
    // leave the ECU as it was found. Back in the default session a UDS server
    // switches DTC setting and its cyclic messages on again.
    if (f.try_ask({0x10, 0x01}, milliseconds(500)) != nullptr) {
      Flasher::ok("session default", "nothing changed on the ECU");
    }
    return false;
  }
  Flasher::ok("session programming", "the ECU restarts into its bootloader");
  return true;
}

/// Step 5: TesterPresent until the bootloader answers.
bool wait_for_bootloader(Flasher& f, milliseconds p2) {
  f.idle(kResetSettle);
  const std::uint64_t start = f.now_ns();
  const std::uint64_t until =
      start + static_cast<std::uint64_t>(kBootloaderWait.count()) * kMs * 1000;
  while (f.now_ns() < until) {
    if (f.try_ask({0x3E, 0x00}, p2 + milliseconds(50)) != nullptr) {
      std::string note = "answers";
      if (const auto* v = f.try_ask({0x22, 0xF1, 0x89}, p2 + milliseconds(50))) {
        note = ascii(v->data, 2);
      }
      Flasher::ok("bootloader", note);
      return true;
    }
    f.idle(milliseconds(100));
  }
  Flasher::print_failure("bootloader", "no answer to TesterPresent within 3 s");
  return false;
}

bool unlock(Flasher& f, std::uint32_t secret) {
  const auto* seed_response = f.ask("security access", {0x27, 0x01});
  if (seed_response == nullptr) {
    return false;
  }
  if (seed_response->data.size() < 5) {
    Flasher::print_failure("security access", "the seed is not four bytes");
    return false;
  }
  const std::uint32_t seed =
      (static_cast<std::uint32_t>(seed_response->data[1]) << 24u) |
      (static_cast<std::uint32_t>(seed_response->data[2]) << 16u) |
      (static_cast<std::uint32_t>(seed_response->data[3]) << 8u) |
      static_cast<std::uint32_t>(seed_response->data[4]);
  if (seed != 0) {  // all zeros: already unlocked
    const std::uint32_t key = sim::toy_key_from_seed(seed, secret);
    Bytes request = {0x27, 0x02};
    push_be32(request, key);
    if (f.ask("security access", std::move(request)) == nullptr) {
      return false;
    }
  }
  Flasher::ok("security access");
  return true;
}

/// Steps 9-11: download one slot file. Prints progress every 10 %.
bool download(Flasher& f, const SlotFile& slot, const std::string& slot_name) {
  const auto size = static_cast<std::uint32_t>(slot.data.size());
  Bytes erase = {0x31, 0x01, 0xFF, 0x00, kAddrLenFormat};
  push_be32(erase, slot.base);
  push_be32(erase, size);
  const auto* erased = f.ask("erase slot " + slot_name, std::move(erase));
  if (erased == nullptr) {
    return false;
  }
  Flasher::ok("erase slot " + slot_name, pending_note(*erased));

  Bytes request = {0x34, 0x00, kAddrLenFormat};
  push_be32(request, slot.base);
  push_be32(request, size);
  const auto* accepted = f.ask("request download", std::move(request));
  if (accepted == nullptr) {
    return false;
  }
  // lengthFormatIdentifier: the high nibble says how many bytes the length has.
  const std::size_t n =
      accepted->data.empty() ? 0u : static_cast<std::size_t>(accepted->data[0] >> 4u);
  if (n == 0 || n > 4 || accepted->data.size() < 1 + n) {
    Flasher::print_failure("request download", "no usable maxNumberOfBlockLength");
    return false;
  }
  std::uint32_t max_block = 0;
  for (std::size_t i = 0; i < n; ++i) {
    max_block = (max_block << 8u) | static_cast<std::uint32_t>(accepted->data[1 + i]);
  }
  if (max_block <= 2) {
    Flasher::print_failure("request download", "maxNumberOfBlockLength too small");
    return false;
  }
  const std::uint32_t chunk = max_block - 2;  // SID and block counter
  Flasher::ok("request download", "blocks of " + std::to_string(chunk) + " bytes");

  const std::uint64_t start = f.now_ns();
  std::uint8_t bsc = 1;
  std::uint32_t sent = 0;
  std::uint32_t blocks = 0;
  std::uint32_t resent = 0;
  unsigned next_report = 10;
  while (sent < size) {
    const std::uint32_t len = std::min(chunk, size - sent);
    const auto from = slot.data.begin() + static_cast<std::ptrdiff_t>(sent);
    const std::string label = "transfer block " + std::to_string(blocks + 1);
    // A block whose answer does not come is sent again, with the same counter
    // and data: ISO 14229-1 lets a server take a repeated block without
    // writing it twice. A frame of the block may have been lost on the way in
    // (the ECU then drops the block), or the answer on the way out (the ECU
    // has written it). A negative answer still ends the run.
    const uds::Response* r = nullptr;
    bool wrong_block = false;
    for (unsigned attempt = 1; attempt <= kBlockTries; ++attempt) {
      Bytes block = {0x36, bsc};
      block.insert(block.end(), from, from + static_cast<std::ptrdiff_t>(len));
      bool silent = false;
      r = f.ask_block(label, std::move(block), silent);
      if (r == nullptr && !silent) {
        return false;  // negative, or the bus failed: already printed
      }
      // An answer naming another block is a late one to an earlier send.
      wrong_block = r != nullptr && (r->data.empty() || r->data[0] != bsc);
      if (r != nullptr && !wrong_block) {
        break;
      }
      r = nullptr;
      if (attempt < kBlockTries) {
        std::printf("%-22s no answer, sent again (try %u of %u)\n", label.c_str(),
                    attempt + 1, kBlockTries);
        std::fflush(stdout);
        ++resent;
        // Long enough for the ECU to give up a half-received block (its N_Cr)
        // and for a late answer to arrive; the client ignores it meanwhile.
        f.idle(kResendPause);
      }
    }
    if (r == nullptr) {
      Flasher::print_failure(label, wrong_block
                                        ? std::string("the answer names another block")
                                        : "the server did not answer within P2, " +
                                              std::to_string(kBlockTries) + " times");
      return false;
    }
    sent += len;
    ++blocks;
    ++bsc;  // wraps 0xFF -> 0x00
    const unsigned percent = static_cast<unsigned>((100ull * sent) / size);
    while (percent >= next_report && next_report <= 100) {
      std::printf("%-22s %3u%%  %u of %u bytes\n", "transfer", next_report, sent, size);
      std::fflush(stdout);
      next_report += 10;
    }
  }
  const double took = seconds_between(start, f.now_ns());
  char note[96];
  std::snprintf(note, sizeof(note), "%u bytes in %u blocks, %.1f s, %.1f KB/s", size,
                blocks, took,
                took > 0 ? static_cast<double>(size) / 1024.0 / took : 0.0);
  std::string summary = note;
  if (resent != 0) {
    summary += ", " + std::to_string(resent) + (resent == 1 ? " block" : " blocks") +
               " sent again";
  }
  Flasher::ok("transfer", summary);

  if (f.ask("transfer exit", {0x37}) == nullptr) {
    return false;
  }
  Flasher::ok("transfer exit");
  return true;
}

}  // namespace

int run_flash(const FlashOptions& options) {
  Package pkg;
  if (options.package.empty()) {
    std::fprintf(stderr, "canforge flash: --package <folder> is required\n");
    return 2;
  }
  if (!load_package(options.package, pkg)) {
    return 2;
  }

  transport::SocketCanBus bus(options.bus);
  if (const auto opened = bus.open(); !opened) {
    std::fprintf(stderr, "canforge flash: cannot open %s: %.*s\n", options.bus.c_str(),
                 static_cast<int>(opened.error().message().size()),
                 opened.error().message().data());
    return 1;
  }
  uds::ClientConfig config;
  config.transport.address =
      isotp::Address::normal(core::CanId::standard(options.tx_id).value(),
                             core::CanId::standard(options.rx_id).value());
  // The ECU promises P2 = 50 ms. A tester allows itself a margin over that for
  // the network; against an ECU in a simulator, which can stall for tens of ms
  // right after it starts, a generous one.
  const milliseconds p2(options.p2_ms);
  config.p2_ns = static_cast<std::uint64_t>(options.p2_ms) * kMs;
  config.max_response_pending = 0;  // an erase may take as long as it takes
  uds::Client client(config);
  uds::BusLink link(bus, client);
  if (const auto filtered = link.install_filter(); !filtered) {
    std::fprintf(stderr, "canforge flash: cannot set the receive filter on %s\n",
                 options.bus.c_str());
    return 1;
  }
  Flasher f(client, link);
  const std::uint64_t t0 = f.now_ns();
  VcuStandby vcu(options.bus);

  std::printf("flash %s: version %s%s%s, slot files %zu / %zu bytes, on %s\n",
              pkg.dir.c_str(), pkg.version.c_str(), pkg.git.empty() ? "" : ", git ",
              pkg.git.c_str(), pkg.slots[0].data.size(), pkg.slots[1].data.size(),
              options.bus.c_str());
  std::fflush(stdout);

  if (options.vcu_standby && !vcu.start()) {
    return 1;
  }
  if (!enter_programming(f) || !wait_for_bootloader(f, p2)) {
    return 1;
  }

  // Which slot is free: the one that is not active.
  const auto* info = f.ask("slot info", {0x22, 0xF1, 0xA0});
  if (info == nullptr) {
    return 1;
  }
  if (info->data.size() < 2 + kSlotInfoLen || info->data[2] > 1) {
    Flasher::print_failure("slot info", "F1A0 is not the 12 bytes expected");
    return 1;
  }
  const int target = 1 - info->data[2];
  const char* names[2] = {"A", "B"};
  const SlotFile& slot = pkg.slots[target];
  char where[128];
  std::snprintf(where, sizeof(where), "active %s: %s goes to slot %s at 0x%08X",
                names[1 - target], slot.file.c_str(), names[target], slot.base);
  Flasher::ok("slot info", where);

  if (!unlock(f, options.secret) || !download(f, slot, names[target])) {
    return 1;
  }

  const auto* checked = f.ask("check image", {0x31, 0x01, 0xFF, 0x01});
  if (checked == nullptr) {
    return 1;
  }
  const std::uint8_t result = checked->data.size() >= 4 ? checked->data[3] : 0xFF;
  const bool accepted = result == 0;
  if (accepted) {
    Flasher::ok("check image", "valid; it runs on trial after the restart");
  } else {
    std::printf("%-22s REFUSED: %s (%u)\n", "check image",
                image_result_name(result).c_str(), result);
  }

  // Restart either way: a refused image leaves the ECU on the release it had.
  if (f.ask("reset", {0x11, 0x01}) == nullptr) {
    return 1;
  }
  Flasher::ok("reset");
  if (!accepted || options.expect_reject) {
    if (options.expect_reject && accepted) {
      Flasher::print_failure("expect reject",
                             "the ECU accepted an image it should have refused");
      return 1;
    }
    return options.expect_reject ? 0 : 1;
  }

  f.idle(kStartupWait);
  const auto* version = f.ask("version", {0x22, 0xF1, 0x89});
  if (version == nullptr) {
    return 1;
  }
  const std::string running = ascii(version->data, 2);
  if (running != pkg.version) {
    Flasher::print_failure("version",
                           "the ECU runs " + running + ", not " + pkg.version);
    return 1;
  }
  Flasher::ok("version", running);

  // The new image confirms itself once it has run healthy for long enough.
  const std::uint64_t start = f.now_ns();
  const std::uint64_t until =
      start + static_cast<std::uint64_t>(kConfirmWait.count()) * 1000 * kMs;
  while (f.now_ns() < until) {
    const auto* r = f.try_ask({0x22, 0xF1, 0xA0}, p2 + milliseconds(50));
    if (r != nullptr && r->data.size() >= 2 + kSlotInfoLen && r->data[2] == target &&
        r->data[2 + 9] == kTrialConfirmed) {
      char note[96];
      std::snprintf(note, sizeof(note),
                    "slot %s active and confirmed after %.1f s; %.1f s in all",
                    names[target], seconds_between(start, f.now_ns()),
                    seconds_between(t0, f.now_ns()));
      Flasher::ok("confirm", note);
      return 0;
    }
    f.idle(milliseconds(500));
  }
  Flasher::print_failure("confirm", "the new image did not confirm itself within 15 s");
  return 1;
}

}  // namespace canforge::cli
