// SPDX-License-Identifier: MIT
//
// Vector BLF reader.
//
// BLF was never published. What's here follows the layout in Vector's own
// binlog SDK headers and in python-can's reader, the two de-facto references.
// They don't agree everywhere; where they don't, there's a comment saying so and
// the code sniffs instead of picking a side.
//
// Structure:
//
//   file header    144 bytes, starting "LOGG"
//   objects        a sequence of "LOBJ" records
//
//   object header base   signature[4] "LOBJ", headerSize u16, headerVersion u16,
//                        objectSize u32, objectType u32                  (16 bytes)
//   object header v1     objectFlags u32, clientIndex u16, objectVersion u16,
//                        objectTimeStamp u64                             (16 more)
//
// Objects are padded to a four-byte boundary. Almost everything real is wrapped
// in LOG_CONTAINER objects (type 10) whose payload is a zlib stream holding yet
// more LOBJ records, so the reader recurses exactly one level. The inflate it
// needs is in Inflate.cpp, since the library layers can't take a zlib dependency.

#include <array>
#include <cstring>
#include <fstream>
#include <memory>
#include <vector>

#include "Inflate.hpp"
#include "canforge/transport/LogFormat.hpp"

namespace canforge::transport {
namespace {

using core::CanId;
using core::Error;
using core::ErrorCode;
using core::FdFrame;
using core::FrameFlags;

Error malformed(std::string_view why, std::uint32_t at = 0) {
  return Error(ErrorCode::LogBadFormat, why, {at, 0});
}

constexpr std::uint32_t kObjCanMessage = 1;
constexpr std::uint32_t kObjLogContainer = 10;
constexpr std::uint32_t kObjCanMessage2 = 86;
constexpr std::uint32_t kObjCanFdMessage = 100;
constexpr std::uint32_t kObjCanFdMessage64 = 101;

/// Object header flag values: they select the unit the timestamp is in.
constexpr std::uint32_t kTimeTenMics = 0x00000001u;
constexpr std::uint32_t kTimeOneNans = 0x00000002u;

std::uint16_t read_u16(const std::uint8_t* p) noexcept {
  return static_cast<std::uint16_t>(static_cast<std::uint16_t>(p[0]) |
                                    (static_cast<std::uint16_t>(p[1]) << 8u));
}
std::uint32_t read_u32(const std::uint8_t* p) noexcept {
  return static_cast<std::uint32_t>(p[0]) |
         (static_cast<std::uint32_t>(p[1]) << 8u) |
         (static_cast<std::uint32_t>(p[2]) << 16u) |
         (static_cast<std::uint32_t>(p[3]) << 24u);
}
std::uint64_t read_u64(const std::uint8_t* p) noexcept {
  return static_cast<std::uint64_t>(read_u32(p)) |
         (static_cast<std::uint64_t>(read_u32(p + 4)) << 32u);
}

struct ObjectHeader {
  std::uint16_t header_size = 0;
  std::uint16_t header_version = 0;
  std::uint32_t object_size = 0;
  std::uint32_t object_type = 0;
  std::uint32_t flags = 0;
  std::uint64_t timestamp = 0;
  std::size_t payload_offset = 0;
};

/// Convert an object timestamp to nanoseconds using the unit the flags select.
std::uint64_t timestamp_ns(const ObjectHeader& h) noexcept {
  if ((h.flags & kTimeOneNans) != 0u) {
    return h.timestamp;
  }
  if ((h.flags & kTimeTenMics) != 0u) {
    return h.timestamp * 10000ULL;
  }
  // Neither bit set happens in files written by older tools; ten microseconds
  // is the older default and the value that makes such files line up with
  // their ASC exports.
  return h.timestamp * 10000ULL;
}

/// The CAN_MESSAGE and CAN_MESSAGE2 payloads share their first 16 bytes:
///   channel u16, flags u8, dlc u8, id u32, data[8]
/// CAN_MESSAGE2 adds frame length and bit count fields that canforge ignores.
/// `flags` bit 0 is the transmit direction.
core::Result<LogRecord> parse_can_message(const ObjectHeader& h,
                                          const std::uint8_t* p, std::size_t n) {
  if (n < 16) {
    return malformed("truncated CAN_MESSAGE object");
  }
  const std::uint16_t channel = read_u16(p);
  const std::uint8_t flags = p[2];
  const std::uint8_t dlc = p[3];
  const std::uint32_t raw_id = read_u32(p + 4);

  const bool extended = (raw_id & 0x80000000u) != 0u;
  auto id = extended ? CanId::extended(raw_id & 0x1FFFFFFFu)
                     : CanId::standard(raw_id & 0x7FFu);
  if (!id) {
    return id.error();
  }

  FrameFlags frame_flags = FrameFlags::None;
  if ((flags & 0x80u) != 0u) {  // remote frame bit
    frame_flags |= FrameFlags::Rtr;
  }

  const std::size_t length = dlc > 8u ? std::size_t{8} : std::size_t{dlc};
  auto frame = FdFrame::make(id.value(), p + 8, length, frame_flags, timestamp_ns(h));
  if (!frame) {
    return frame.error();
  }
  LogRecord record;
  record.frame = frame.value();
  record.channel = "can" + std::to_string(channel);
  record.is_rx = (flags & 0x01u) == 0u;
  return record;
}

/// CAN_FD_MESSAGE:
///   channel u16, flags u8, dlc u8, id u32, frameLength u32, arbBitCount u8,
///   canFdFlags u8, validDataBytes u8, reserved u8, reserved u32, data[64]
core::Result<LogRecord> parse_can_fd_message(const ObjectHeader& h,
                                             const std::uint8_t* p, std::size_t n) {
  if (n < 24) {
    return malformed("truncated CAN_FD_MESSAGE object");
  }
  const std::uint16_t channel = read_u16(p);
  const std::uint8_t flags = p[2];
  const std::uint8_t dlc = p[3];
  const std::uint32_t raw_id = read_u32(p + 4);
  const std::uint8_t fd_flags = p[13];
  const std::uint8_t valid_bytes = p[14];

  const bool extended = (raw_id & 0x80000000u) != 0u;
  auto id = extended ? CanId::extended(raw_id & 0x1FFFFFFFu)
                     : CanId::standard(raw_id & 0x7FFu);
  if (!id) {
    return id.error();
  }

  FrameFlags frame_flags = FrameFlags::None;
  // canFdFlags: bit 0 EDL (this really is an FD frame), bit 1 BRS, bit 2 ESI.
  const bool edl = (fd_flags & 0x01u) != 0u;
  if (edl) {
    frame_flags |= FrameFlags::Fd;
    if ((fd_flags & 0x02u) != 0u) {
      frame_flags |= FrameFlags::Brs;
    }
    if ((fd_flags & 0x04u) != 0u) {
      frame_flags |= FrameFlags::Esi;
    }
  }

  std::size_t length = valid_bytes;
  if (length == 0 || length > 64u) {
    length = core::dlc_to_length(dlc, edl);
  }
  if (24u + length > n) {
    return malformed("CAN FD payload runs past the end of the object");
  }
  auto frame =
      FdFrame::make(id.value(), p + 24, length, frame_flags, timestamp_ns(h));
  if (!frame) {
    return frame.error();
  }
  LogRecord record;
  record.frame = frame.value();
  record.channel = "can" + std::to_string(channel);
  record.is_rx = (flags & 0x01u) == 0u;
  return record;
}

class BlfReader final : public LogReader {
 public:
  static core::Result<std::unique_ptr<LogReader>> open(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
      return Error(ErrorCode::ParseIoError, "cannot open the BLF file");
    }
    std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                                    std::istreambuf_iterator<char>());
    if (bytes.size() < 144) {
      return malformed("the file is shorter than a BLF header");
    }
    if (std::memcmp(bytes.data(), "LOGG", 4) != 0) {
      return malformed("missing the LOGG signature");
    }
    const std::uint32_t header_size = read_u32(bytes.data() + 4);
    if (header_size < 16 || header_size > bytes.size()) {
      return malformed("implausible BLF header size", header_size);
    }
    auto reader = std::unique_ptr<BlfReader>(new BlfReader());
    reader->bytes_ = std::move(bytes);
    reader->pos_ = header_size;
    return std::unique_ptr<LogReader>(std::move(reader));
  }

