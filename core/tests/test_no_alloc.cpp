// SPDX-License-Identifier: MIT
//
// Proof that the signal codec is allocation-free and therefore callable from a
// real-time context. Every form of the global allocation operator is replaced;
// while the trap is armed any allocation aborts the process, so passing is
// evidence that nothing between arm() and disarm() allocated at all.
//
// Two constraints shape the file: no GoogleTest macro may appear inside the
// armed region, because a failing assertion builds a std::string; and this test
// cannot run under AddressSanitizer, whose allocator replaces the same
// operators. CI excludes it from the sanitizer job by name.

#include "canforge/core/Frame.hpp"
#include "canforge/core/Result.hpp"
#include "canforge/core/Signal.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <vector>

namespace {

// Not std::atomic: the trap is armed and disarmed on one thread, and a relaxed
// bool keeps the check out of the way of the code being measured.
volatile bool g_trap_armed = false;
std::size_t g_allocations_seen = 0;

void trip(const char* which) {
  if (g_trap_armed) {
    g_trap_armed = false;  // avoid recursing through fprintf's own buffers
    std::fprintf(stderr, "canforge: allocation via %s inside a no-allocation region\n",
                 which);
    std::abort();
  }
}

void* checked_malloc(std::size_t size) {
  void* p = std::malloc(size == 0 ? 1 : size);
  if (p == nullptr) {
    std::abort();  // operator new must not return null; and we cannot throw here
  }
  ++g_allocations_seen;
  return p;
}

}  // namespace

// Note: GCC's -Wmismatched-new-delete fires on these definitions because it
// pairs a `new` call site with a `delete` and then sees malloc/free underneath.
// Replacing the global operators this way is exactly what the standard allows,
// so the warning is turned off for this one target in tests/CMakeLists.txt --
// a #pragma does not reach the pass that emits it.

void* operator new(std::size_t size) {
  trip("operator new");
  return checked_malloc(size);
}
void* operator new[](std::size_t size) {
  trip("operator new[]");
  return checked_malloc(size);
}
void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
  trip("operator new(nothrow)");
  return checked_malloc(size);
}
void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
  trip("operator new[](nothrow)");
  return checked_malloc(size);
}
void* operator new(std::size_t size, std::align_val_t) {
  trip("operator new(align_val_t)");
  return checked_malloc(size);
}
void* operator new[](std::size_t size, std::align_val_t) {
  trip("operator new[](align_val_t)");
  return checked_malloc(size);
}

void operator delete(void* p) noexcept {
  std::free(p);
}
void operator delete[](void* p) noexcept {
  std::free(p);
}
void operator delete(void* p, std::size_t) noexcept {
  std::free(p);
}
void operator delete[](void* p, std::size_t) noexcept {
  std::free(p);
}
void operator delete(void* p, const std::nothrow_t&) noexcept {
  std::free(p);
}
void operator delete[](void* p, const std::nothrow_t&) noexcept {
  std::free(p);
}
void operator delete(void* p, std::align_val_t) noexcept {
  std::free(p);
}
void operator delete[](void* p, std::align_val_t) noexcept {
  std::free(p);
}
void operator delete(void* p, std::size_t, std::align_val_t) noexcept {
  std::free(p);
}
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept {
  std::free(p);
}

