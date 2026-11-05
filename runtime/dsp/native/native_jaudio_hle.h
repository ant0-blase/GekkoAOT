#pragma once
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>

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

  // Standard GameCube JAudio tasks are admitted by an exact uploaded-image
  // hash. Other tasks retain their existing backend and protocol.
  bool ProbeStandardTask(std::uint32_t address, std::uint32_t bytes);
  bool StandardTask() const { return standard_task_; }
  void StartStandardTask();
  bool HandleStandardMail(std::uint32_t mail);
  void AdvanceStandardMail(std::uint64_t cycles = 0);

  std::uint64_t CyclesUntilStandardMail() const;

  bool Active() const { return active_; }
  std::uint64_t SetupPackets() const { return setup_packets_; }
  std::uint64_t SyncFrames() const { return sync_frames_; }
  std::uint64_t VoicesMixed() const { return voices_mixed_; }

private:
  friend struct NativeJAudioTestAccess;
  static constexpr std::size_t kMaxVoices = 64u;
  static constexpr std::size_t kVpbBytes = 0x180u;
  // The DSP owns/writes back only the first 0x80 16-bit words. The final
  // 0x40 words are PPC-owned/read-only parameters.
  static constexpr std::size_t kVpbWritebackBytes = 0x100u;
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

  void RenderVoiceRange(std::uint32_t begin, std::uint32_t end,
                        const std::array<std::uint16_t, 4>* mask = nullptr,
                        std::uint8_t* table = nullptr);
  void RenderStandard();
  void ExecuteStandardPacket(const std::array<std::uint32_t, 32>& packet, unsigned count);
  void RunPendingStandardPackets();
  void QueueStandardAck(std::uint16_t token);
  void BeginSubframe();
  void FinishSubframe(std::int16_t* left, std::int16_t* right);
  void RenderVoice(std::uint32_t index, Voice* voice);
  void LoadInput(std::uint32_t index, Voice* voice, MixBuffer* output);
  std::uint16_t NeededRawSamples(const Voice& voice) const;
  void Resample(Voice* voice, const std::int16_t* source, MixBuffer* output);
  void LoadPcm(Voice* voice, std::int16_t* output, std::uint16_t count,
               std::uint8_t bytes_per_sample);
  void LoadPcmMram(Voice* voice, std::int16_t* output, std::uint16_t count);
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

  enum class StandardState { Waiting, Packet, SyncMask, Upload, Halted };
  struct Reply { std::uint32_t value; bool irq; std::uint64_t delay = 0; };
  bool standard_task_ = false;
  bool standard_ready_ = false;
  std::uint32_t standard_image_hash_ = 0;
  std::array<std::uint32_t, 10> standard_upload_{};
  unsigned standard_upload_seen_ = 0;
  bool dolby_louder_ = false;
  bool standard_blocked_ = false;
  StandardState standard_state_ = StandardState::Waiting;
  std::deque<Reply> standard_replies_;
  struct WorkPacket { std::array<std::uint32_t, 32> words; unsigned count; };
  std::deque<WorkPacket> standard_pending_packets_;
  std::array<std::uint32_t, 32> standard_packet_{};
  std::uint32_t standard_expected_ = 0;
  std::uint32_t standard_seen_ = 0;
  std::array<std::uint16_t, 4> standard_masks_{};
  std::uint32_t standard_frames_ = 0;
  std::uint32_t standard_frame_ = 0;
  std::uint32_t standard_voice_ = 0;
  std::uint32_t standard_max_voice_ = 0;
  std::uint32_t standard_left_ = 0;
  std::uint32_t standard_right_ = 0;

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
  std::uint64_t voice_slots_scanned_ = 0;
  std::uint64_t voice_full_loads_ = 0;
  std::uint64_t voice_slots_skipped_ = 0;
  std::uint64_t output_clips_ = 0;
};

} // namespace GekkoAOT::DSP
