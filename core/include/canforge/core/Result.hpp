// SPDX-License-Identifier: MIT
#ifndef CANFORGE_CORE_RESULT_HPP
#define CANFORGE_CORE_RESULT_HPP

/// Hand-rolled stand-in for C++23's std::expected; the core library targets
/// C++17 and takes no dependencies. Error is trivially copyable and nothing
/// here throws or allocates, so the real-time signal codec
/// return a Result.

#include <cstdint>
#include <new>
#include <string_view>
#include <type_traits>
#include <utility>

namespace canforge::core {

enum class ErrorCode : std::uint16_t {
  Ok = 0,

  // Generic
  InvalidArgument,
  OutOfRange,
  NotImplemented,
  Unsupported,

  // core / frame
  FrameBadIdentifier,
  FrameBadDlc,
  FrameBadFlags,
  FramePayloadTooLarge,

  // core / codec
  CodecSignalOutOfBounds,
  CodecBadBitLength,
  CodecBadFactor,
  CodecValueNotFinite,
  CodecMultiplexMismatch,
  CodecUnknownSignal,

  // dbc / parser
  ParseIoError,
  ParseBadEncoding,
  ParseUnexpectedToken,
  ParseUnexpectedEof,
  ParseBadNumber,
  ParseUnterminatedString,
  ParseUnknownKeyword,
  ParseDuplicateDefinition,
  ParseUndefinedReference,
  ParseSemantic,

  // transport
  TransportOpenFailed,
  TransportNotOpen,
  TransportWriteFailed,
  TransportReadFailed,
  TransportTimeout,
  TransportUnsupported,
  TransportOverflow,

  // logs
  LogBadFormat,
  LogUnsupportedVersion,
  LogEndOfFile,
};

const char* to_string(ErrorCode code) noexcept;

/// Machine-readable context, interpreted according to the code:
///   Parse*      a = line (1-based),  b = column (1-based)
///   Codec*      a = start bit,       b = bit length
///   Frame*      a = offending value, b = limit
///   Transport*  a = errno,           b = unused
struct ErrorDetail {
  std::uint32_t a = 0;
  std::uint32_t b = 0;
};

class Error {
 public:
  constexpr Error() noexcept = default;

  /// `message` must have static storage duration.
  constexpr Error(ErrorCode code, std::string_view message,
                  ErrorDetail detail = {}) noexcept
      : code_(code), message_(message), detail_(detail) {}

  constexpr ErrorCode code() const noexcept { return code_; }
  constexpr std::string_view message() const noexcept { return message_; }
  constexpr ErrorDetail detail() const noexcept { return detail_; }

  constexpr Error with(ErrorDetail d) const noexcept {
    return Error(code_, message_, d);
  }

  friend constexpr bool operator==(const Error& a, const Error& b) noexcept {
    return a.code_ == b.code_ && a.detail_.a == b.detail_.a &&
           a.detail_.b == b.detail_.b;
  }
  friend constexpr bool operator!=(const Error& a, const Error& b) noexcept {
    return !(a == b);
  }

 private:
  ErrorCode code_ = ErrorCode::Ok;
  std::string_view message_{};
  ErrorDetail detail_{};
};

static_assert(std::is_trivially_copyable_v<Error>,
              "Error must be trivially copyable to cross the RT boundary");

namespace detail {
[[noreturn]] void result_no_value(ErrorCode code, std::string_view msg) noexcept;
[[noreturn]] void result_no_error() noexcept;
}  // namespace detail

template <typename T, typename E = Error>
class Result {
  static_assert(!std::is_reference_v<T>, "Result<T&> is not supported");
  static_assert(!std::is_same_v<std::decay_t<T>, E>,
                "Result<E, E> would be ambiguous");

 public:
  using value_type = T;
  using error_type = E;

  Result(const T& v) : engaged_(true) { ::new (ptr()) T(v); }        // NOLINT
  Result(T&& v) : engaged_(true) { ::new (ptr()) T(std::move(v)); }  // NOLINT
  Result(const E& e) : engaged_(false) { ::new (err()) E(e); }       // NOLINT

  Result(const Result& o) : engaged_(o.engaged_) {
    if (engaged_) {
      ::new (ptr()) T(o.storage_.value);
    } else {
      ::new (err()) E(o.storage_.error);
    }
  }
  Result(Result&& o) noexcept(std::is_nothrow_move_constructible_v<T>)
      : engaged_(o.engaged_) {
    if (engaged_) {
      ::new (ptr()) T(std::move(o.storage_.value));
    } else {
      ::new (err()) E(o.storage_.error);
    }
  }
  Result& operator=(const Result& o) {
    if (this != &o) {
      destroy();
      engaged_ = o.engaged_;
      if (engaged_) {
        ::new (ptr()) T(o.storage_.value);
      } else {
        ::new (err()) E(o.storage_.error);
      }
    }
    return *this;
  }
  Result& operator=(Result&& o) noexcept(
      std::is_nothrow_move_constructible_v<T>) {
    if (this != &o) {
      destroy();
      engaged_ = o.engaged_;
      if (engaged_) {
        ::new (ptr()) T(std::move(o.storage_.value));
      } else {
        ::new (err()) E(o.storage_.error);
      }
    }
    return *this;
  }
  ~Result() { destroy(); }

