// SPDX-License-Identifier: MIT
#ifndef CANFORGE_CLI_FLASH_HPP
#define CANFORGE_CLI_FLASH_HPP

/// `canforge flash`: updates an ECU with an A/B bootloader over UDS on CAN.
///
/// The package is a folder written by the ECU project's release tooling:
///
///   btms-1.1.0/manifest.json   version, git hash, and per slot its file and base
///   btms-1.1.0/slotA.bin       the release linked for slot A, image header first
///   btms-1.1.0/slotB.bin       the same release linked for slot B
///
/// The ECU never copies an image: it runs from one slot while the other is
/// written. So the tool asks which slot is active and sends the file built for
/// the other one. The sequence, first in the application, then in the bootloader:
///
///   10 03, 85 02, 28 03 01  extended session, DTC setting off, cyclic messages off
///   10 02                   programming: the ECU restarts into its bootloader.
///                           If refused: 10 01 (DTC setting and messages back
///                           on), and the run ends with the NRC's name
///   3E 00 ...               until the bootloader answers (at most 3 s)
///   22 F189, 22 F1A0        bootloader version; which slot is active
///   27 01 / 27 02           seed and key
///   31 01 FF00 44 a s       erase the free slot (responsePending meanwhile)
///   34 00 44 a s            request download; block length from the answer
///   36 n <block> ...        transfer; n from 1, wrapping 0xFF -> 0x00. A
///                           block left unanswered is sent again (at most
///                           three sends, 1.1 s apart), same n, same data
///   37                      transfer exit
///   31 01 FF01              check the image; the answer carries the result
///   11 01                   restart: the new image runs on trial (also after
///                           a refused image, so the old one runs again)
///   22 F189, 22 F1A0        new version running, then confirmed (up to 15 s)
///
/// Any negative response ends the run with the NRC's name.
///
/// With --vcu-standby the tool also plays the vehicle controller for as long as
/// it runs: the BTMS ECU enters programming only while the vehicle is parked.
/// It sends VCU_Cmd (0x100) every 100 ms with target 30 degC and cooling mode
/// STANDBY, alive counter and CRC-8 SAE J1850 as the btms.dbc describes them,
/// starting 300 ms before 10 02. No other node may send 0x100 meanwhile (stop
/// `canforge sim` first): the tool checks and refuses if one does.

#include <cstdint>
#include <string>

namespace canforge::cli {

struct FlashOptions {
  std::string bus = "vcan0";
  std::string package;                 ///< folder with manifest.json
  std::uint32_t secret = 0xA5C3F00Du;  ///< seed/key secret, as in canforge's UDS server
  std::uint32_t tx_id = 0x7E0;
  std::uint32_t rx_id = 0x7E8;
  std::uint32_t p2_ms = 500;   ///< see Flash.cpp: a simulated ECU needs a margin
  bool expect_reject = false;  ///< success means the ECU refused the image
  bool vcu_standby = false;    ///< send VCU_Cmd STANDBY throughout (see above)
};

/// Runs the whole update and prints one line per step. Returns the process
/// exit code: 0 on success (with --expect-reject: the image was refused), 1 on
/// any failure, 2 for bad options or an unreadable package.
int run_flash(const FlashOptions& options);

}  // namespace canforge::cli

#endif  // CANFORGE_CLI_FLASH_HPP
