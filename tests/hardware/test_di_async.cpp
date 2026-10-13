// SPDX-License-Identifier: GPL-3.0-or-later
#include "hw/di/native_di.h"

#include <cstdint>
#include <cstdlib>
#include <array>
#include <cstring>
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
  unsigned audio_reads = 0;
  std::uint64_t last_audio_offset = 0;

  static NativeDI::CommandResponse Execute(void* opaque, const NativeDI::CommandRequest& request) {
    auto& self = *static_cast<Media*>(opaque);
    ++self.calls;
    self.last_sequence = request.sequence;
    return {self.result, request.immediate, request.dma_length, 0u};
  }

  static bool ReadAudio(void* opaque, std::uint64_t offset, void* destination,
                        std::uint32_t size) {
    auto& self = *static_cast<Media*>(opaque);
    if (size != 32u || (offset != 0x100u && offset != 0x120u)) return false;
    ++self.audio_reads;
    self.last_audio_offset = offset;
    std::array<std::uint8_t, 32> block{};
    block.fill(offset == 0x100u ? 0x21u : 0x43u);
    block[0] = block[1] = 0u;
    std::memcpy(destination, block.data(), block.size());
    return true;
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

  // Native DOL boot inherits IPL audio-buffer state. A status query on an
  // enabled but idle stream returns zero and TCINT, rather than poisoning the
  // SDK DVD queue with InvalidCommand. Installing boot state emits no IRQ.
  di.Reset(true);
  const auto boot_sequence = di.CommandSequence();
  di.ConfigureAudioBuffer(true, 0);
  CHECK(di.AudioEnabled());
  CHECK(di.CommandSequence() == boot_sequence);
  CHECK(Read(di, kStatus) == 0);
  Write(di, kStatus, 8);
  Write(di, kCommand0, 0xe2000000);
  Write(di, kDmaControl, 1);
  CHECK(Read(di, kImmediate) == 0);
  CHECK((Read(di, kStatus) & 16) != 0);
  CHECK((Read(di, kStatus) & 4) == 0);
  CHECK(di.LastError() == 0);
  Write(di, kStatus, 24);
  Write(di, kCommand0, 0xe4000000); // SDK explicitly disables audio.
  Write(di, kDmaControl, 1);
  CHECK(!di.AudioEnabled());

  // Two 28-sample stereo DTK blocks from the disc must reach PCM at the
  // configured stream rate and volume, then repeat at the track boundary.
  di.Reset(true);
  di.SetDeviceHooks({&media, &Media::Execute, &Media::ReadAudio});
  di.ConfigureAudioBuffer(true, 0u);
  Write(di, kCommand0, 0xe1000000u);
  Write(di, kCommand1, 0x100u / 4u);
  Write(di, kCommand2, 64u);
  Write(di, kDmaControl, 1u);
  CHECK(di.AudioStreaming());
  std::array<std::int16_t, 16> audio{};
  di.MixAudio(audio.data(), 8u, 32028u, 32028u, 0xffffu);
  CHECK(audio[0] == 4096 && audio[1] == 8192);
  CHECK(media.audio_reads == 1u && media.last_audio_offset == 0x100u);
  CHECK(di.AudioPosition() == 0x120u);
  for (unsigned i = 0; i < 4u; ++i)
    di.MixAudio(audio.data(), 8u, 32028u, 32028u, 0u);
  CHECK(media.audio_reads >= 2u);
  CHECK(di.AudioStreaming());
  Write(di, kCommand0, 0xe1010000u);
  Write(di, kDmaControl, 1u);
  CHECK(!di.AudioStreaming());

  std::cout << "DI absent-media, async cancel/restart, and reset tests passed\n";
}
