#pragma once
// SPDX-License-Identifier: GPL-3.0-or-later
#include "card/sdk_card.h"
#include "core/cpu_state.h"
#include "native/dvd_queue.h"
#include <array>
#include <cstdio>
#include <optional>
namespace GekkoAOT::Native {
struct DeviceState {
  struct Completion {
    bool active = false, persist = false, read = false, cancelled = false, metadata = false;
    std::uint32_t callback = 0, info = 0, buffer = 0, length = 0, offset = 0;
    int result = 0;
  };
  DvdQueue dvd;
  std::vector<std::uint8_t> dvd_bytes;
  std::uint32_t dvd_cancel_callback = 0, dvd_cancel_block = 0;
  std::array<Card::SdkCard, 2> cards;
  std::array<Completion, 2> completion{};
  std::array<std::uint32_t, 2> disk{0x80000000u, 0x80000000u};
  std::array<std::uint32_t, 2> detach{};
  std::array<std::uint32_t, 2> card_work{};
  std::uint32_t card_control = 0, card_stride = 0x110, card_disk_none = 0;
  std::int16_t card_disk_sda = 0;
  bool card_initialized = false;
  std::uint64_t pad_read_field = ~std::uint64_t(0);
  std::array<std::uint64_t, 4> pad_response_generation{};
  std::uint32_t input_policy = 0, input_samples_per_field = 2;
  unsigned pad_analog_mode = 3;
  std::optional<std::int16_t> pad_analog_sda;
  std::array<int, 2> last_result{-3, -3};
  std::uint32_t disable_scheduler = 0, enable_scheduler = 0;
  std::uint32_t set_interrupt_handler = 0, register_reset = 0;
  std::uint32_t dvd_fs_init = 0;
  bool dvd_initialized = false;
  std::FILE* console = nullptr;
  CPUState interrupted{};
  bool ai_initialized = false, ai_owned = false, audio_callback_active = false;
  std::uint32_t aid_callback = 0, ais_callback = 0, audio_stack = 0,
                callback_stack = 0;
  unsigned callback_phase = 0;
  std::uint32_t callback = 0, arg3 = 0, arg4 = 0;
  std::uint32_t saved_current_context = 0, saved_physical_context = 0,
                saved_fpu_context = 0;
  static constexpr std::uint32_t ReturnPc = 0xffffffe8u;
  // SDK asynchronous exception entry reserves OS_CONTEXT_FRAME, not an
  // ordinary eight-byte call linkage: interruption may split a prologue.
  static constexpr std::uint32_t CallbackFrameBytes = 0x300u;
};
} // namespace GekkoAOT::Native
