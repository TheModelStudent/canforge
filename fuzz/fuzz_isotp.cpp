// SPDX-License-Identifier: MIT
#include <cstddef>
#include <cstdint>
#include <vector>

#include "canforge/isotp/IsoTp.hpp"

/// The ISO-TP receiver reassembles a message from frames a peer controls
/// entirely: lengths, sequence numbers and the escape header all come off the
/// wire. The fuzzer drives it with arbitrary frames and arbitrary timing.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  using namespace canforge;
  if (size < 2) {
    return 0;
  }

  isotp::Config config;
  config.address = isotp::Address::normal(core::CanId::standard(0x7E8).value(),
                                          core::CanId::standard(0x7E0).value());
  // The first two bytes steer the configuration, so the fuzzer explores
  // extended addressing and different block sizes, not one shape.
  if ((data[0] & 0x01u) != 0u) {
    config.address =
        isotp::Address::extended(core::CanId::standard(0x7E8).value(),
                                 core::CanId::standard(0x7E0).value(), 0x10, data[1]);
  }
  config.block_size = data[0] >> 1u;
  config.max_receive_length = 100000;

  isotp::Receiver receiver(config);
  std::uint64_t now = 0;

  std::size_t at = 2;
  while (at < size) {
    // One frame per chunk: a length byte then that many payload bytes.
    const std::size_t want = data[at] % 9u;
    ++at;
    const std::size_t take = std::min(want, size - at);
    std::array<std::uint8_t, 64> payload{};
    for (std::size_t i = 0; i < take; ++i) {
      payload[i] = data[at + i];
    }
    at += take;

    auto frame = core::FdFrame::make(config.address.rx_id, payload.data(), take,
                                     core::FrameFlags::None, now);
    if (frame) {
      receiver.on_frame(frame.value(), now);
      static_cast<void>(receiver.poll(now));
    }
    now += 1000000ULL;  // 1 ms between frames, so N_Cr eventually fires
    if (receiver.done()) {
      receiver.reset();
    }
  }
  return 0;
}
