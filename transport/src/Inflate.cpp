// SPDX-License-Identifier: MIT
#include "Inflate.hpp"

#include <algorithm>
#include <array>
#include <cstring>

namespace canforge::transport::detail {
namespace {

using core::Error;
using core::ErrorCode;
using core::Result;

Error bad(std::string_view why, std::uint32_t where = 0) {
  return Error(ErrorCode::LogBadFormat, why, {where, 0});
}

/// Canonical Huffman decoder, built from a list of code lengths as RFC 1951
/// section 3.2.2 describes. Decoding walks the code bit by bit, which is slow
/// compared with a table-driven decoder but is short, obviously correct, and
/// fast enough: a BLF container is a few hundred kilobytes.
class Huffman {
 public:
  static Result<Huffman> build(const std::uint8_t* lengths, std::size_t count) {
    Huffman h;
    h.counts_.fill(0);
    for (std::size_t i = 0; i < count; ++i) {
      if (lengths[i] > kMaxBits) {
        return bad("Huffman code length exceeds 15 bits");
      }
      ++h.counts_[lengths[i]];
    }
    h.counts_[0] = 0;

    // An over-subscribed or incomplete set means the stream is corrupt.
    int left = 1;
    for (std::size_t bits = 1; bits <= kMaxBits; ++bits) {
      left <<= 1;
      left -= static_cast<int>(h.counts_[bits]);
      if (left < 0) {
        return bad("over-subscribed Huffman code");
      }
    }

    std::array<std::uint16_t, kMaxBits + 1> offsets{};
    offsets[1] = 0;
    for (std::size_t bits = 1; bits < kMaxBits; ++bits) {
      offsets[bits + 1] = static_cast<std::uint16_t>(offsets[bits] + h.counts_[bits]);
    }
    h.symbols_.assign(count, 0);
    for (std::size_t i = 0; i < count; ++i) {
      if (lengths[i] != 0) {
        h.symbols_[offsets[lengths[i]]++] = static_cast<std::uint16_t>(i);
      }
    }
    return h;
  }

  const std::array<std::uint16_t, 16>& counts() const noexcept { return counts_; }
  const std::vector<std::uint16_t>& symbols() const noexcept { return symbols_; }

 private:
  static constexpr std::size_t kMaxBits = 15;
  std::array<std::uint16_t, 16> counts_{};
  std::vector<std::uint16_t> symbols_;
};

class BitReader {
 public:
  BitReader(const std::uint8_t* data, std::size_t size) : data_(data), size_(size) {}

  bool ok() const noexcept { return !overrun_; }
  std::size_t byte_position() const noexcept { return pos_; }

  std::uint32_t bits(std::uint32_t count) noexcept {
    std::uint32_t value = 0;
    for (std::uint32_t i = 0; i < count; ++i) {
      if (held_ == 0) {
        if (pos_ >= size_) {
          overrun_ = true;
          return value;
        }
        buffer_ = data_[pos_++];
        held_ = 8;
      }
      value |= static_cast<std::uint32_t>(buffer_ & 1u) << i;
      buffer_ = static_cast<std::uint8_t>(buffer_ >> 1u);
      --held_;
    }
    return value;
  }

  void align_to_byte() noexcept {
    held_ = 0;
    buffer_ = 0;
  }

  bool copy_bytes(std::vector<std::uint8_t>& out, std::size_t n) {
    if (pos_ + n > size_) {
      overrun_ = true;
      return false;
    }
    out.insert(out.end(), data_ + pos_, data_ + pos_ + n);
    pos_ += n;
    return true;
  }

  std::uint16_t read_u16_le() noexcept {
    if (pos_ + 2 > size_) {
      overrun_ = true;
      return 0;
    }
    const auto lo = static_cast<std::uint16_t>(data_[pos_]);
    const auto hi = static_cast<std::uint16_t>(data_[pos_ + 1]);
    pos_ += 2;
    return static_cast<std::uint16_t>(lo | (hi << 8u));
  }

  /// Walk the canonical code one bit at a time (RFC 1951 makes codes
  /// prefix-free, so this terminates as soon as a valid code is complete).
  int decode(const Huffman& table) noexcept {
    int code = 0;
    int first = 0;
    int index = 0;
    for (std::size_t length = 1; length <= 15; ++length) {
      code |= static_cast<int>(bits(1));
      if (overrun_) {
        return -1;
      }
      const int count = static_cast<int>(table.counts()[length]);
      if (code - first < count) {
        const auto off = static_cast<std::size_t>(code - first);
        const auto at = static_cast<std::size_t>(index) + off;
        if (at >= table.symbols().size()) {
          return -1;
        }
        return table.symbols()[at];
      }
      index += count;
      first = (first + count) << 1;
      code <<= 1;
    }
    return -1;
  }

