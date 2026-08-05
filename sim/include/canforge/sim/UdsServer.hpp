// SPDX-License-Identifier: MIT
#ifndef CANFORGE_SIM_UDSSERVER_HPP
#define CANFORGE_SIM_UDSSERVER_HPP

/// A simulated UDS server, so the diagnostic client has an ECU to talk to with
/// no hardware attached.
///
/// It implements enough of ISO 14229 to demonstrate a complete firmware
/// download: session control, security access, a DTC store, and the
/// RequestDownload / TransferData / RequestTransferExit sequence with a
/// checksum verified afterwards through RoutineControl.
///
/// The seed/key algorithm is a toy and is labelled as one at its definition.
/// Real ones are supplier secrets and usually no stronger, which is a separate
/// problem; nothing here should be mistaken for security.

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "canforge/isotp/IsoTp.hpp"
#include "canforge/uds/Uds.hpp"

namespace canforge::sim {

/// The deliberately weak seed/key transform. Documented, reproducible, and not
/// cryptography: rotate left by three and exclusive-or with a constant.
std::uint32_t toy_key_from_seed(std::uint32_t seed, std::uint32_t secret) noexcept;

struct UdsServerConfig {
  isotp::Config transport;
  std::uint32_t security_secret = 0xA5C3F00Du;
  /// Bytes of flashable memory the server pretends to have.
  std::uint32_t flash_size = 64 * 1024;
  std::uint32_t flash_base = 0x08000000u;
  /// How many TransferData blocks to answer with NRC 0x78 before the real
  /// answer, so a client's responsePending handling is actually exercised.
  std::uint32_t pending_responses_per_block = 0;
  /// Simulated erase time reported through responsePending.
  std::uint64_t erase_duration_ns = 0;
  /// Data identifiers the server knows about.
  std::map<std::uint16_t, std::vector<std::uint8_t>> data_by_identifier;
  std::vector<uds::Dtc> dtcs;
};

/// Build a server preloaded with a plausible VIN, a few DTCs and an empty
/// flash region -- enough for a demo without any configuration.
UdsServerConfig default_server_config(isotp::Config transport);

class UdsServer {
 public:
  explicit UdsServer(UdsServerConfig config);

  void on_frame(const core::FdFrame& frame, std::uint64_t now_ns);
  std::vector<core::FdFrame> poll(std::uint64_t now_ns);

  uds::SessionType session() const noexcept { return session_; }
  bool unlocked() const noexcept { return unlocked_; }
  const std::vector<std::uint8_t>& flash() const noexcept { return flash_; }
  const std::vector<uds::Dtc>& dtcs() const noexcept { return dtcs_; }
  std::uint64_t requests_handled() const noexcept { return handled_; }
  std::uint64_t tester_present_seen() const noexcept { return tester_present_; }
  /// True once RequestTransferExit has been accepted.
  bool download_complete() const noexcept { return download_complete_; }

  /// Set a DTC, as a fault would.
  void add_dtc(uds::Dtc dtc) { dtcs_.push_back(std::move(dtc)); }

 private:
  std::vector<std::uint8_t> handle(const std::vector<std::uint8_t>& request,
                                   std::uint64_t now_ns);
  std::vector<std::uint8_t> negative(std::uint8_t service, uds::Nrc nrc) const;

  UdsServerConfig config_;
  isotp::Receiver receiver_;
  isotp::Sender sender_;
  bool sending_ = false;

  uds::SessionType session_ = uds::SessionType::Default;
  bool unlocked_ = false;
  std::uint32_t last_seed_ = 0;
  std::uint8_t security_level_ = 0;

  std::vector<std::uint8_t> flash_;
  bool download_active_ = false;
  bool download_complete_ = false;
  std::uint32_t download_address_ = 0;
  std::uint32_t download_size_ = 0;
  std::uint32_t download_written_ = 0;
  std::uint8_t expected_block_ = 1;
  std::uint32_t pending_left_ = 0;
  std::vector<std::uint8_t> deferred_response_;
  std::uint8_t deferred_service_ = 0;
  std::uint64_t deferred_until_ns_ = 0;

  std::vector<uds::Dtc> dtcs_;
  std::uint64_t handled_ = 0;
  std::uint64_t tester_present_ = 0;
};

}  // namespace canforge::sim

#endif  // CANFORGE_SIM_UDSSERVER_HPP
