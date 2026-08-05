// SPDX-License-Identifier: MIT
//
// Single-threaded throughput of the signal codec, reported in signals per
// second. No dependency on a benchmark framework: the measurement is a
// straight loop over a realistic signal set with the result accumulated into a
// sink the optimiser cannot discard.
//
// The signal set is modelled on a powertrain message mix: mostly 8 and 16 bit
// signals, a few odd widths, both byte orders, some scaled and some raw.

#include "canforge/core/Frame.hpp"
#include "canforge/core/Signal.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace {

using canforge::core::ByteOrder;
using canforge::core::CanId;
using canforge::core::Frame;
using canforge::core::SignalLayout;
using canforge::core::Signedness;

SignalLayout mk(std::uint16_t start, std::uint8_t len, ByteOrder order, Signedness sign,
                double factor, double offset) {
  SignalLayout s;
  s.start_bit = start;
  s.bit_length = len;
  s.byte_order = order;
  s.signedness = sign;
  s.factor = factor;
  s.offset = offset;
  return s;
}

const std::vector<SignalLayout>& signal_mix() {
  static const std::vector<SignalLayout> mix = {
      mk(0, 8, ByteOrder::Intel, Signedness::Unsigned, 1.0, -40.0),
      mk(8, 16, ByteOrder::Intel, Signedness::Unsigned, 0.125, 0.0),
      mk(24, 16, ByteOrder::Intel, Signedness::Signed, 0.1, 0.0),
      mk(40, 4, ByteOrder::Intel, Signedness::Unsigned, 1.0, 0.0),
      mk(44, 12, ByteOrder::Intel, Signedness::Unsigned, 0.05, 0.0),
      mk(7, 16, ByteOrder::Motorola, Signedness::Unsigned, 0.125, 0.0),
      mk(23, 10, ByteOrder::Motorola, Signedness::Signed, 1.0, 0.0),
      mk(29, 6, ByteOrder::Motorola, Signedness::Unsigned, 1.0, 0.0),
      mk(39, 16, ByteOrder::Motorola, Signedness::Unsigned, 1.0 / 256.0, 0.0),
      mk(55, 8, ByteOrder::Motorola, Signedness::Unsigned, 0.4, 0.0),
      mk(28, 8, ByteOrder::Intel, Signedness::Unsigned, 1.0, 0.0),  // 32-bit boundary
      mk(31, 16, ByteOrder::Motorola, Signedness::Unsigned, 1.0, 0.0),
  };
  return mix;
}

struct Measurement {
  double seconds;
  double per_second;
};

template <typename Body>
Measurement measure(std::size_t operations, Body&& body) {
  const auto start = std::chrono::steady_clock::now();
  body();
  const auto end = std::chrono::steady_clock::now();
  const double seconds = std::chrono::duration<double>(end - start).count();
  return {seconds, static_cast<double>(operations) / seconds};
}

void report(const char* label, const Measurement& m, std::size_t ops) {
  std::printf("  %-38s %10.2f M/s   (%zu ops in %.3f s, %.2f ns/op)\n", label,
              m.per_second / 1e6, ops, m.seconds,
              m.seconds / static_cast<double>(ops) * 1e9);
}

}  // namespace

int main() {
  const auto& mix = signal_mix();
  const auto id = CanId::make_standard<0x123>();

  // A block of distinct frames, so the loop is not decoding one hot payload
  // that stays in a register.
  constexpr std::size_t kFrames = 4096;
  std::vector<Frame> frames;
  frames.reserve(kFrames);
  for (std::size_t i = 0; i < kFrames; ++i) {
    std::array<std::uint8_t, 8> payload{};
    for (std::size_t b = 0; b < payload.size(); ++b) {
      payload[b] = static_cast<std::uint8_t>((i * 31u + b * 17u + 7u) & 0xFFu);
    }
    frames.push_back(Frame::make(id, payload.data(), payload.size()).value());
  }

  constexpr std::size_t kRounds = 2000;
  const std::size_t decode_ops = kRounds * kFrames * mix.size();

  std::printf("canforge signal codec benchmark (single threaded)\n");
  std::printf("  %zu signals x %zu frames x %zu rounds\n\n", mix.size(), kFrames,
              kRounds);

  std::uint64_t raw_sink = 0;
  const Measurement raw = measure(decode_ops, [&] {
    for (std::size_t r = 0; r < kRounds; ++r) {
      for (const Frame& f : frames) {
        for (const SignalLayout& s : mix) {
          raw_sink = raw_sink * 1000003u + s.decode_raw(f.data());
        }
      }
    }
  });
  report("decode_raw (bit extraction only)", raw, decode_ops);

  double sink = 0.0;
  const Measurement full = measure(decode_ops, [&] {
    for (std::size_t r = 0; r < kRounds; ++r) {
      for (const Frame& f : frames) {
        for (const SignalLayout& s : mix) {
          sink += s.decode(f.data());
        }
      }
    }
  });
  report("decode (extraction + scaling)", full, decode_ops);

  constexpr std::size_t kRefRounds = 100;
  const std::size_t ref_ops = kRefRounds * kFrames * mix.size();
  std::uint64_t ref_sink = 0;
  const Measurement ref = measure(ref_ops, [&] {
    for (std::size_t r = 0; r < kRefRounds; ++r) {
      for (const Frame& f : frames) {
        for (const SignalLayout& s : mix) {
          ref_sink = ref_sink * 1000003u + s.decode_raw_reference(f.data());
        }
      }
    }
  });
  report("decode_raw_reference (bit at a time)", ref, ref_ops);

  std::vector<Frame> scratch = frames;
  const std::size_t encode_ops = kRounds * kFrames * mix.size();
  const Measurement enc = measure(encode_ops, [&] {
    for (std::size_t r = 0; r < kRounds; ++r) {
      for (Frame& f : scratch) {
        for (const SignalLayout& s : mix) {
          s.encode_raw(static_cast<std::uint64_t>(r), f.data());
        }
      }
    }
  });
  report("encode_raw (bit insertion only)", enc, encode_ops);

  std::printf("\n  speedup of the word-at-a-time path over the reference: %.1fx\n",
              raw.per_second / ref.per_second);
  std::printf("  checksum %llu %.6f %llu\n",
              static_cast<unsigned long long>(raw_sink + ref_sink), sink,
              static_cast<unsigned long long>(scratch[0].data()[0]));
  return 0;
}