  core::Result<LogRecord> next() override {
    for (;;) {
      // Anything already unpacked from a container comes first.
      while (inner_pos_ < inner_.size()) {
        auto record = read_object(inner_.data(), inner_.size(), inner_pos_);
        if (record) {
          return record;
        }
        if (record.error().code() != ErrorCode::LogEndOfFile) {
          return record.error();
        }
        // LogEndOfFile from read_object means "not a frame object"; keep going.
      }
      inner_.clear();
      inner_pos_ = 0;

      if (pos_ >= bytes_.size()) {
        return Error(ErrorCode::LogEndOfFile, "end of BLF log");
      }
      auto record = read_object(bytes_.data(), bytes_.size(), pos_);
      if (record) {
        return record;
      }
      if (record.error().code() != ErrorCode::LogEndOfFile) {
        return record.error();
      }
    }
  }

 private:
  BlfReader() = default;

  core::Result<ObjectHeader> read_header(const std::uint8_t* data, std::size_t size,
                                         std::size_t at) const {
    if (at + 16 > size) {
      return malformed("truncated object header");
    }
    if (std::memcmp(data + at, "LOBJ", 4) != 0) {
      return malformed("missing the LOBJ signature",
                       static_cast<std::uint32_t>(at));
    }
    ObjectHeader h;
    h.header_size = read_u16(data + at + 4);
    h.header_version = read_u16(data + at + 6);
    h.object_size = read_u32(data + at + 8);
    h.object_type = read_u32(data + at + 12);
    if (h.object_size < 16 || at + h.object_size > size) {
      return malformed("object size runs past the end of the data",
                       h.object_size);
    }
    if (h.header_size >= 32 && at + 32 <= size) {
      h.flags = read_u32(data + at + 16);
      h.timestamp = read_u64(data + at + 24);
      h.payload_offset = at + h.header_size;
    } else {
      h.payload_offset = at + (h.header_size >= 16 ? h.header_size : 16u);
    }
    if (h.payload_offset > at + h.object_size) {
      return malformed("object header is larger than the object");
    }
    return h;
  }

