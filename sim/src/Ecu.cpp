// SPDX-License-Identifier: MIT
#include "canforge/sim/Ecu.hpp"

#include <algorithm>
#include <cstring>

namespace canforge::sim {

std::uint8_t crc8_sae_j1850(const std::uint8_t* data, std::size_t n) noexcept {
  // Every step is done in `unsigned` and narrowed explicitly. Letting the
  // uint8_t promote to int and casting back compiles cleanly at -O2 but trips
  // -Wsign-conversion at -O1, where the optimiser cannot prove the range.
  unsigned crc = 0xFFu;
  for (std::size_t i = 0; i < n; ++i) {
    crc ^= static_cast<unsigned>(data[i]);
    for (int bit = 0; bit < 8; ++bit) {
      const unsigned shifted = (crc << 1u) & 0xFFu;
      crc = (crc & 0x80u) != 0u ? (shifted ^ 0x1Du) : shifted;
    }
  }
  return static_cast<std::uint8_t>(crc ^ 0xFFu);
}

std::uint8_t xor8(const std::uint8_t* data, std::size_t n, std::size_t skip) noexcept {
  std::uint8_t acc = 0;
  for (std::size_t i = 0; i < n; ++i) {
    if (i == skip) {
      continue;
    }
    acc = static_cast<std::uint8_t>(acc ^ data[i]);
  }
  return acc;
}

void Ecu::add(TxMessage message) {
  messages_.push_back(std::move(message));
  Runtime rt;
  rt.next_due_ns = messages_.back().phase_ns;
  runtime_.push_back(rt);
}

void Ecu::reset(std::uint64_t now_ns) {
  // Reseeding matters: jitter is drawn from this generator, so without it a
  // reset simulation would replay with different transmission times.
  rng_.seed(kJitterSeed);
  for (std::size_t i = 0; i < messages_.size(); ++i) {
    runtime_[i] = Runtime{};
    runtime_[i].next_due_ns = now_ns + messages_[i].phase_ns;
    for (SignalBinding& b : messages_[i].bindings) {
      if (b.source) {
        b.source->reset();
      }
    }
  }
}

bool Ecu::trigger(std::string_view message_name) noexcept {
  for (std::size_t i = 0; i < messages_.size(); ++i) {
    if (messages_[i].message != nullptr &&
        messages_[i].message->name() == message_name) {
      runtime_[i].triggered = true;
      return true;
    }
  }
  return false;
}

core::Frame Ecu::build(TxMessage& tx, Runtime& rt, const Vehicle* plant,
                       std::uint64_t now_ns) {
  const core::Message& m = *tx.message;
  core::Frame frame = core::Frame::make_empty(m.id(), m.dlc()).value();

  for (SignalBinding& b : tx.bindings) {
    const core::Signal* signal = m.find_signal(b.signal);
    if (signal == nullptr) {
      continue;
    }
    double value = 0.0;
    if (b.source) {
      value = b.source->sample(now_ns);
    } else if (plant != nullptr && !b.plant_field.empty()) {
      if (!plant->read(b.plant_field, value)) {
        continue;
      }
    }
    static_cast<void>(signal->encode(value, frame));
  }

  // The counter goes in before the checksum, because the checksum covers it.
  if (!tx.counter_signal.empty()) {
    if (const core::Signal* c = m.find_signal(tx.counter_signal)) {
      // A rolling counter is a raw quantity: it counts wire values, not
      // scaled physical ones, so it bypasses factor and offset.
      const std::uint64_t modulus = core::bit_mask(c->layout().bit_length) + 1u;
      c->layout().encode_raw(rt.counter % modulus, frame.data());
    }
  }
  if (tx.checksum != ChecksumKind::None && !tx.checksum_signal.empty()) {
    if (const core::Signal* c = m.find_signal(tx.checksum_signal)) {
      // The checksum signal itself must read as zero while it is computed;
      // encode() has already zeroed it because the frame started empty.
      const std::size_t skip = c->layout().start_bit / 8u;
      const std::uint8_t sum =
          tx.checksum == ChecksumKind::Crc8Sae
              ? crc8_sae_j1850(frame.data(), frame.size())
              : xor8(frame.data(), frame.size(), skip);
      // Raw, for the same reason as the counter: a checksum that went through
      // a factor and offset would not be the checksum any receiver computes.
      c->layout().encode_raw(sum, frame.data());
    }
  }
  frame.set_timestamp_ns(now_ns);
  ++rt.counter;
  return frame;
}

std::vector<core::Frame> Ecu::step(std::uint64_t now_ns, const Vehicle* plant) {
  std::vector<core::Frame> out;
  if (!running_) {
    return out;
  }
  for (std::size_t i = 0; i < messages_.size(); ++i) {
    TxMessage& tx = messages_[i];
    Runtime& rt = runtime_[i];
    if (tx.message == nullptr) {
      continue;
    }

    bool due = false;
    if (tx.mode == TxMode::Event) {
      due = rt.triggered;
      rt.triggered = false;
    } else if (now_ns >= rt.next_due_ns) {
      due = true;
    }
    if (!due) {
      continue;
    }

    core::Frame frame = build(tx, rt, plant, now_ns);

    if (tx.mode == TxMode::OnChange && rt.has_last && frame == rt.last) {
      // Nothing changed; still reschedule so the next check happens on time.
      rt.next_due_ns += tx.cycle_ns;
      continue;
    }
    rt.last = frame;
    rt.has_last = true;
    out.push_back(frame);

    if (tx.mode != TxMode::Event) {
      std::uint64_t next = rt.next_due_ns + tx.cycle_ns;
      if (tx.jitter_ns != 0) {
        std::uniform_int_distribution<std::int64_t> dist(
            -static_cast<std::int64_t>(tx.jitter_ns),
            static_cast<std::int64_t>(tx.jitter_ns));
        const std::int64_t delta = dist(rng_);
        const auto shifted = static_cast<std::int64_t>(next) + delta;
        next = shifted < 0 ? 0u : static_cast<std::uint64_t>(shifted);
      }
      // If the simulation was stepped coarsely, do not emit a burst catching
      // up on every missed cycle: skip forward instead, which is what a real
      // node with a single mailbox does.
      if (next <= now_ns) {
        next = now_ns + tx.cycle_ns;
      }
      rt.next_due_ns = next;
    }
  }
  return out;
}

}  // namespace canforge::sim
