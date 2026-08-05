// SPDX-License-Identifier: MIT
#ifndef CANFORGE_TRANSPORT_INFLATE_HPP
#define CANFORGE_TRANSPORT_INFLATE_HPP

/// A self-contained DEFLATE (RFC 1951) and zlib (RFC 1950) decompressor.
///
/// Vector BLF log files store their objects inside zlib-compressed containers.
/// canforge is not permitted a third-party dependency in the library layers,
/// so rather than drop BLF support -- the format real automotive logs actually
/// arrive in -- the ~200 lines of inflate needed to read them are implemented
/// here. Only decompression is provided; nothing in canforge writes BLF.
///
/// This is decompression of untrusted input, so every table lookup and every
/// back-reference distance is bounds-checked, and a malformed stream returns
/// an error instead of reading out of bounds.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "canforge/core/Result.hpp"

namespace canforge::transport::detail {

/// Hard ceiling on the decompressed size of one container.
///
/// Found by fuzzing: a BLF container header carries an `uncompressedSize` the
/// file controls, and it was passed straight to `reserve()`. A four-byte field
/// therefore let a 200-byte file ask for four gigabytes. The size hint is now
/// clamped, and the output is bounded during inflation as well, so a
/// compression bomb cannot get there the slow way either. 256 MiB is far above
/// any real BLF container and far below anything that threatens a machine.
inline constexpr std::size_t kMaxInflateOutput = 256u * 1024u * 1024u;

/// Raw DEFLATE, no wrapper. `expected_size` is a hint used to reserve output.
core::Result<std::vector<std::uint8_t>> inflate_raw(const std::uint8_t* data,
                                                    std::size_t size,
                                                    std::size_t expected_size = 0);

/// zlib wrapper: two-byte header, deflate stream, four-byte Adler-32.
/// The checksum is verified.
core::Result<std::vector<std::uint8_t>> inflate_zlib(const std::uint8_t* data,
                                                     std::size_t size,
                                                     std::size_t expected_size = 0);

std::uint32_t adler32(const std::uint8_t* data, std::size_t size) noexcept;

}  // namespace canforge::transport::detail

#endif  // CANFORGE_TRANSPORT_INFLATE_HPP
