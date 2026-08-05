// SPDX-License-Identifier: MIT
//
// A standalone driver for the libFuzzer targets.
//
// The targets use the libFuzzer entry point so they run unchanged under
// `clang -fsanitize=fuzzer`. This file bolts a `main` onto them so they also
// build with GCC, which has no libFuzzer. It's a mutation fuzzer with no
// coverage feedback: strictly weaker, and no substitute for the real thing. But
// it needs no clang, runs anywhere, and it's what found the bug in the README.
//
//   ./fuzz_dbc corpus/dbc 5000000        run 5 million mutated inputs
//   ./fuzz_dbc file.dbc                  run one file and exit
//
// Reproducibility: the seed is printed at startup and can be supplied with
// CANFORGE_FUZZ_SEED, so a crash is replayable.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <csignal>
#include <random>
#include <string>
#include <vector>

#if defined(__unix__)
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size);

namespace {

std::vector<std::uint8_t> read_file(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(in)),
                                   std::istreambuf_iterator<char>());
}

bool is_directory(const std::string& path) {
#if defined(__unix__)
  struct stat info {};
  return ::stat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode);
#else
  return false;
#endif
}

std::vector<std::vector<std::uint8_t>> load_corpus(const std::string& path) {
  std::vector<std::vector<std::uint8_t>> out;
#if defined(__unix__)
  if (is_directory(path)) {
    DIR* dir = ::opendir(path.c_str());
    if (dir != nullptr) {
      while (const dirent* entry = ::readdir(dir)) {
        const std::string name = entry->d_name;
        if (name == "." || name == "..") {
          continue;
        }
        auto bytes = read_file(path + "/" + name);
        if (!bytes.empty()) {
          out.push_back(std::move(bytes));
        }
      }
      ::closedir(dir);
    }
    return out;
  }
#endif
  auto bytes = read_file(path);
  if (!bytes.empty()) {
    out.push_back(std::move(bytes));
  }
  return out;
}

/// Byte-level mutations. Deliberately crude: without coverage feedback the
/// value comes from volume and from starting at a valid seed, not from
/// cleverness.
void mutate(std::vector<std::uint8_t>& data, std::mt19937_64& rng) {
  if (data.empty()) {
    data.push_back(static_cast<std::uint8_t>(rng()));
    return;
  }
  std::uniform_int_distribution<int> choice(0, 6);
  std::uniform_int_distribution<std::size_t> position(0, data.size() - 1);
  switch (choice(rng)) {
    case 0:  // flip a bit
      data[position(rng)] ^= static_cast<std::uint8_t>(1u << (rng() % 8u));
      break;
    case 1:  // replace a byte
      data[position(rng)] = static_cast<std::uint8_t>(rng());
      break;
    case 2:  // insert a byte
      data.insert(data.begin() + static_cast<std::ptrdiff_t>(position(rng)),
                  static_cast<std::uint8_t>(rng()));
      break;
    case 3:  // erase a byte
      data.erase(data.begin() + static_cast<std::ptrdiff_t>(position(rng)));
      break;
    case 4: {  // duplicate a run, which is how you grow a structure
      const std::size_t from = position(rng);
      const std::size_t len = std::min<std::size_t>(data.size() - from, 1u + rng() % 32u);
      data.insert(data.end(), data.begin() + static_cast<std::ptrdiff_t>(from),
                  data.begin() + static_cast<std::ptrdiff_t>(from + len));
      break;
    }
    case 5: {  // splice out a run
      const std::size_t from = position(rng);
      const std::size_t len = std::min<std::size_t>(data.size() - from, 1u + rng() % 32u);
      data.erase(data.begin() + static_cast<std::ptrdiff_t>(from),
                 data.begin() + static_cast<std::ptrdiff_t>(from + len));
      break;
    }
    default: {  // write an interesting integer, which finds length bugs
      static const std::uint8_t kInteresting[] = {0x00, 0x01, 0x7F, 0x80, 0xFF,
                                                  0xFE, 0x10, 0x0F, 0x20, 0x30};
      data[position(rng)] = kInteresting[rng() % (sizeof(kInteresting))];
      break;
    }
  }
  if (data.size() > 65536) {
    data.resize(65536);
  }
}

/// The input currently being run, written out by the crash handler so a
/// sanitizer abort leaves a reproducer behind instead of just a stack trace.
std::vector<std::uint8_t>* g_current = nullptr;
std::string g_crash_prefix = "crash";

void save_current() {
  if (g_current == nullptr || g_current->empty()) {
    return;
  }
  const std::string name = g_crash_prefix + "-" + std::to_string(::getpid()) + ".bin";
  std::ofstream out(name, std::ios::binary);
  out.write(reinterpret_cast<const char*>(g_current->data()),
            static_cast<std::streamsize>(g_current->size()));
  std::fprintf(stderr, "\ncanforge-fuzz: crashing input written to %s (%zu bytes)\n",
               name.c_str(), g_current->size());
}

void on_fatal(int signal) {
  save_current();
  std::signal(signal, SIG_DFL);
  std::raise(signal);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr,
                 "usage: %s <corpus-dir-or-file> [iterations]\n"
                 "       CANFORGE_FUZZ_SEED=<n> to reproduce a run\n",
                 argv[0]);
    return 2;
  }
  const std::string path = argv[1];
  const std::uint64_t iterations =
      argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 0;

  std::vector<std::vector<std::uint8_t>> corpus = load_corpus(path);
  if (corpus.empty()) {
    std::fprintf(stderr, "%s: no inputs found in %s\n", argv[0], path.c_str());
    return 2;
  }

  if (iterations == 0) {
    // Replay mode: run every corpus entry once and exit. This is what CI does
    // on every push, so a committed regression input stays a regression test.
    for (const auto& input : corpus) {
      LLVMFuzzerTestOneInput(input.data(), input.size());
    }
    std::fprintf(stderr, "%s: replayed %zu inputs\n", argv[0], corpus.size());
    return 0;
  }

  const char* seed_env = std::getenv("CANFORGE_FUZZ_SEED");
  const std::uint64_t seed =
      seed_env != nullptr ? std::strtoull(seed_env, nullptr, 10) : 0x9E3779B97F4A7C15ULL;
  std::fprintf(stderr, "%s: %llu iterations, seed %llu, %zu corpus entries\n", argv[0],
               static_cast<unsigned long long>(iterations),
               static_cast<unsigned long long>(seed), corpus.size());

  std::signal(SIGABRT, on_fatal);
  std::signal(SIGSEGV, on_fatal);
  std::signal(SIGBUS, on_fatal);
  g_crash_prefix = std::string(argv[0]).substr(std::string(argv[0]).find_last_of('/') + 1);

  std::mt19937_64 rng(seed);
  std::vector<std::uint8_t> buffer;
  g_current = &buffer;
  for (std::uint64_t i = 0; i < iterations; ++i) {
    buffer = corpus[rng() % corpus.size()];
    const int rounds = 1 + static_cast<int>(rng() % 8u);
    for (int r = 0; r < rounds; ++r) {
      mutate(buffer, rng);
    }
    LLVMFuzzerTestOneInput(buffer.data(), buffer.size());
    if ((i % 500000u) == 0u && i != 0) {
      std::fprintf(stderr, "  %llu\n", static_cast<unsigned long long>(i));
    }
  }
  std::fprintf(stderr, "%s: done\n", argv[0]);
  return 0;
}