  /// Reads one object at `cursor`, advancing it. Returns LogEndOfFile when the
  /// object was consumed but produced no frame (a container, or an object type
  /// canforge does not model).
  core::Result<LogRecord> read_object(const std::uint8_t* data, std::size_t size,
                                      std::size_t& cursor) {
    auto header = read_header(data, size, cursor);
    if (!header) {
      return header.error();
    }
    const ObjectHeader& h = header.value();
    const std::size_t next =
        cursor + ((h.object_size + 3u) & ~std::size_t{3});  // 4-byte aligned
    const std::uint8_t* payload = data + h.payload_offset;
    const std::size_t payload_size =
        cursor + h.object_size - h.payload_offset;
    cursor = next > size ? size : next;

    switch (h.object_type) {
      case kObjLogContainer: {
        auto st = unpack_container(data, cursor, h);
        if (!st) {
          return st.error();
        }
        return Error(ErrorCode::LogEndOfFile, "container consumed");
      }
      case kObjCanMessage:
      case kObjCanMessage2:
        return parse_can_message(h, payload, payload_size);
      case kObjCanFdMessage:
      case kObjCanFdMessage64:
        return parse_can_fd_message(h, payload, payload_size);
      default:
        // Ethernet, LIN, FlexRay, statistics and marker objects all live in
        // the same stream; skipping them is normal, not an error.
        return Error(ErrorCode::LogEndOfFile, "not a CAN object");
    }
  }

  core::Status unpack_container(const std::uint8_t* data, std::size_t after,
                                const ObjectHeader& h) {
    // The two references disagree about the container header: Vector's SDK
    // puts the compressed payload at offset 32 from the object start, while
    // python-can reads it at 28. Rather than pick one and be wrong half the
    // time, the compression method and uncompressed size are read from the
    // fixed positions both agree on, and the start of the zlib stream is found
    // by looking for its two-byte header, whose low nibble must be 8 (deflate)
    // and whose check bits must be consistent.
    const std::size_t object_start = after - ((h.object_size + 3u) & ~std::size_t{3});
    const std::size_t body = object_start + 16u;
    if (body + 12u > object_start + h.object_size) {
      return malformed("truncated log container header");
    }
    const std::uint16_t method = read_u16(data + body);
    std::uint32_t uncompressed_size = read_u32(data + body + 8);
    // Found by fuzzing: this field is whatever the file says, and it used to
    // be handed to the decompressor as an allocation hint unchecked.
    if (uncompressed_size > detail::kMaxInflateOutput) {
      return malformed("log container claims an implausible uncompressed size",
                       uncompressed_size);
    }
    const std::size_t object_end = object_start + h.object_size;

    std::size_t start = 0;
    for (const std::size_t candidate : {body + 12u, body + 16u}) {
      if (candidate + 2 > object_end) {
        continue;
      }
      if (method == 0) {
        // Uncompressed: the payload length is known exactly, so the right
        // offset is the one that makes the arithmetic come out.
        if (object_end - candidate == uncompressed_size) {
          start = candidate;
          break;
        }
        continue;
      }
      // Compressed: look for a plausible zlib header -- deflate method in the
      // low nibble and header check bits that divide by 31.
      const std::uint8_t cmf = data[candidate];
      const std::uint8_t flg = data[candidate + 1];
      if ((cmf & 0x0Fu) == 8u &&
          ((static_cast<std::uint32_t>(cmf) << 8u) | flg) % 31u == 0u) {
        start = candidate;
        break;
      }
    }
    if (start == 0) {
      return malformed("cannot locate the container payload");
    }

    const std::size_t compressed_size = object_end - start;
    if (method == 0) {
      inner_.assign(data + start, data + object_end);
    } else {
      auto out = detail::inflate_zlib(data + start, compressed_size,
                                      uncompressed_size);
      if (!out) {
        return out.error();
      }
      inner_ = std::move(out).value();
    }
    inner_pos_ = 0;
    return core::ok();
  }

  std::vector<std::uint8_t> bytes_;
  std::vector<std::uint8_t> inner_;
  std::size_t pos_ = 0;
  std::size_t inner_pos_ = 0;
};

}  // namespace

core::Result<std::unique_ptr<LogReader>> open_blf_reader(const std::string& path) {
  return BlfReader::open(path);
}

}  // namespace canforge::transport