 private:
  const std::uint8_t* data_;
  std::size_t size_;
  std::size_t pos_ = 0;
  std::uint8_t buffer_ = 0;
  std::uint32_t held_ = 0;
  bool overrun_ = false;
};

// RFC 1951 section 3.2.5.
constexpr std::uint16_t kLengthBase[29] = {3,  4,  5,  6,   7,   8,   9,   10,  11, 13,
                                           15, 17, 19, 23,  27,  31,  35,  43,  51, 59,
                                           67, 83, 99, 115, 131, 163, 195, 227, 258};
constexpr std::uint8_t kLengthExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                           2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
constexpr std::uint16_t kDistanceBase[30] = {
    1,    2,    3,    4,    5,    7,    9,    13,    17,    25,
    33,   49,   65,   97,   129,  193,  257,  385,   513,   769,
    1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
constexpr std::uint8_t kDistanceExtra[30] = {0, 0, 0,  0,  1,  1,  2,  2,  3,  3,
                                             4, 4, 5,  5,  6,  6,  7,  7,  8,  8,
                                             9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

Result<std::vector<std::uint8_t>> fixed_tables(Huffman& literals, Huffman& distances) {
  std::array<std::uint8_t, 288> lit{};
  for (std::size_t i = 0; i < 144; ++i) lit[i] = 8;
  for (std::size_t i = 144; i < 256; ++i) lit[i] = 9;
  for (std::size_t i = 256; i < 280; ++i) lit[i] = 7;
  for (std::size_t i = 280; i < 288; ++i) lit[i] = 8;
  auto l = Huffman::build(lit.data(), lit.size());
  if (!l) {
    return l.error();
  }
  literals = std::move(l).value();

  std::array<std::uint8_t, 30> dist{};
  dist.fill(5);
  auto d = Huffman::build(dist.data(), dist.size());
  if (!d) {
    return d.error();
  }
  distances = std::move(d).value();
  return std::vector<std::uint8_t>{};
}

core::Status dynamic_tables(BitReader& in, Huffman& literals, Huffman& distances) {
  const std::uint32_t hlit = in.bits(5) + 257u;
  const std::uint32_t hdist = in.bits(5) + 1u;
  const std::uint32_t hclen = in.bits(4) + 4u;
  if (!in.ok() || hlit > 286u || hdist > 30u) {
    return bad("malformed dynamic block header");
  }

  static constexpr std::uint8_t kOrder[19] = {16, 17, 18, 0, 8,  7, 9,  6, 10, 5,
                                              11, 4,  12, 3, 13, 2, 14, 1, 15};
  std::array<std::uint8_t, 19> code_lengths{};
  for (std::uint32_t i = 0; i < hclen; ++i) {
    code_lengths[kOrder[i]] = static_cast<std::uint8_t>(in.bits(3));
  }
  if (!in.ok()) {
    return bad("truncated code length alphabet");
  }
  auto cl = Huffman::build(code_lengths.data(), code_lengths.size());
  if (!cl) {
    return cl.error();
  }

  std::vector<std::uint8_t> lengths(hlit + hdist, 0);
  std::size_t written = 0;
  while (written < lengths.size()) {
    const int symbol = in.decode(cl.value());
    if (symbol < 0) {
      return bad("bad code length symbol");
    }
    if (symbol < 16) {
      lengths[written++] = static_cast<std::uint8_t>(symbol);
      continue;
    }
    std::uint8_t value = 0;
    std::uint32_t repeat = 0;
    if (symbol == 16) {
      if (written == 0) {
        return bad("repeat code at the start of the length list");
      }
      value = lengths[written - 1];
      repeat = 3u + in.bits(2);
    } else if (symbol == 17) {
      repeat = 3u + in.bits(3);
    } else {
      repeat = 11u + in.bits(7);
    }
    if (!in.ok() || written + repeat > lengths.size()) {
      return bad("code length repeat runs past the end of the list");
    }
    for (std::uint32_t i = 0; i < repeat; ++i) {
      lengths[written++] = value;
    }
  }

  auto l = Huffman::build(lengths.data(), hlit);
  if (!l) {
    return l.error();
  }
  auto d = Huffman::build(lengths.data() + hlit, hdist);
  if (!d) {
    return d.error();
  }
  literals = std::move(l).value();
  distances = std::move(d).value();
  return core::ok();
}

}  // namespace

std::uint32_t adler32(const std::uint8_t* data, std::size_t size) noexcept {
  std::uint32_t a = 1;
  std::uint32_t b = 0;
  constexpr std::uint32_t kMod = 65521u;
  for (std::size_t i = 0; i < size; ++i) {
    a = (a + data[i]) % kMod;
    b = (b + a) % kMod;
  }
  return (b << 16u) | a;
}

Result<std::vector<std::uint8_t>> inflate_raw(const std::uint8_t* data,
                                              std::size_t size,
                                              std::size_t expected_size) {
  std::vector<std::uint8_t> out;
  // The hint comes from the file being parsed, so it is not trusted: clamp it
  // rather than reserving whatever a four-byte field asks for.
  const std::size_t hint = expected_size != 0 ? expected_size : size * 4u;
  out.reserve(std::min(hint, std::size_t{1024} * 1024));

  BitReader in(data, size);
  bool final_block = false;
  while (!final_block) {
    final_block = in.bits(1) != 0;
    const std::uint32_t type = in.bits(2);
    if (!in.ok()) {
      return bad("truncated block header");
    }

    if (type == 0) {  // stored
      in.align_to_byte();
      const std::uint16_t length = in.read_u16_le();
      const std::uint16_t inverse = in.read_u16_le();
      if (!in.ok()) {
        return bad("truncated stored block header");
      }
      if (static_cast<std::uint16_t>(~length & 0xFFFFu) != inverse) {
        return bad("stored block length does not match its complement");
      }
      if (out.size() + length > kMaxInflateOutput) {
        return bad("decompressed output exceeds the size limit");
      }
      if (!in.copy_bytes(out, length)) {
        return bad("stored block runs past the end of the input");
      }
      continue;
    }
    if (type == 3) {
      return bad("reserved block type");
    }

    Huffman literals;
    Huffman distances;
    if (type == 1) {
      auto st = fixed_tables(literals, distances);
      if (!st) {
        return st.error();
      }
    } else {
      const core::Status st = dynamic_tables(in, literals, distances);
      if (!st) {
        return st.error();
      }
    }

    for (;;) {
      const int symbol = in.decode(literals);
      if (symbol < 0) {
        return bad("bad literal or length code");
      }
      if (out.size() >= kMaxInflateOutput) {
        return bad("decompressed output exceeds the size limit");
      }
      if (symbol < 256) {
        out.push_back(static_cast<std::uint8_t>(symbol));
        continue;
      }
      if (symbol == 256) {
        break;  // end of block
      }
      const auto index = static_cast<std::size_t>(symbol - 257);
      if (index >= 29) {
        return bad("length code out of range");
      }
      const std::size_t length = kLengthBase[index] + in.bits(kLengthExtra[index]);

      const int dist_symbol = in.decode(distances);
      if (dist_symbol < 0 || dist_symbol >= 30) {
        return bad("bad distance code");
      }
      const auto dist_index = static_cast<std::size_t>(dist_symbol);
      const std::size_t distance =
          kDistanceBase[dist_index] + in.bits(kDistanceExtra[dist_index]);
      if (!in.ok()) {
        return bad("truncated back reference");
      }
      if (distance == 0 || distance > out.size()) {
        return bad("back reference points before the start of the output");
      }
      // Copies may overlap: a distance of 1 with a length of 100 repeats one
      // byte, which is the whole point of the encoding, so this is a byte
      // loop rather than a memcpy.
      if (out.size() + length > kMaxInflateOutput) {
        return bad("decompressed output exceeds the size limit");
      }
      const std::size_t from = out.size() - distance;
      for (std::size_t i = 0; i < length; ++i) {
        out.push_back(out[from + i]);
      }
    }
  }
  return out;
}

Result<std::vector<std::uint8_t>> inflate_zlib(const std::uint8_t* data,
                                               std::size_t size,
                                               std::size_t expected_size) {
  if (size < 6) {
    return bad("zlib stream is too short to contain a header and a checksum");
  }
  const std::uint8_t cmf = data[0];
  const std::uint8_t flg = data[1];
  if ((cmf & 0x0Fu) != 8u) {
    return bad("zlib compression method is not deflate", cmf);
  }
  if (((static_cast<std::uint32_t>(cmf) << 8u) | flg) % 31u != 0u) {
    return bad("zlib header check bits are wrong");
  }
  if ((flg & 0x20u) != 0u) {
    return bad("zlib preset dictionaries are not supported");
  }

  auto out = inflate_raw(data + 2, size - 6, expected_size);
  if (!out) {
    return out;
  }
  const std::uint8_t* tail = data + size - 4;
  const std::uint32_t stored = (static_cast<std::uint32_t>(tail[0]) << 24u) |
                               (static_cast<std::uint32_t>(tail[1]) << 16u) |
                               (static_cast<std::uint32_t>(tail[2]) << 8u) |
                               static_cast<std::uint32_t>(tail[3]);
  const std::uint32_t actual = adler32(out.value().data(), out.value().size());
  if (stored != actual) {
    return bad("zlib checksum mismatch: the container is corrupt");
  }
  return out;
}

}  // namespace canforge::transport::detail
