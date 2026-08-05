// SPDX-License-Identifier: MIT
#include "canforge/core/Message.hpp"

#include <algorithm>
#include <set>

namespace canforge::core {

const Signal* Message::find_signal(std::string_view name) const noexcept {
  for (const Signal& s : signals_) {
    if (s.name() == name) {
      return &s;
    }
  }
  return nullptr;
}

Signal* Message::find_signal(std::string_view name) noexcept {
  return const_cast<Signal*>(  // NOLINT: the const overload does the work
      static_cast<const Message*>(this)->find_signal(name));
}

bool Message::is_multiplexed() const noexcept {
  for (const Signal& s : signals_) {
    if (s.multiplex_role() != MultiplexRole::None) {
      return true;
    }
  }
  return false;
}

const Signal* Message::multiplexor() const noexcept {
  // With extended multiplexing a message can contain several multiplexors
  // nested inside each other. The root is the one that is not itself
  // switched by another multiplexor.
  for (const Signal& s : signals_) {
    if (s.multiplex_role() == MultiplexRole::Multiplexor &&
        s.multiplexor_name().empty()) {
      return &s;
    }
  }
  for (const Signal& s : signals_) {
    if (s.multiplex_role() == MultiplexRole::Multiplexor) {
      return &s;
    }
  }
  return nullptr;
}

Result<Frame> Message::encode(
    const std::vector<std::pair<std::string, double>>& values) const {
  if (dlc_ > Frame::capacity) {
    return Error(ErrorCode::FramePayloadTooLarge,
                 "message is larger than a classic CAN frame; use FdFrame",
                 {dlc_, static_cast<std::uint32_t>(Frame::capacity)});
  }
  CANFORGE_TRY(auto frame, Frame::make_empty(id_, dlc_));

  // Apply any declared start values first, so an unlisted signal is not
  // silently zero when the database says otherwise.
  for (const Signal& s : signals_) {
    const auto it = s.attributes().find("GenSigStartValue");
    if (it != s.attributes().end() && s.layout().fits(frame.size())) {
      CANFORGE_CHECK(s.layout().encode(it->second.number(), frame.data()));
    }
  }

  for (const auto& [name, value] : values) {
    const Signal* s = find_signal(name);
    if (s == nullptr) {
      return Error(ErrorCode::CodecUnknownSignal,
                   "no signal with that name in this message");
    }
    CANFORGE_CHECK(s->encode(value, frame));
  }
  return frame;
}

Status Message::validate() const noexcept {
  const std::size_t limit = is_fd_ ? std::size_t{64} : std::size_t{8};
  if (dlc_ > limit) {
    return Error(ErrorCode::ParseSemantic,
                 "message length exceeds what the frame format allows",
                 {dlc_, static_cast<std::uint32_t>(limit)});
  }
  for (const Signal& s : signals_) {
    const Status st = s.layout().validate(dlc_);
    if (!st) {
      return st;
    }
  }
  // Duplicate signal names inside one message make find_signal ambiguous and
  // break VAL_ / CM_ / BA_ lookups, so they are an error, not a lint.
  for (std::size_t i = 0; i < signals_.size(); ++i) {
    for (std::size_t j = i + 1; j < signals_.size(); ++j) {
      if (signals_[i].name() == signals_[j].name()) {
        return Error(ErrorCode::ParseDuplicateDefinition,
                     "two signals in this message share a name");
      }
    }
  }
  std::size_t roots = 0;
  for (const Signal& s : signals_) {
    if (s.multiplex_role() == MultiplexRole::Multiplexor &&
        s.multiplexor_name().empty()) {
      ++roots;
    }
  }
  if (roots > 1) {
    return Error(ErrorCode::ParseSemantic,
                 "a message may have only one top level multiplexor",
                 {static_cast<std::uint32_t>(roots), 1});
  }
  return ok();
}

bool operator==(const Message& a, const Message& b) {
  return a.id_ == b.id_ && a.name_ == b.name_ && a.dlc_ == b.dlc_ &&
         a.transmitter_ == b.transmitter_ && a.comment_ == b.comment_ &&
         a.additional_transmitters_ == b.additional_transmitters_ &&
         a.signals_ == b.signals_ && a.attributes_ == b.attributes_ &&
         a.is_fd_ == b.is_fd_;
}

}  // namespace canforge::core
