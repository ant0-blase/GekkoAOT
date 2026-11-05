#pragma once
// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/input.h"
#include <atomic>
#include <cstring>
namespace GekkoAOT::Input {
// Single SDL producer, arbitrary readers. Atomic payload words avoid the C++
// data race of a seqlock over ordinary Pad structs. SC ordering gives a simple
// portable coherent-frame proof; readers only issue loads (no RMW or mutex).
class PadSnapshot final {
public:
  PadSnapshot() { Publish({}); }
  void Publish(const std::array<Pad, 4> &pads) {
    std::array<std::uint8_t, 48> bytes{};
    const auto motor = Encode(pads, bytes.data());
    generation_.fetch_add(1);
    for (unsigned i = 0; i < 6; ++i) {
      std::uint64_t word;
      std::memcpy(&word, bytes.data() + i * 8, 8);
      words_[i].store(word);
    }
    words_[6].store(motor);
    generation_.fetch_add(1);
  }
  std::uint32_t ReadEncoded(std::uint8_t *out) const {
    std::array<std::uint64_t, 7> words;
    for (;;) {
      const auto before = generation_.load();
      if (before & 1)
        continue;
      for (unsigned i = 0; i < 7; ++i)
        words[i] = words_[i].load();
      if (generation_.load() == before)
        break;
    }
    std::memcpy(out, words.data(), 48);
    return static_cast<std::uint32_t>(words[6]);
  }
  void Read(std::array<Pad,4>& out) const {
    std::array<std::uint8_t,48> bytes;
    const auto motor = ReadEncoded(bytes.data());
    out = {};
    for (unsigned i=0;i<4;++i) {
      const auto* p = bytes.data()+12*i; auto& pad = out[i];
      pad.connected = p[10] == 0; pad.rumble = (motor & (0x80000000u>>i)) != 0;
      pad.buttons = std::uint16_t(p[0])<<8 | p[1];
      pad.stick_x = static_cast<std::int8_t>(p[2]); pad.stick_y = static_cast<std::int8_t>(p[3]);
      pad.sub_x = static_cast<std::int8_t>(p[4]); pad.sub_y = static_cast<std::int8_t>(p[5]);
      pad.left=p[6];pad.right=p[7];pad.analog_a=p[8];pad.analog_b=p[9];
    }
  }
  static_assert(std::atomic<std::uint64_t>::is_always_lock_free);

private:
  alignas(64) std::atomic<std::uint64_t> generation_{0};
  std::array<std::atomic<std::uint64_t>, 7> words_{};
};
} // namespace GekkoAOT::Input
