// SPDX-License-Identifier: MIT
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>

#include "canforge/transport/LogFormat.hpp"

/// The ASC and candump readers split lines on whitespace and index into the
/// resulting fields. Both are fuzzed together because they share that code.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  const std::string text(reinterpret_cast<const char*>(data), size);

  // candump, straight through the line parser: no file needed.
  std::size_t start = 0;
  while (start < text.size()) {
    const std::size_t end = text.find('\n', start);
    const std::string line =
        text.substr(start, end == std::string::npos ? std::string::npos : end - start);
    static_cast<void>(canforge::transport::parse_candump_line(line));
    if (end == std::string::npos) {
      break;
    }
    start = end + 1;
  }

  // ASC, through the file reader.
  char path[] = "/tmp/canforge_fuzz_asc_XXXXXX";
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
  auto reader =
      canforge::transport::open_reader(path, canforge::transport::LogFormat::Asc);
  if (reader) {
    static_cast<void>(reader.value()->read_all());
  }
  std::remove(path);
  return 0;
}
