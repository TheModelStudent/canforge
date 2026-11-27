# Changelog

All notable changes to this project are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/); versions are `MAJOR.MINOR.PATCH`.

Versions 0.2.0 to 0.6.0 were made for [btms-ecu](https://github.com/TheModelStudent/btms-ecu),
which uses canforge as its code generator, vehicle controller, dashboard, diagnostic
tester and flashing tool.

## [Unreleased]

## [0.6.0] - 2026-11-16

The first tagged release. It publishes versions 0.2.0 to 0.6.0 together (`gen-c`,
`BusLink`, `diag`, the DID files and the JSON reader, `flash`), with 434 tests in all.

### Added

- `flash --vcu-standby`: sends the BTMS `VCU_Cmd` (0x100) with cooling mode STANDBY,
  alive counter and CRC-8 every 100 ms from 300 ms before the programming request until
  the update ends, on its own socket and thread. The BTMS ECU programs only while the
  vehicle is parked. Refuses to start while another node sends 0x100.

### Changed

- `flash`: when the ECU refuses the programming session, the tool returns it to the
  default session before it ends, which switches DTC setting and the cyclic messages
  back on. Before, they stayed off until the session timed out.

### Documentation

- README: "Used in btms-ecu", what each part of canforge does for the ECU it was built
  next to.
- README and `docs/dashboard.tape`: the sample commands pass
  `--database dbc/tests/data/powertrain.dbc`. A config's `database` line is read
  relative to the config file, and `sim/tests/data/` has no `powertrain.dbc`; without
  the option they stopped at once with "cannot open the file" (CI already passed it).

## [0.5.0] - 2026-11-15

### Added

- `gen-c --raw`: every field an integer holding its signal's raw value, scaled or not,
  and no floating point, for 8-bit targets. Pack cuts a value to its signal's bits but
  does not clamp it to the database's `[min|max]`, so a raw marker outside it reaches
  the wire. Proved against the runtime codec like the scaled output (6 new tests).

### Changed

- `flash` sends a transfer block that gets no answer again, with the same block counter
  and data, at most three times and 1.1 s apart. A lost frame on the way in or a lost
  answer on the way out no longer ends an update; a negative answer still does.
- README: `flash` and `gen-c --raw`.

## [0.4.0] - 2026-11-15

### Added

- `flash`: the whole programming sequence for an ECU with an A/B bootloader, from a
  release package (`manifest.json` and one image per slot), with progress and
  throughput; `--expect-reject` for tests.

### Changed

- The JSON reader moved to the `text` library (`canforge/text/Json.hpp`); it reads the
  DID files and the package manifest. canforge still has no dependencies.

## [0.3.0] - 2026-11-14

### Added

- `uds::BusLink`: runs the poll-driven UDS client on any `IBus`.
- `diag`: sessions, read and write by identifier, read and clear DTCs, security access,
  reset and raw requests, carried out in the order given on the command line.
- `uds::DidCatalog` and `--did-file`: data identifiers described in JSON and decoded
  generically.
- 29 new tests.

## [0.2.0] - 2026-11-13

### Added

- `codegen/` and `gen-c`: a DBC database to C11 pack and unpack code, proved byte for
  byte against the runtime codec.

### Changed

- `dash` without `--virtual` listens on a real bus instead of running the configured
  simulation in-process.

## [0.1.0]

The codec, the DBC parser and writer, the virtual bus with arbitration, SocketCAN, log
formats, ISO-TP, the UDS client and a simulated UDS server, the vehicle simulator and
the dashboard, with 383 tests.
