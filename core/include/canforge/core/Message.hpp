// SPDX-License-Identifier: MIT
#ifndef CANFORGE_CORE_MESSAGE_HPP
#define CANFORGE_CORE_MESSAGE_HPP

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "canforge/core/Attribute.hpp"
#include "canforge/core/Frame.hpp"
#include "canforge/core/Result.hpp"
#include "canforge/core/Signal.hpp"

namespace canforge::core {

/// The placeholder Vector tools write where a node is required but none applies.
inline constexpr std::string_view kNoNode = "Vector__XXX";

struct DecodedSignal {
  const Signal* signal = nullptr;
  std::uint64_t raw = 0;
  double value = 0.0;

  std::string_view name() const noexcept {
    return signal != nullptr ? std::string_view{signal->name()} : std::string_view{};
  }
};

class Message {
 public:
  Message() = default;
  Message(CanId id, std::string name, std::uint8_t dlc)
      : name_(std::move(name)), id_(id), dlc_(dlc) {}

  CanId id() const noexcept { return id_; }
  const std::string& name() const noexcept { return name_; }
  /// Payload length in bytes. For CAN FD this is the byte count, not the DLC.
  std::uint8_t dlc() const noexcept { return dlc_; }
  const std::string& transmitter() const noexcept { return transmitter_; }
  const std::string& comment() const noexcept { return comment_; }
  const std::vector<Signal>& signals() const noexcept { return signals_; }
  std::vector<Signal>& signals() noexcept { return signals_; }
  const AttributeMap& attributes() const noexcept { return attributes_; }
  AttributeMap& attributes() noexcept { return attributes_; }
  /// Extra transmitters from `BO_TX_BU_`.
  const std::vector<std::string>& additional_transmitters() const noexcept {
    return additional_transmitters_;
  }
  bool is_fd() const noexcept { return is_fd_; }

  void set_id(CanId v) noexcept { id_ = v; }
  void set_name(std::string v) { name_ = std::move(v); }
  void set_dlc(std::uint8_t v) noexcept { dlc_ = v; }
  void set_transmitter(std::string v) { transmitter_ = std::move(v); }
  void set_comment(std::string v) { comment_ = std::move(v); }
  std::string& comment_ref() noexcept { return comment_; }
  void set_fd(bool v) noexcept { is_fd_ = v; }
  void set_additional_transmitters(std::vector<std::string> v) {
    additional_transmitters_ = std::move(v);
  }
  void add_signal(Signal s) { signals_.push_back(std::move(s)); }

  const Signal* find_signal(std::string_view name) const noexcept;
  Signal* find_signal(std::string_view name) noexcept;

  bool is_multiplexed() const noexcept;

  /// The root multiplexor: with extended multiplexing a message can have several,
  /// and this returns the one that is not itself switched by another.
  const Signal* multiplexor() const noexcept;

  template <std::size_t Cap>
  Result<std::uint32_t> multiplex_value(const BasicFrame<Cap>& frame) const noexcept {
    const Signal* m = multiplexor();
    if (m == nullptr) {
      return Error(ErrorCode::CodecMultiplexMismatch,
                   "message has no multiplexor signal");
    }
    if (!m->layout().fits(frame.size())) {
      return Error(ErrorCode::CodecSignalOutOfBounds,
                   "multiplexor signal does not fit in the received payload",
                   {m->layout().start_bit, m->layout().bit_length});
    }
    return static_cast<std::uint32_t>(m->layout().decode_raw(frame.data()));
  }

  /// Extended multiplexing (SG_MUL_VAL_) nests: a signal can be switched in by a
  /// multiplexor that is itself switched in by an outer one, to arbitrary depth.
  /// So the chain is walked upward and every level above must also select it.
  template <std::size_t Cap>
  bool is_signal_active(const Signal& s, const BasicFrame<Cap>& frame,
                        int depth = 0) const noexcept {
    if (s.multiplex_role() == MultiplexRole::None) {
      return true;
    }
    if (s.multiplex_role() == MultiplexRole::Multiplexor &&
        s.multiplexor_name().empty()) {
      return true;  // the root switch is always on the wire
    }
    if (depth > 8) {
      return false;  // a database with a multiplexor cycle; refuse to loop
    }
    // A signal with no named multiplexor is switched by the message's root.
    const Signal* sw = s.multiplexor_name().empty()
                           ? multiplexor()
                           : find_signal(s.multiplexor_name());
    if (sw == nullptr || sw == &s) {
      return false;
    }
    if (!is_signal_active(*sw, frame, depth + 1)) {
      return false;
    }
    if (!sw->layout().fits(frame.size())) {
      return false;
    }
    const auto value =
        static_cast<std::uint32_t>(sw->layout().decode_raw(frame.data()));
    return s.is_present_for(value);
  }

  /// Calls fn(const Signal&, raw, physical) for every active signal, allocating
  /// nothing. Signals that do not fit the received payload are skipped rather
  /// than read out of bounds -- a short frame is a bus fact, not a database error.
  template <std::size_t Cap, typename Fn>
  Status for_each_active_signal(const BasicFrame<Cap>& frame, Fn&& fn) const {
    for (const Signal& s : signals_) {
      if (!s.layout().fits(frame.size())) {
        continue;
      }
      if (!is_signal_active(s, frame)) {
        continue;
      }
      const std::uint64_t raw = s.layout().decode_raw(frame.data());
      fn(s, raw, s.layout().raw_to_physical(raw));
    }
    return ok();
  }

  /// Allocates; not for a real-time path.
  template <std::size_t Cap>
  Result<std::vector<DecodedSignal>> decode(const BasicFrame<Cap>& frame) const {
    std::vector<DecodedSignal> out;
    out.reserve(signals_.size());
    const Status st = for_each_active_signal(
        frame, [&out](const Signal& s, std::uint64_t raw, double value) {
          out.push_back(DecodedSignal{&s, raw, value});
        });
    if (!st) {
      return st.error();
    }
    return out;
  }

  /// Unknown names are an error, so a typo cannot silently produce an all-zero
  /// frame. Signals not named keep their existing value.
  Result<Frame> encode(
      const std::vector<std::pair<std::string, double>>& values) const;

  Status validate() const noexcept;

  friend bool operator==(const Message& a, const Message& b);
  friend bool operator!=(const Message& a, const Message& b) { return !(a == b); }

 private:
  std::string name_;
  std::string transmitter_;
  std::string comment_;
  std::vector<std::string> additional_transmitters_;
  std::vector<Signal> signals_;
  AttributeMap attributes_;
  CanId id_{};
  std::uint8_t dlc_ = 0;
  bool is_fd_ = false;
};

}  // namespace canforge::core

#endif  // CANFORGE_CORE_MESSAGE_HPP