namespace canforge::core {
namespace {

/// RAII guard so an assertion failure or early return cannot leave the trap
/// armed for the rest of the process.
class AllocationTrap {
 public:
  AllocationTrap() { g_trap_armed = true; }
  ~AllocationTrap() { g_trap_armed = false; }
  AllocationTrap(const AllocationTrap&) = delete;
  AllocationTrap& operator=(const AllocationTrap&) = delete;
};

/// Sanity check: without it the file would pass just as happily if the
/// replacement operators were never linked in, which is how this kind of test
/// usually rots.
// Both self-checks call ::operator new directly instead of writing `new int`.
// C++14 lets the compiler delete a new-expression whose allocation it can prove
// unnecessary, and clang does exactly that at -O2, which made these two tests
// pass on GCC and fail on clang. A direct call to the operator is an ordinary
// function call, so it has to happen.
TEST(NoAllocSelfCheck, TheTrapActuallyFires) {
  EXPECT_DEATH(
      {
        AllocationTrap guard;
        void* p = ::operator new(sizeof(int));
        ::operator delete(p);
      },
      "no-allocation region");
}

TEST(NoAllocSelfCheck, ReplacementOperatorsAreLinkedIn) {
  const std::size_t before = g_allocations_seen;
  void* p = ::operator new(sizeof(int));
  EXPECT_GT(g_allocations_seen, before)
      << "the replacement operator new was not linked in; this whole file is "
         "then vacuous";
  ::operator delete(p);
}

TEST(NoAlloc, SignalCodecAllocatesNothing) {
  // Build everything, including the Frames, before arming.
  const std::array<SignalLayout, 8> layouts = {{
      {0, 8, ByteOrder::Intel, Signedness::Unsigned, ValueType::Integer, 1.0, 0.0, 0.0,
       0.0},
      {0, 16, ByteOrder::Intel, Signedness::Signed, ValueType::Integer, 0.1, -40.0, 0.0,
       0.0},
      {7, 16, ByteOrder::Motorola, Signedness::Unsigned, ValueType::Integer, 0.125, 0.0,
       0.0, 0.0},
      {13, 10, ByteOrder::Motorola, Signedness::Signed, ValueType::Integer, 1.0, 0.0,
       0.0, 0.0},
      {28, 8, ByteOrder::Intel, Signedness::Unsigned, ValueType::Integer, 1.0, 0.0, 0.0,
       0.0},
      {0, 64, ByteOrder::Intel, Signedness::Signed, ValueType::Integer, 1.0, 0.0, 0.0,
       0.0},
      {7, 64, ByteOrder::Motorola, Signedness::Unsigned, ValueType::Integer, 1.0, 0.0,
       0.0, 0.0},
      {0, 32, ByteOrder::Intel, Signedness::Unsigned, ValueType::Float32, 1.0, 0.0, 0.0,
       0.0},
  }};

  const auto id = CanId::make_standard<0x123>();
  auto frame =
      Frame::make(id, {0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC, 0xDE, 0xF0}).value();

  // Named Signals own strings, so they are built outside the trap; only their
  // decode path is measured.
  std::vector<Signal> signals;
  signals.reserve(layouts.size());
  for (const SignalLayout& s : layouts) {
    signals.emplace_back("sig", s);
  }

  double sink = 0.0;
  bool all_encodes_ok = true;
  std::uint64_t raw_sink = 0;

  {
    AllocationTrap guard;
    for (int iteration = 0; iteration < 1000; ++iteration) {
      // A bounded, deterministic input. Feeding decode() back into encode()
      // would diverge for the 64-bit layouts and say nothing about allocation.
      const double input = static_cast<double>(iteration % 100);
      for (const SignalLayout& s : layouts) {
        sink += s.decode(frame.data()) * 1e-24;
        raw_sink ^= s.decode_raw(frame.data());

        // encode: returns a Result<void>, which must also not allocate.
        const Status st = s.encode(input, frame.data());
        if (!st) {
          all_encodes_ok = false;
        }
      }
      // The checked entry points build and return a Result<double>.
      for (const Signal& sig : signals) {
        const Result<double> checked = sig.try_decode(frame);
        if (checked) {
          sink += *checked * 1e-24;
        }
      }
    }
  }

  EXPECT_TRUE(all_encodes_ok);
  EXPECT_TRUE(std::isfinite(sink));
  static_cast<void>(raw_sink);
}

TEST(NoAlloc, ErrorPathsAllocateNothing) {
  // A failing operation must not allocate either -- that is the point of an
  // Error that holds a string_view onto a literal and not a std::string.
  SignalLayout s;
  s.start_bit = 60;
  s.bit_length = 16;  // does not fit in 8 bytes
  const auto id = CanId::make_standard<0x1>();
  auto frame = Frame::make(id, {1, 2, 3, 4, 5, 6, 7, 8}).value();
  const Signal signal("sig", s);

  ErrorCode seen = ErrorCode::Ok;
  bool had_error = false;
  {
    AllocationTrap guard;
    for (int i = 0; i < 1000; ++i) {
      const auto r = signal.try_decode(frame);
      if (!r) {
        had_error = true;
        seen = r.error().code();
      }
      const auto w = signal.encode(1.0, frame);
      if (!w) {
        seen = w.error().code();
      }
      // Frame construction failures too.
      const auto bad = CanId::standard(0x800u);
      if (!bad) {
        seen = bad.error().code();
      }
    }
  }
  EXPECT_TRUE(had_error);
  EXPECT_EQ(seen, ErrorCode::FrameBadIdentifier);
}

TEST(NoAlloc, FrameConstructionAllocatesNothing) {
  const auto id = CanId::make_extended<0x18DAF110>();
  const std::array<std::uint8_t, 8> payload = {1, 2, 3, 4, 5, 6, 7, 8};
  std::size_t built = 0;
  {
    AllocationTrap guard;
    for (int i = 0; i < 10000; ++i) {
      const auto f = Frame::make(id, payload.data(), payload.size(), FrameFlags::None,
                                 static_cast<std::uint64_t>(i));
      if (f) {
        ++built;
      }
      Frame copy = *f;
      copy.set_timestamp_ns(0);
    }
  }
  EXPECT_EQ(built, 10000u);
}

}  // namespace
}  // namespace canforge::core
