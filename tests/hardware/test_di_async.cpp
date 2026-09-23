// SPDX-License-Identifier: GPL-3.0-or-later
#include "hw/di/native_di.h"

#include <cstdint>
#include <cstdlib>
#include <iostream>

#define CHECK(expr) do { if (!(expr)) { std::cerr << __LINE__ << ": " #expr "\n"; std::exit(1); } } while (false)

using GekkoAOT::HW::DI::NativeDI;

namespace {
constexpr std::uint32_t kBase = 0xcc006000u;
constexpr std::uint32_t kStatus = 0x00u;
constexpr std::uint32_t kCommand0 = 0x08u;
constexpr std::uint32_t kCommand1 = 0x0cu;
constexpr std::uint32_t kCommand2 = 0x10u;
constexpr std::uint32_t kDmaAddress = 0x14u;
constexpr std::uint32_t kDmaLength = 0x18u;
constexpr std::uint32_t kDmaControl = 0x1cu;
constexpr std::uint32_t kImmediate = 0x20u;

struct Media {
  unsigned calls = 0;
  int result = 1;
  std::uint64_t last_sequence = 0;

  static NativeDI::CommandResponse Execute(void* opaque, const NativeDI::CommandRequest& request) {
    auto& self = *static_cast<Media*>(opaque);
    ++self.calls;
    self.last_sequence = request.sequence;
    return {self.result, request.immediate, request.dma_length, 0u};
  }
};

void Write(NativeDI& di, std::uint32_t offset, std::uint32_t value) {
  CHECK(di.Write(kBase + offset, value, 4u));
}

std::uint32_t Read(const NativeDI& di, std::uint32_t offset) {
  std::uint64_t value = 0;
  CHECK(di.Read(kBase + offset, 4u, &value));
  return static_cast<std::uint32_t>(value);
}

void StartRead(NativeDI& di, std::uint32_t address, std::uint32_t length) {
  Write(di, kCommand0, 0xa8000000u);
  Write(di, kCommand1, 0x100u);
  Write(di, kCommand2, length);
  Write(di, kDmaAddress, address);
  Write(di, kDmaLength, length);
  Write(di, kDmaControl, 3u);
}
}

int main() {
  NativeDI di;
  Media media;
  di.SetDeviceHooks({&media, &Media::Execute});
  di.Reset(false);

  // An attached image provider must not make a read succeed with the cover open.
  Write(di, kStatus, 2u); // Unmask device errors.
  StartRead(di, 0x1000u, 0x40u);
  CHECK(media.calls == 0u);
  CHECK((Read(di, kDmaControl) & 1u) == 0u);
  CHECK((Read(di, kStatus) & 4u) != 0u);
  CHECK((Read(di, kStatus) & 16u) == 0u);
  CHECK(di.InterruptPending());

  Write(di, kStatus, 6u); // Acknowledge the error, retain its mask.
  CHECK(!di.InterruptPending());
  Write(di, kCommand0, 0xe0000000u); // RequestError returns the drive error.
  Write(di, kDmaControl, 1u);
  CHECK(Read(di, kImmediate) == 0x00023a00u);
  CHECK(di.LastError() == 0u);

  // A stale completion must never complete a new transfer that reuses TSTART.
  di.Reset(true);
  media.result = 0; // The provider schedules both reads asynchronously.
  Write(di, kStatus, 8u); // Unmask transfer completion.
  StartRead(di, 0x2000u, 0x40u);
  const std::uint64_t first = media.last_sequence;
  CHECK(first == di.CommandSequence());
  CHECK((Read(di, kDmaControl) & 1u) != 0u);
  CHECK((Read(di, kStatus) & 16u) == 0u);
  Write(di, kStatus, 0x29u); // BREAK request, with BRKINT and TCINT unmasked.
  CHECK((Read(di, kDmaControl) & 1u) == 0u);
  CHECK((Read(di, kStatus) & 64u) != 0u);
  CHECK(di.InterruptPending());
  Write(di, kStatus, 0x48u); // Acknowledge BRKINT and release BREAK.
  CHECK(!di.InterruptPending());
  StartRead(di, 0x3000u, 0x60u);
  const std::uint64_t second = media.last_sequence;
  CHECK(second != first);
  CHECK(!di.CompletePendingFor(first, true, 0u, 0x40u));
  CHECK(Read(di, kDmaAddress) == 0x3000u);
  CHECK(Read(di, kDmaLength) == 0x60u);
  CHECK((Read(di, kDmaControl) & 1u) != 0u);
  CHECK((Read(di, kStatus) & 16u) == 0u);
  CHECK(di.CompletePendingFor(second, true, 0u, 0x60u));
  CHECK(Read(di, kDmaAddress) == 0x3060u);
  CHECK(Read(di, kDmaLength) == 0u);
  CHECK((Read(di, kDmaControl) & 1u) == 0u);
  CHECK((Read(di, kStatus) & 16u) != 0u);
  CHECK(di.InterruptPending());

  // Reset also retires any outstanding provider completion.
  Write(di, kStatus, 24u); // Acknowledge TCINT, retain its mask.
  StartRead(di, 0x4000u, 0x20u);
  const std::uint64_t before_reset = media.last_sequence;
  di.Reset(true);
  StartRead(di, 0x5000u, 0x20u);
  CHECK(!di.CompletePendingFor(before_reset, true, 0u, 0x20u));
  CHECK(Read(di, kDmaAddress) == 0x5000u);
  CHECK(di.CompletePendingFor(media.last_sequence, true, 0u, 0x20u));
  CHECK(Read(di, kDmaAddress) == 0x5020u);

  std::cout << "DI absent-media, async cancel/restart, and reset tests passed\n";
}
