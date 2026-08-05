// SPDX-License-Identifier: MIT
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>

#include "canforge/transport/LogFormat.hpp"

/// BLF is a binary, compressed, undocumented format parsed from offsets that
/// two published references disagree about. If anything in this project is
/// going to read out of bounds, it is this.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  // The reader takes a path, so the input is staged through a temporary file.
  // Slower than an in-memory entry point but it exercises the real code path
  // including the header validation.
  char path[] = "/tmp/canforge_fuzz_blf_XXXXXX";
  const int fd = mkstemp(path);
  if (fd < 0) {
    return 0;
  }
  std::FILE* file = fdopen(fd, "wb");
  if (file == nullptr) {
    std::remove(path);
    return 0;
  }
  if (size != 0) {
    std::fwrite(data, 1, size, file);
  }
  std::fclose(file);

  auto reader = canforge::transport::open_reader(path,
                                                 canforge::transport::LogFormat::Blf);
  if (reader) {
    // read_all stops at the first hard error, so a bounded loop is used to
    // make sure a corrupt file cannot spin forever.
    for (int i = 0; i < 100000; ++i) {
      auto record = reader.value()->next();
      if (!record) {
        break;
      }
    }
  }
  std::remove(path);
  return 0;
}
