#pragma once
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <cstddef>
#include <cstdint>

namespace GekkoAOT::DSP {

class NativeDSP;

// Native renderer for the early GameCube JAudio "light DSP" work protocol.
//
// The PPC side describes the renderer at runtime with the 0x8100 setup packet
// (voice table + SRC/AFC tables + FX table), so this implementation does not
// contain title-specific addresses or copied DSP microcode.  0x8200 packets
// request one planar stereo render frame.
class NativeJAudioHLE final {
public:
  explicit NativeJAudioHLE(NativeDSP* dsp = nullptr) : dsp_(dsp) {}

  void Bind(NativeDSP* dsp) { dsp_ = dsp; }
  void Reset();

  // words[0] is the PPC command word as sent; subsequent words are 31-bit DSP
  // mailbox payloads (which naturally converts cached MEM1 pointers to their
  // physical form). Returns true when the packet belongs to JAudio.
  bool HandlePacket(const std::uint32_t* words, std::size_t count);

  bool Active() const { return active_; }
  std::uint64_t SetupPackets() const { return setup_packets_; }
  std::uint64_t SyncFrames() const { return sync_frames_; }
  std::uint64_t VoicesMixed() const { return voices_mixed_; }

private:
  static constexpr std::size_t kMaxVoices = 64u;
  static constexpr std::size_t kVpbBytes = 0x180u;
  static constexpr std::size_t kMixerCount = 6u;
  static constexpr std::size_t kSamplesPerSubframe = 80u;
  static constexpr std::size_t kMaxSubframes = 16u;
  static constexpr std::size_t kRawSamples = 0x500u + 20u;
  static constexpr std::size_t kBusCount = 13u;

  enum Bus : std::size_t {
    FrontLeft = 0,
    FrontRight,
    BackLeft,
    BackRight,
    FrontLeftWet,
    FrontRightWet,
    BackLeftWet,
    BackRightWet,
    ExtraWet0,
    ExtraWet1,
    Extra0,
    Extra1,
    Extra2,
  };

  struct Voice {
    std::array<std::uint8_t, kVpbBytes> bytes{};

    std::uint16_t U16(std::size_t off) const;
    std::int16_t S16(std::size_t off) const;
    std::uint32_t U32(std::size_t off) const;
    void SetU16(std::size_t off, std::uint16_t value);
    void SetS16(std::size_t off, std::int16_t value);
    void SetU32(std::size_t off, std::uint32_t value);
  };

  using MixBuffer = std::array<std::int32_t, kSamplesPerSubframe>;
  using Buses = std::array<MixBuffer, kBusCount>;

  bool Setup(const std::uint32_t* words, std::size_t count);
  bool Sync(const std::uint32_t* words, std::size_t count);
  bool LoadTables();
  bool LoadVoice(std::uint32_t index, Voice* voice) const;
  bool StoreVoice(std::uint32_t index, const Voice& voice);

  void BeginSubframe();
  void FinishSubframe(std::int16_t* left, std::int16_t* right);
  void RenderVoice(std::uint32_t index, Voice* voice);
  void LoadInput(std::uint32_t index, Voice* voice, MixBuffer* output);
  std::uint16_t NeededRawSamples(const Voice& voice) const;
  void Resample(Voice* voice, const std::int16_t* source, MixBuffer* output);
  void LoadPcm(Voice* voice, std::int16_t* output, std::uint16_t count,
               std::uint8_t bytes_per_sample);
  void LoadAfc(Voice* voice, std::int16_t* output, std::uint16_t count);
  bool DecodeAfcBlocks(Voice* voice, std::int16_t* output, std::size_t blocks);
  void LoadPattern(Voice* voice, MixBuffer* output);

  void ApplyLowPass(Voice* voice, MixBuffer* samples);
  void ApplyFir(Voice* voice, MixBuffer* samples);
  void ApplyBiquad(Voice* voice, MixBuffer* samples);
  void MixVoice(Voice* voice, const MixBuffer& samples);

  void ReadEffects();
  void WriteEffects();
  MixBuffer* BusForId(std::uint16_t id);

  bool ReadMain(std::uint32_t address, void* dst, std::size_t size) const;
  bool WriteMain(std::uint32_t address, const void* src, std::size_t size);
  bool ReadAram(std::uint32_t address, void* dst, std::size_t size) const;
  bool ReadMainS16(std::uint32_t address, std::int16_t* dst, std::size_t count) const;
  bool WriteMainS16(std::uint32_t address, const std::int16_t* src, std::size_t count);

  static std::int16_t Clamp16(std::int64_t value);
  static std::uint16_t LoadBe16(const std::uint8_t* p);
  static std::uint32_t LoadBe32(const std::uint8_t* p);
  static void StoreBe16(std::uint8_t* p, std::uint16_t value);
  static void StoreBe32(std::uint8_t* p, std::uint32_t value);

  NativeDSP* dsp_ = nullptr;
  bool active_ = false;
  std::uint32_t voice_count_ = 0;
  std::uint32_t vpb_address_ = 0;
  std::uint32_t resample_table_address_ = 0;
  std::uint32_t afc_table_address_ = 0;
  std::uint32_t fx_table_address_ = 0;
  std::uint16_t master_level_ = 0x4000u;

  std::array<std::int16_t, 256> resample_coefficients_{};
  std::array<std::int16_t, 256> patterns_{};
  std::array<std::int16_t, 32> afc_coefficients_{};
  bool tables_ready_ = false;

  Buses buses_{};
  MixBuffer previous_back_left_{};
  MixBuffer previous_back_right_{};
  std::array<std::array<std::int16_t, 8>, 4> fx_history_{};
  std::array<std::uint16_t, 4> fx_cursor_{};
  std::array<std::int16_t, 128> surround_sine_{};

  std::uint64_t setup_packets_ = 0;
  std::uint64_t sync_frames_ = 0;
  std::uint64_t voices_mixed_ = 0;
  std::uint64_t unsupported_sources_ = 0;
};

} // namespace GekkoAOT::DSP
