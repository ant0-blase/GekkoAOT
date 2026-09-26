#pragma once
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <cstddef>
#include <cstdint>

namespace GekkoAOT::DSP {

class NativeDSP;

// Native high-level implementation of the common GameCube AX DSP uCode
// protocol.  Detection is based on the uploaded DSP task image and the public
// mailbox protocol (0xBABExxxx command lists), never on a title/game id.
// Unknown/custom ucodes remain on the standalone LLE path.
class NativeAXHLE final {
public:
  explicit NativeAXHLE(NativeDSP* dsp = nullptr) : dsp_(dsp) {}

  void Bind(NativeDSP* dsp) { dsp_ = dsp; }
  void Reset();

  // Probe a freshly uploaded DSP task. Returns true only for a structurally
  // recognizable GameCube AX uCode.
  bool ProbeTask(std::uint32_t code_address, std::uint32_t code_size);
  bool Active() const { return active_; }

  // Consume a PPC->DSP mailbox value while AX HLE owns the task.
  bool HandleCpuMail(std::uint32_t raw_mail);
  void AdvancePpcCycles(std::uint64_t cycles);

  std::uint64_t CommandLists() const { return command_lists_; }
  std::uint64_t VoicesMixed() const { return voices_mixed_; }
  std::uint64_t OutputFrames() const { return output_frames_; }
  std::uint32_t UcodeCrc32() const { return ucode_crc32_; }

private:
  static constexpr std::size_t kSamplesPerFrame = 32u * 5u;
  static constexpr std::size_t kChannelCount = 9u;
  static constexpr std::size_t kCanonicalPbWords = 122u;
  static constexpr std::size_t kOldPbWords = 118u; // old AX layout omits LPF (4 words)

  enum class MailState : std::uint8_t {
    WaitingForCommandSize,
    WaitingForCommandAddress,
    WaitingForTaskReply,
  };

  enum Channel : std::size_t {
    MainL = 0,
    MainR = 1,
    MainS = 2,
    AuxAL = 3,
    AuxAR = 4,
    AuxAS = 5,
    AuxBL = 6,
    AuxBR = 7,
    AuxBS = 8,
  };

  struct VoiceCursor {
    std::uint32_t loop = 0;
    std::uint32_t end = 0;
    std::uint32_t current = 0;
    std::uint16_t format = 0;
    std::int16_t gain = 0;
    std::uint16_t pred_scale = 0;
    std::int16_t yn1 = 0;
    std::int16_t yn2 = 0;
    std::uint16_t loop_pred_scale = 0;
    std::int16_t loop_yn1 = 0;
    std::int16_t loop_yn2 = 0;
    bool looping = false;
    bool stream = false;
    bool stopped = false;
    std::uint16_t loop_counter = 0;
  };

  using PbWords = std::array<std::uint16_t, kCanonicalPbWords>;
  using MixBuffer = std::array<std::int32_t, kSamplesPerFrame>;
  using MixBuffers = std::array<MixBuffer, kChannelCount>;

  bool LooksLikeGcAx(const std::uint8_t* data, std::size_t size) const;
  static std::uint32_t Crc32(const std::uint8_t* data, std::size_t size);

  bool ReadMain(std::uint32_t address, void* dst, std::size_t size) const;
  bool WriteMain(std::uint32_t address, const void* src, std::size_t size);
  bool ReadBe16(std::uint32_t address, std::uint16_t* out) const;
  bool ReadBe32(std::uint32_t address, std::uint32_t* out) const;
  bool ReadBeS32(std::uint32_t address, std::int32_t* out) const;
  bool WriteBe16(std::uint32_t address, std::uint16_t value);
  bool WriteBe32(std::uint32_t address, std::uint32_t value);
  bool WriteBeS32(std::uint32_t address, std::int32_t value);
  bool ReadBeS32Block(std::uint32_t address, std::int32_t* out, std::size_t count) const;
  bool WriteBeS32Block(std::uint32_t address, const std::int32_t* values, std::size_t count);

  void ClearMixBuffers();
  bool ProcessCommandList(std::uint32_t address, std::uint16_t byte_size);
  bool LoadCommandList(std::uint32_t address, std::uint16_t byte_size,
                       std::array<std::uint16_t, 512>* words, std::size_t* word_count) const;
  void SetupProcessing(std::uint32_t init_address);
  void DownloadAndMix(std::uint32_t address, std::uint16_t main_volume,
                      std::uint16_t aux_a_volume, std::uint16_t aux_b_volume);
  void UploadLrs(std::uint32_t address);
  void SetMainLr(std::uint32_t address);
  void MixAux(std::size_t aux_base, std::uint32_t write_address, std::uint32_t read_address,
              bool write_back);
  void OutputSamples(std::uint32_t stereo_address, std::uint32_t surround_address);

  bool ReadPb(std::uint32_t address, PbWords* pb) const;
  bool WritePb(std::uint32_t address, const PbWords& pb);
  void ApplyPbUpdates(std::uint32_t millisecond, PbWords* pb) const;
  void ProcessPbList(std::uint32_t first_pb);
  void ProcessVoice(PbWords* pb, std::size_t output_offset, std::size_t count);
  std::int16_t ReadVoiceSample(VoiceCursor* cursor, const PbWords& pb);
  void HandleVoiceEnd(VoiceCursor* cursor, PbWords* pb);
  void ResampleVoice(VoiceCursor* cursor, PbWords* pb, std::int16_t* out, std::size_t count);
  void MixVoiceChannel(MixBuffer* out, const std::int16_t* input, std::size_t output_offset,
                       std::size_t count, std::uint16_t* volume, std::uint16_t volume_delta,
                       bool ramp, std::int16_t* dpop);

  static std::int16_t ClampS16(std::int64_t value);
  static std::int32_t ClampS32(std::int64_t value);
  static std::uint32_t Join(std::uint16_t hi, std::uint16_t lo) {
    return (static_cast<std::uint32_t>(hi) << 16u) | lo;
  }

  NativeDSP* dsp_ = nullptr;
  bool active_ = false;
  bool old_no_lpf_layout_ = false;
  MailState mail_state_ = MailState::WaitingForCommandSize;
  std::uint16_t command_list_bytes_ = 0;
  std::uint64_t completion_cycles_ = 0;
  std::uint32_t ucode_crc32_ = 0;
  bool output_reported_ = false;
  MixBuffers mix_{};

  std::uint64_t command_lists_ = 0;
  std::uint64_t voices_mixed_ = 0;
  std::uint64_t output_frames_ = 0;
};

} // namespace GekkoAOT::DSP
