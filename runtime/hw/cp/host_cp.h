#pragma once
// SPDX-License-Identifier: GPL-3.0-or-later
#include <cstdint>

namespace GekkoAOT::NativeHW::CP {
// NativeHW 1.0-safe deliberately preserves the v4 CP/FIFO timing contract.
// HPPOA uses CP breakpoints as a guest-visible producer/consumer rendezvous;
// increasing the backlog quantum or forcing FIFO progress can reorder the
// acknowledge/re-arm sequence. Keep the proven 4 KiB ceiling and never let a
// NativeHW policy override guest breakpoint semantics.
constexpr std::uint32_t SafeMaxBatchBursts() noexcept { return 128u; }
constexpr bool PreserveGuestBreakpoints() noexcept { return true; }

// Safe direct CP->Aurora submission is possible only for the common synchronous
// empty-FIFO case. Any backlog, breakpoint or disabled GP link/read falls back
// to the exact FIFO-memory callback path.
constexpr bool CanDirectSubmit(bool enabled, bool gx_ready, bool gp_link,
                               bool gp_read, bool breakpoint_active,
                               std::uint32_t fifo_distance,
                               bool read_equals_write) noexcept {
  return enabled && gx_ready && gp_link && gp_read && !breakpoint_active &&
         fifo_distance == 0u && read_equals_write;
}
} // namespace GekkoAOT::NativeHW::CP