  bool has_value() const noexcept { return engaged_; }
  explicit operator bool() const noexcept { return engaged_; }

  /// Reports and aborts on error; never throws, so it is safe across a boundary.
  const T& value() const& {
    check();
    return storage_.value;
  }
  T& value() & {
    check();
    return storage_.value;
  }
  T&& value() && {
    check();
    return std::move(storage_.value);
  }

  /// Unchecked; the precondition is has_value(). For hot paths.
  const T& operator*() const noexcept { return storage_.value; }
  T& operator*() noexcept { return storage_.value; }
  const T* operator->() const noexcept { return &storage_.value; }
  T* operator->() noexcept { return &storage_.value; }

  const E& error() const noexcept {
    if (engaged_) {
      detail::result_no_error();
    }
    return storage_.error;
  }

  template <typename U>
  T value_or(U&& fallback) const& {
    return engaged_ ? storage_.value
                    : static_cast<T>(std::forward<U>(fallback));
  }

  template <typename F>
  auto map(F&& f) const& -> Result<decltype(f(std::declval<const T&>())), E> {
    using U = decltype(f(std::declval<const T&>()));
    if (!engaged_) {
      return Result<U, E>(storage_.error);
    }
    return Result<U, E>(f(storage_.value));
  }

  template <typename F>
  auto and_then(F&& f) const& -> decltype(f(std::declval<const T&>())) {
    if (!engaged_) {
      return decltype(f(std::declval<const T&>()))(storage_.error);
    }
    return f(storage_.value);
  }

 private:
  void check() const {
    if (!engaged_) {
      detail::result_no_value(storage_.error.code(), storage_.error.message());
    }
  }
  void destroy() noexcept {
    if (engaged_) {
      storage_.value.~T();
    } else {
      storage_.error.~E();
    }
  }
  void* ptr() noexcept { return static_cast<void*>(&storage_.value); }
  void* err() noexcept { return static_cast<void*>(&storage_.error); }

  union Storage {
    Storage() noexcept : none() {}
    ~Storage() {}
    char none;
    T value;
    E error;
  } storage_;
  bool engaged_;
};

template <typename E>
class Result<void, E> {
 public:
  using value_type = void;
  using error_type = E;

  Result() noexcept : error_(), engaged_(true) {}
  Result(const E& e) noexcept : error_(e), engaged_(false) {}  // NOLINT

  bool has_value() const noexcept { return engaged_; }
  explicit operator bool() const noexcept { return engaged_; }
  void value() const {
    if (!engaged_) {
      detail::result_no_value(error_.code(), error_.message());
    }
  }
  const E& error() const noexcept {
    if (engaged_) {
      detail::result_no_error();
    }
    return error_;
  }

 private:
  E error_;
  bool engaged_;
};

using Status = Result<void, Error>;

inline Status ok() noexcept { return Status{}; }

// Propagation helpers. Statement macros, not expressions, so they stay
// portable: GNU statement-expressions would trip -Wpedantic.
#define CANFORGE_TRY_CAT_(a, b) a##b
#define CANFORGE_TRY_ID_(a, b) CANFORGE_TRY_CAT_(a, b)

/// `CANFORGE_TRY(auto x, expr);` binds on success, returns the Error on failure.
#define CANFORGE_TRY(decl, expr)                              \
  auto CANFORGE_TRY_ID_(canforge_try_, __LINE__) = (expr);    \
  if (!CANFORGE_TRY_ID_(canforge_try_, __LINE__)) {           \
    return CANFORGE_TRY_ID_(canforge_try_, __LINE__).error(); \
  }                                                           \
  decl = std::move(CANFORGE_TRY_ID_(canforge_try_, __LINE__)).value()

/// `CANFORGE_CHECK(expr);` for calls whose value is not needed.
#define CANFORGE_CHECK(expr)                                    \
  do {                                                          \
    auto CANFORGE_TRY_ID_(canforge_chk_, __LINE__) = (expr);    \
    if (!CANFORGE_TRY_ID_(canforge_chk_, __LINE__)) {           \
      return CANFORGE_TRY_ID_(canforge_chk_, __LINE__).error(); \
    }                                                           \
  } while (false)

}  // namespace canforge::core

#endif  // CANFORGE_CORE_RESULT_HPP
