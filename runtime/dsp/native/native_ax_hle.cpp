// SPDX-License-Identifier: GPL-3.0-or-later
#include "dsp/native/native_ax_hle.h"

#include "dsp/native/native_dsp.h"
#include "dsp/native/ax_mix_kernels.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <vector>

namespace GekkoAOT::DSP {
namespace {
constexpr std::uint32_t kAxCommandPrefix = 0xBABE0000u;
constexpr std::uint32_t kAxCommandMask = 0xffff0000u;
constexpr std::uint32_t kTaskMailMask = 0xffff0000u;
constexpr std::uint32_t kTaskMailToDsp = 0xCDD10000u;
constexpr std::uint32_t kMailResume = kTaskMailToDsp | 0x0000u;
constexpr std::uint32_t kMailNewUcode = kTaskMailToDsp | 0x0001u;
constexpr std::uint32_t kMailReset = kTaskMailToDsp | 0x0002u;
constexpr std::uint32_t kMailContinue = kTaskMailToDsp | 0x0003u;
constexpr std::uint32_t kDspResume = 0xDCD10001u;
constexpr std::uint32_t kDspYield = 0xDCD10002u;
constexpr std::uint64_t kAxCompletionPpcCycles = 2500u;

// Canonical AXPB word offsets. The original 0x4e8a8b21/6a696ce7 AX uCode
// omits words [93, 97) (LPF) from its MRAM representation.
constexpr std::size_t PB_NEXT_HI = 0;
constexpr std::size_t PB_NEXT_LO = 1;
constexpr std::size_t PB_SRC_TYPE = 4;
constexpr std::size_t PB_COEF_SELECT = 5;
constexpr std::size_t PB_MIXER_CONTROL = 6;
constexpr std::size_t PB_RUNNING = 7;
constexpr std::size_t PB_IS_STREAM = 8;
constexpr std::size_t PB_MIXER = 9;      // 18 words
constexpr std::size_t PB_UPDATES = 34;   // counts[5], address hi/lo
constexpr std::size_t PB_DPOP = 41;      // 9 signed words
constexpr std::size_t PB_VOL_ENV = 50;   // current, delta
constexpr std::size_t PB_AUDIO = 55;     // 8 words
constexpr std::size_t PB_ADPCM = 63;     // coefs[16], gain, pred, yn1, yn2
constexpr std::size_t PB_SRC = 83;       // ratio hi/lo, frac, last[4]
constexpr std::size_t PB_LOOP = 90;      // pred, yn1, yn2
constexpr std::size_t PB_LPF = 93;       // 4 words
constexpr std::size_t PB_LOOP_COUNTER = 97;

constexpr std::uint16_t FORMAT_ADPCM = 0x0000u;
constexpr std::uint16_t FORMAT_PCM16 = 0x000au;
constexpr std::uint16_t FORMAT_PCM8 = 0x0019u;
constexpr std::uint16_t SRC_POLYPHASE = 0u;
constexpr std::uint16_t SRC_LINEAR = 1u;
constexpr std::uint16_t SRC_NEAREST = 2u;

constexpr std::uint16_t CMD_SETUP = 0x00u;
constexpr std::uint16_t CMD_DL_AND_VOL_MIX = 0x01u;
constexpr std::uint16_t CMD_PB_ADDR = 0x02u;
constexpr std::uint16_t CMD_PROCESS = 0x03u;
constexpr std::uint16_t CMD_MIX_AUXA = 0x04u;
constexpr std::uint16_t CMD_MIX_AUXB = 0x05u;
constexpr std::uint16_t CMD_UPLOAD_LRS = 0x06u;
constexpr std::uint16_t CMD_SET_LR = 0x07u;
constexpr std::uint16_t CMD_UNK_08 = 0x08u;
constexpr std::uint16_t CMD_MIX_AUXB_NOWRITE = 0x09u;
constexpr std::uint16_t CMD_UNK_0A = 0x0au;
constexpr std::uint16_t CMD_UNK_0B = 0x0bu;
constexpr std::uint16_t CMD_UNK_0C = 0x0cu;
constexpr std::uint16_t CMD_MORE = 0x0du;
constexpr std::uint16_t CMD_OUTPUT = 0x0eu;
constexpr std::uint16_t CMD_END = 0x0fu;
constexpr std::uint16_t CMD_MIX_AUXB_LR = 0x10u;
constexpr std::uint16_t CMD_SET_OPPOSITE_LR = 0x11u;
constexpr std::uint16_t CMD_COMPRESSOR = 0x12u;
constexpr std::uint16_t CMD_SEND_AUX_AND_MIX = 0x13u;

std::uint16_t LoadBe16(const std::uint8_t* p) {
  return static_cast<std::uint16_t>((static_cast<std::uint16_t>(p[0]) << 8u) | p[1]);
}

void StoreBe16(std::uint8_t* p, std::uint16_t value) {
  p[0] = static_cast<std::uint8_t>(value >> 8u);
  p[1] = static_cast<std::uint8_t>(value);
}

std::uint32_t LoadBe32(const std::uint8_t* p) {
  return (static_cast<std::uint32_t>(p[0]) << 24u) |
         (static_cast<std::uint32_t>(p[1]) << 16u) |
         (static_cast<std::uint32_t>(p[2]) << 8u) |
         static_cast<std::uint32_t>(p[3]);
}

void StoreBe32(std::uint8_t* p, std::uint32_t value) {
  p[0] = static_cast<std::uint8_t>(value >> 24u);
  p[1] = static_cast<std::uint8_t>(value >> 16u);
  p[2] = static_cast<std::uint8_t>(value >> 8u);
  p[3] = static_cast<std::uint8_t>(value);
}
} // namespace

void NativeAXHLE::Reset() {
  active_ = false;
  old_no_lpf_layout_ = false;
  mail_state_ = MailState::WaitingForCommandSize;
  command_list_bytes_ = 0;
  completion_cycles_ = 0;
  ucode_crc32_ = 0;
  output_reported_ = false;
  ClearMixBuffers();
  command_lists_ = 0;
  voices_mixed_ = 0;
  output_frames_ = 0;
}

std::uint32_t NativeAXHLE::Crc32(const std::uint8_t* data, std::size_t size) {
  std::uint32_t crc = 0xffffffffu;
  for (std::size_t i = 0; i < size; ++i) {
    crc ^= data[i];
    for (unsigned bit = 0; bit < 8u; ++bit)
      crc = (crc >> 1u) ^ (0xedb88320u & (0u - (crc & 1u)));
  }
  return crc ^ 0xffffffffu;
}

bool NativeAXHLE::LooksLikeGcAx(const std::uint8_t* data, std::size_t size) const {
  if (!data || size < 0x80u) return false;
  bool has_babe = false;
  bool has_dcd1 = false;
  bool has_dsp_mail_io = false;
  for (std::size_t i = 0; i + 3u < size; i += 2u) {
    const std::uint16_t op = LoadBe16(data + i);
    const std::uint16_t imm = LoadBe16(data + i + 2u);
    if (op == 0x009fu && imm == 0xbabeu) has_babe = true;       // lri AC1.M,#BABE
    if (op == 0x16fcu && imm == 0xdcd1u) has_dcd1 = true;       // si DMBH,#DCD1
    if (op == 0x26feu || op == 0x26fcu) has_dsp_mail_io = true; // CMBH/DMBH poll
  }
  return has_babe && has_dcd1 && has_dsp_mail_io;
}

bool NativeAXHLE::ProbeTask(std::uint32_t code_address, std::uint32_t code_size) {
  Reset();
  if (!dsp_) return false;

  std::size_t probe_size = code_size;
  if (probe_size < 0x100u) probe_size = 0x2000u;
  probe_size = std::min<std::size_t>(probe_size, 0x10000u);
  std::vector<std::uint8_t> code(probe_size);
  if (!dsp_->CoreReadMain(code_address, code.data(), static_cast<std::uint32_t>(code.size())))
    return false;
  if (!LooksLikeGcAx(code.data(), code.size())) return false;

  ucode_crc32_ = Crc32(code.data(), code_size != 0u ? std::min<std::size_t>(code_size, code.size()) : code.size());
  // The classic AX image used by NDDemo/Monkey Ball/Crazy Taxi is documented
  // as DSP_UC_6A696CE7 and is the sole GC AX PB layout that omits the LPF words.
  old_no_lpf_layout_ = (ucode_crc32_ == 0x6a696ce7u);
  active_ = true;
  mail_state_ = MailState::WaitingForCommandSize;
  std::fprintf(stderr,
               "GEKKOAOT_NATIVE_AX_HLE_V50=1 ucode_crc32=%08x layout=%s probe=babe+dcd1 "
               "rom_required=0 lle_fallback=1\n",
               ucode_crc32_, old_no_lpf_layout_ ? "gc-ax-old-no-lpf" : "gc-ax");
  return true;
}

bool NativeAXHLE::ReadMain(std::uint32_t address, void* dst, std::size_t size) const {
  if (!dsp_ || !dst || size == 0u || size > std::numeric_limits<std::uint32_t>::max()) return false;
  return dsp_->CoreReadMain(address, static_cast<std::uint8_t*>(dst), static_cast<std::uint32_t>(size));
}

bool NativeAXHLE::WriteMain(std::uint32_t address, const void* src, std::size_t size) {
  if (!dsp_ || !src || size == 0u || size > std::numeric_limits<std::uint32_t>::max()) return false;
  return dsp_->CoreWriteMain(address, static_cast<const std::uint8_t*>(src), static_cast<std::uint32_t>(size));
}

bool NativeAXHLE::ReadBe16(std::uint32_t address, std::uint16_t* out) const {
  std::uint8_t b[2]{};
  if (!out || !ReadMain(address, b, sizeof(b))) return false;
  *out = LoadBe16(b);
  return true;
}

bool NativeAXHLE::ReadBe32(std::uint32_t address, std::uint32_t* out) const {
  std::uint8_t b[4]{};
  if (!out || !ReadMain(address, b, sizeof(b))) return false;
  *out = LoadBe32(b);
  return true;
}

bool NativeAXHLE::ReadBeS32(std::uint32_t address, std::int32_t* out) const {
  std::uint32_t v = 0;
  if (!out || !ReadBe32(address, &v)) return false;
  *out = static_cast<std::int32_t>(v);
  return true;
}

bool NativeAXHLE::WriteBe16(std::uint32_t address, std::uint16_t value) {
  std::uint8_t b[2];
  StoreBe16(b, value);
  return WriteMain(address, b, sizeof(b));
}

bool NativeAXHLE::WriteBe32(std::uint32_t address, std::uint32_t value) {
  std::uint8_t b[4];
  StoreBe32(b, value);
  return WriteMain(address, b, sizeof(b));
}

bool NativeAXHLE::WriteBeS32(std::uint32_t address, std::int32_t value) {
  return WriteBe32(address, static_cast<std::uint32_t>(value));
}

bool NativeAXHLE::ReadBeS32Block(std::uint32_t address, std::int32_t* out,
                                     std::size_t count) const {
  if (!out || count == 0u) return count == 0u;
  std::vector<std::uint8_t> raw(count * sizeof(std::int32_t));
  if (!ReadMain(address, raw.data(), raw.size())) return false;
  for (std::size_t i = 0; i < count; ++i)
    out[i] = static_cast<std::int32_t>(LoadBe32(raw.data() + i * 4u));
  return true;
}

bool NativeAXHLE::WriteBeS32Block(std::uint32_t address, const std::int32_t* values,
                                      std::size_t count) {
  if (!values || count == 0u) return count == 0u;
  std::vector<std::uint8_t> raw(count * sizeof(std::int32_t));
  for (std::size_t i = 0; i < count; ++i)
    StoreBe32(raw.data() + i * 4u, static_cast<std::uint32_t>(values[i]));
  return WriteMain(address, raw.data(), raw.size());
}

std::int16_t NativeAXHLE::ClampS16(std::int64_t value) {
  return static_cast<std::int16_t>(std::clamp<std::int64_t>(value, -32768, 32767));
}

std::int32_t NativeAXHLE::ClampS32(std::int64_t value) {
  return static_cast<std::int32_t>(std::clamp<std::int64_t>(
      value, std::numeric_limits<std::int32_t>::min(), std::numeric_limits<std::int32_t>::max()));
}

void NativeAXHLE::ClearMixBuffers() {
  for (auto& channel : mix_) channel.fill(0);
}

bool NativeAXHLE::LoadCommandList(std::uint32_t address, std::uint16_t byte_size,
                                  std::array<std::uint16_t, 512>* words,
                                  std::size_t* word_count) const {
  if (!words || !word_count || byte_size == 0u || (byte_size & 1u) != 0u || byte_size > 1024u)
    return false;
  std::array<std::uint8_t, 1024> raw{};
  if (!ReadMain(address, raw.data(), byte_size)) return false;
  *word_count = byte_size / 2u;
  for (std::size_t i = 0; i < *word_count; ++i) (*words)[i] = LoadBe16(raw.data() + i * 2u);
  return true;
}

void NativeAXHLE::SetupProcessing(std::uint32_t init_address) {
  // AX setup supplies (s32 initial, s16 delta) for each of the nine 5 ms buffers.
  // Pull the descriptor in one guest-memory transaction; per-word callbacks
  // dominated this command despite the actual ramp being trivial host work.
  std::array<std::uint8_t, kChannelCount * 6u> raw{};
  if (!ReadMain(init_address, raw.data(), raw.size())) {
    ClearMixBuffers();
    return;
  }
  for (std::size_t ch = 0; ch < kChannelCount; ++ch) {
    const std::uint8_t* entry = raw.data() + ch * 6u;
    std::int64_t value = static_cast<std::int32_t>(
        (static_cast<std::uint32_t>(LoadBe16(entry + 0u)) << 16u) |
        LoadBe16(entry + 2u));
    const auto delta = static_cast<std::int16_t>(LoadBe16(entry + 4u));
    for (std::size_t i = 0; i < kSamplesPerFrame; ++i) {
      mix_[ch][i] = ClampS32(value);
      value += delta;
    }
  }
}

void NativeAXHLE::DownloadAndMix(std::uint32_t address, std::uint16_t main_volume,
                                 std::uint16_t aux_a_volume, std::uint16_t aux_b_volume) {
  const std::array<std::uint16_t, 3> volume{main_volume, aux_a_volume, aux_b_volume};
  std::array<std::int32_t, 3u * kSamplesPerFrame> source{};
  if (!ReadBeS32Block(address, source.data(), source.size())) return;
  for (std::size_t group = 0; group < 3u; ++group) {
    for (std::size_t channel = 0; channel < 3u; ++channel) {
      const std::size_t dst_channel = group * 3u + channel;
      const std::int32_t* src = source.data() + channel * kSamplesPerFrame;
      for (std::size_t i = 0; i < kSamplesPerFrame; ++i) {
        const std::int64_t scaled =
            (static_cast<std::int64_t>(src[i]) * volume[group]) >> 15;
        mix_[dst_channel][i] = ClampS32(
            static_cast<std::int64_t>(mix_[dst_channel][i]) + scaled);
      }
    }
  }
}

void NativeAXHLE::UploadLrs(std::uint32_t address) {
  std::array<std::int32_t, 3u * kSamplesPerFrame> packed{};
  for (std::size_t ch = 0; ch < 3u; ++ch)
    std::copy(mix_[ch].begin(), mix_[ch].end(), packed.begin() + ch * kSamplesPerFrame);
  (void)WriteBeS32Block(address, packed.data(), packed.size());
}

void NativeAXHLE::SetMainLr(std::uint32_t address) {
  // AX CMD_SET_LR downloads ONE mono 5 ms buffer and mirrors it to both main channels.
  std::array<std::int32_t, kSamplesPerFrame> mono{};
  if (!ReadBeS32Block(address, mono.data(), mono.size())) return;
  std::copy(mono.begin(), mono.end(), mix_[MainL].begin());
  std::copy(mono.begin(), mono.end(), mix_[MainR].begin());
  mix_[MainS].fill(0);
}

void NativeAXHLE::MixAux(std::size_t aux_base, std::uint32_t write_address,
                         std::uint32_t read_address, bool write_back) {
  if (aux_base + 2u >= kChannelCount) return;
  std::array<std::int32_t, 3u * kSamplesPerFrame> packed{};
  if (write_back && write_address != 0u) {
    for (std::size_t ch = 0; ch < 3u; ++ch)
      std::copy(mix_[aux_base + ch].begin(), mix_[aux_base + ch].end(),
                packed.begin() + ch * kSamplesPerFrame);
    (void)WriteBeS32Block(write_address, packed.data(), packed.size());
  }
  if (read_address != 0u && ReadBeS32Block(read_address, packed.data(), packed.size())) {
    for (std::size_t ch = 0; ch < 3u; ++ch)
      for (std::size_t i = 0; i < kSamplesPerFrame; ++i)
        mix_[ch][i] = ClampS32(static_cast<std::int64_t>(mix_[ch][i]) +
                               packed[ch * kSamplesPerFrame + i]);
  }
}

void NativeAXHLE::OutputSamples(std::uint32_t stereo_address, std::uint32_t surround_address) {
  std::array<std::uint8_t, kSamplesPerFrame * 4u> stereo_raw{};
  std::array<std::int32_t, kSamplesPerFrame> surround{};
  for (std::size_t i = 0; i < kSamplesPerFrame; ++i) {
    const auto left = ClampS16(mix_[MainL][i]);
    const auto right = ClampS16(mix_[MainR][i]);
    // GameCube AID buffers are R,L interleaved, big-endian. Pack the complete
    // 5 ms frame locally and cross the guest-memory boundary once.
    StoreBe16(stereo_raw.data() + i * 4u + 0u, static_cast<std::uint16_t>(right));
    StoreBe16(stereo_raw.data() + i * 4u + 2u, static_cast<std::uint16_t>(left));
    surround[i] = mix_[MainS][i];
  }
  (void)WriteMain(stereo_address, stereo_raw.data(), stereo_raw.size());
  if (surround_address != 0u)
    (void)WriteBeS32Block(surround_address, surround.data(), surround.size());

  output_frames_ += kSamplesPerFrame;
  if (!output_reported_) {
    output_reported_ = true;
    std::fprintf(stderr,
                 "GEKKOAOT_NATIVE_AX_AUDIO_V52=1 frames_per_list=%zu rate_hz=32000 "
                 "aid_layout=be-r-l mram=bulk mix=simd-when-exact src=rom-free-4tap-v51.5\n",
                 kSamplesPerFrame);
  }
}

bool NativeAXHLE::ReadPb(std::uint32_t address, PbWords* pb) const {
  if (!pb) return false;
  pb->fill(0);
  const std::size_t guest_words = old_no_lpf_layout_ ? kOldPbWords : kCanonicalPbWords;
  std::array<std::uint8_t, kCanonicalPbWords * 2u> raw{};
  if (!ReadMain(address, raw.data(), guest_words * 2u)) return false;

  if (!old_no_lpf_layout_) {
    for (std::size_t i = 0; i < kCanonicalPbWords; ++i) (*pb)[i] = LoadBe16(raw.data() + i * 2u);
    return true;
  }

  for (std::size_t i = 0; i < PB_LPF; ++i) (*pb)[i] = LoadBe16(raw.data() + i * 2u);
  for (std::size_t i = PB_LPF; i < PB_LOOP_COUNTER; ++i) (*pb)[i] = 0u;
  for (std::size_t i = PB_LOOP_COUNTER; i < kCanonicalPbWords; ++i) {
    const std::size_t guest_i = i - 4u;
    (*pb)[i] = LoadBe16(raw.data() + guest_i * 2u);
  }
  return true;
}

bool NativeAXHLE::WritePb(std::uint32_t address, const PbWords& pb) {
  const std::size_t guest_words = old_no_lpf_layout_ ? kOldPbWords : kCanonicalPbWords;
  std::array<std::uint8_t, kCanonicalPbWords * 2u> raw{};
  if (!old_no_lpf_layout_) {
    for (std::size_t i = 0; i < kCanonicalPbWords; ++i) StoreBe16(raw.data() + i * 2u, pb[i]);
  } else {
    for (std::size_t i = 0; i < PB_LPF; ++i) StoreBe16(raw.data() + i * 2u, pb[i]);
    for (std::size_t i = PB_LOOP_COUNTER; i < kCanonicalPbWords; ++i) {
      const std::size_t guest_i = i - 4u;
      StoreBe16(raw.data() + guest_i * 2u, pb[i]);
    }
  }
  return WriteMain(address, raw.data(), guest_words * 2u);
}

void NativeAXHLE::ApplyPbUpdates(std::uint32_t millisecond, PbWords* pb) const {
  if (!pb || millisecond >= 5u) return;
  const std::uint16_t count = (*pb)[PB_UPDATES + millisecond];
  if (count == 0u) return;
  const std::uint32_t updates_address = Join((*pb)[PB_UPDATES + 5u], (*pb)[PB_UPDATES + 6u]);
  std::uint32_t start = 0;
  for (std::uint32_t i = 0; i < millisecond; ++i) start += (*pb)[PB_UPDATES + i];
  const std::uint32_t capped = std::min<std::uint32_t>(count, 32u - std::min<std::uint32_t>(start, 32u));
  for (std::uint32_t i = 0; i < capped; ++i) {
    std::uint16_t off = 0, value = 0;
    const std::uint32_t entry = updates_address + (start + i) * 4u;
    if (!ReadBe16(entry, &off) || !ReadBe16(entry + 2u, &value)) break;
    // PB update offsets are DSP/canonical AXPB word offsets even for the
    // old MRAM layout that omits the LPF words. ReadPb() already expands that
    // compact layout into the canonical structure, so shifting updates here
    // a second time corrupts loop-counter/padding state.
    const std::size_t logical = off;
    if (logical < pb->size()) (*pb)[logical] = value;
  }
}

void NativeAXHLE::HandleVoiceEnd(VoiceCursor* cursor, PbWords* pb) {
  if (!cursor || !pb) return;
  if (!cursor->looping) {
    cursor->stopped = true;
    (*pb)[PB_RUNNING] = 0u;
    return;
  }

  // The accelerator has already wrapped its current address to the programmed
  // start/loop address before raising the end exception.  The exception then
  // restores the loop ADPCM history.  Streaming voices intentionally keep
  // YN1/YN2 and only bump the loop counter.
  cursor->stopped = false;
  cursor->current = cursor->loop;
  cursor->pred_scale = cursor->loop_pred_scale & 0x7fu;
  if (!cursor->stream) {
    cursor->yn1 = cursor->loop_yn1;
    cursor->yn2 = cursor->loop_yn2;
  } else {
    ++cursor->loop_counter;
  }
}

std::int16_t NativeAXHLE::ReadVoiceSample(VoiceCursor* c, const PbWords& pb) {
  if (!dsp_ || !c || c->stopped) return 0;

  if (c->format == FORMAT_ADPCM) {
    // Match the Flipper DSP accelerator ordering rather than treating every
    // 16-nibble boundary as a header before the sample read.  The PB current
    // address already points at the next sample nibble; after consuming it the
    // accelerator advances, handles the two block-end special cases, and only
    // then fetches a new predictor/scale header when it lands on a 16-nibble
    // boundary.  Getting this order wrong produces a discontinuity every AX
    // subframe, heard as the repeating "bop"/buzz fixed by v51.1.
    const std::uint8_t packed = dsp_->ReadAram(c->current >> 1u);
    std::int32_t nibble = (c->current & 1u) ? (packed & 0x0fu) : (packed >> 4u);
    if (nibble >= 8) nibble -= 16;

    const std::uint32_t predictor = (c->pred_scale >> 4u) & 7u;
    const std::int32_t coef1 = static_cast<std::int16_t>(pb[PB_ADPCM + predictor * 2u + 0u]);
    const std::int32_t coef2 = static_cast<std::int16_t>(pb[PB_ADPCM + predictor * 2u + 1u]);
    const std::int32_t scale = 1 << (c->pred_scale & 0x0fu);
    const std::int32_t decoded = scale * nibble +
        ((0x400 + coef1 * static_cast<std::int32_t>(c->yn1) +
          coef2 * static_cast<std::int32_t>(c->yn2)) >> 11);
    const std::int16_t sample = ClampS16(decoded);
    c->yn2 = c->yn1;
    c->yn1 = sample;

    std::uint32_t step_size = 2u;
    ++c->current;

    // Hardware has two aligned-end cases where it wraps without raising the
    // normal accelerator end exception and without loading a new header.
    bool special_wrap = false;
    if ((c->end & 0x0fu) == 0u && c->current == c->end) {
      c->current = c->loop + 1u;
      special_wrap = true;
    } else if ((c->end & 0x0fu) == 1u && c->current == c->end - 1u) {
      c->current = c->loop;
      special_wrap = true;
    } else if ((c->current & 0x0fu) == 0u) {
      c->pred_scale = dsp_->ReadAram((c->current & ~0x0fu) >> 1u) & 0x7fu;
      c->current += 2u;
      step_size += 2u;
    }

    if (!special_wrap && c->current == c->end + step_size - 1u) {
      c->current = c->loop;
      c->stopped = true;
      HandleVoiceEnd(c, const_cast<PbWords*>(&pb));
    }
    return sample;
  }

  std::int32_t raw = 0;
  if (c->format == FORMAT_PCM8) {
    raw = dsp_->ReadAram(c->current);
    ++c->current;
  } else if (c->format == FORMAT_PCM16) {
    const std::uint32_t byte = (c->current * 2u) & NativeDSP::AramMask;
    raw = static_cast<std::int16_t>((static_cast<std::uint16_t>(dsp_->ReadAram(byte)) << 8u) |
                                    dsp_->ReadAram(byte + 1u));
    ++c->current;
  } else {
    // Unknown accelerator mode: silence the voice rather than poisoning the mix.
    c->stopped = true;
    return 0;
  }

  const std::uint32_t gain_shift = (c->format == FORMAT_PCM16) ? 11u : 0u;
  const std::int32_t coef1 = static_cast<std::int16_t>(pb[PB_ADPCM + 0u]);
  const std::int32_t coef2 = static_cast<std::int16_t>(pb[PB_ADPCM + 1u]);
  const std::int64_t decoded =
      ((static_cast<std::int64_t>(c->gain) * raw) >> gain_shift) +
      ((static_cast<std::int64_t>(coef1) * c->yn1) >> gain_shift) +
      ((static_cast<std::int64_t>(coef2) * c->yn2) >> gain_shift);
  const auto sample = static_cast<std::int16_t>(decoded);
  c->yn2 = c->yn1;
  c->yn1 = sample;

  // PCM accelerator addresses advance one sample at a time and raise the end
  // exception when the post-increment address reaches end+1.
  if (c->current == c->end + 1u) {
    c->current = c->loop;
    c->stopped = true;
    HandleVoiceEnd(c, const_cast<PbWords*>(&pb));
  }
  return sample;
}

void NativeAXHLE::ResampleVoice(VoiceCursor* cursor, PbWords* pb, std::int16_t* out,
                                std::size_t count) {
  if (!cursor || !pb || !out || count == 0u) return;
  const std::uint32_t ratio = Join((*pb)[PB_SRC + 0u], (*pb)[PB_SRC + 1u]);
  std::uint32_t phase = (*pb)[PB_SRC + 2u];
  std::array<std::int16_t, 4> ring{
      static_cast<std::int16_t>((*pb)[PB_SRC + 3u]),
      static_cast<std::int16_t>((*pb)[PB_SRC + 4u]),
      static_cast<std::int16_t>((*pb)[PB_SRC + 5u]),
      static_cast<std::int16_t>((*pb)[PB_SRC + 6u]),
  };
  std::size_t head = 0u;
  const std::uint16_t src_type = (*pb)[PB_SRC_TYPE];

  if (src_type == SRC_NEAREST) {
    for (std::size_t i = 0; i < count; ++i) out[i] = ReadVoiceSample(cursor, *pb);
    if (count >= 4u) {
      for (std::size_t i = 0; i < 4u; ++i) ring[i] = out[count - 4u + i];
    }
  } else {
    for (std::size_t i = 0; i < count; ++i) {
      phase += ratio;
      while (phase >= 0x10000u) {
        ring[head] = ReadVoiceSample(cursor, *pb);
        head = (head + 1u) & 3u;
        phase -= 0x10000u;
      }

      const std::uint32_t frac = phase & 0xffffu;
      if (src_type == SRC_POLYPHASE) {
        // v51.5: ROM-free four-tap interpolation for AX polyphase voices.
        //
        // Retail AX uses four DROM coefficients selected from 128 fractional
        // phases.  We deliberately do not ship/copy Nintendo's coefficient
        // ROM.  Instead generate an independent 4-point Lagrange kernel over
        // the same four-sample history and quantise the phase to 128 steps.
        // It preserves the AX four-tap state/phase behaviour and removes the
        // harsh aliasing produced by the old two-tap linear fallback.
        const std::uint32_t phase7 = frac >> 9u;
        const double x = static_cast<double>(phase7) / 128.0;
        const double c0 = -((x - 1.0) * (x - 2.0) * (x - 3.0)) / 6.0;
        const double c1 =  (x * (x - 2.0) * (x - 3.0)) / 2.0;
        const double c2 = -(x * (x - 1.0) * (x - 3.0)) / 2.0;
        const double c3 =  (x * (x - 1.0) * (x - 2.0)) / 6.0;
        const double mixed =
            static_cast<double>(ring[(head + 0u) & 3u]) * c0 +
            static_cast<double>(ring[(head + 1u) & 3u]) * c1 +
            static_cast<double>(ring[(head + 2u) & 3u]) * c2 +
            static_cast<double>(ring[(head + 3u) & 3u]) * c3;
        out[i] = ClampS16(static_cast<std::int64_t>(std::llround(mixed)));
      } else {
        const auto a = static_cast<std::int32_t>(ring[head]);
        const auto b = static_cast<std::int32_t>(ring[(head + 1u) & 3u]);
        out[i] = static_cast<std::int16_t>(
            (static_cast<std::int64_t>(a) * (0x10000u - frac) +
             static_cast<std::int64_t>(b) * frac) >> 16u);
      }
    }
  }

  (*pb)[PB_SRC + 2u] = static_cast<std::uint16_t>(phase);
  if (src_type == SRC_NEAREST) {
    for (std::size_t i = 0; i < 4u; ++i)
      (*pb)[PB_SRC + 3u + i] = static_cast<std::uint16_t>(ring[i]);
  } else {
    // `head` points at the oldest entry after all producer writes.  Persist
    // the history in temporal order, not the backing array's physical order.
    // The old code stored ring[0..3] directly; once the ring rotated, the next
    // 1 ms AX subframe started with shuffled history and generated a periodic
    // discontinuity/click.
    for (std::size_t i = 0; i < 4u; ++i)
      (*pb)[PB_SRC + 3u + i] = static_cast<std::uint16_t>(ring[(head + i) & 3u]);
  }
}

void NativeAXHLE::MixVoiceChannel(MixBuffer* out, const std::int16_t* input,
                                  std::size_t output_offset, std::size_t count,
                                  std::uint16_t* volume, std::uint16_t volume_delta,
                                  bool ramp, std::int16_t* dpop) {
  if (!out || !input || !volume || output_offset + count > out->size()) return;
  const auto mixed = AXKernels::MixVoice(out->data() + output_offset, input, count,
                                         *volume, volume_delta, ramp);
  *volume = mixed.volume;
  if (dpop) *dpop = mixed.last;
}

void NativeAXHLE::ProcessVoice(PbWords* pb, std::size_t output_offset, std::size_t count) {
  if (!pb || (*pb)[PB_RUNNING] != 1u || output_offset + count > kSamplesPerFrame) return;

  VoiceCursor cursor{};
  cursor.looping = (*pb)[PB_AUDIO + 0u] != 0u;
  cursor.format = (*pb)[PB_AUDIO + 1u];
  cursor.loop = Join((*pb)[PB_AUDIO + 2u], (*pb)[PB_AUDIO + 3u]);
  cursor.end = Join((*pb)[PB_AUDIO + 4u], (*pb)[PB_AUDIO + 5u]);
  cursor.current = Join((*pb)[PB_AUDIO + 6u], (*pb)[PB_AUDIO + 7u]);
  cursor.gain = static_cast<std::int16_t>((*pb)[PB_ADPCM + 16u]);
  cursor.pred_scale = (*pb)[PB_ADPCM + 17u];
  cursor.yn1 = static_cast<std::int16_t>((*pb)[PB_ADPCM + 18u]);
  cursor.yn2 = static_cast<std::int16_t>((*pb)[PB_ADPCM + 19u]);
  cursor.loop_pred_scale = (*pb)[PB_LOOP + 0u];
  cursor.loop_yn1 = static_cast<std::int16_t>((*pb)[PB_LOOP + 1u]);
  cursor.loop_yn2 = static_cast<std::int16_t>((*pb)[PB_LOOP + 2u]);
  cursor.stream = (*pb)[PB_IS_STREAM] == 1u;
  cursor.loop_counter = (*pb)[PB_LOOP_COUNTER];

  std::array<std::int16_t, 32> samples{};
  ResampleVoice(&cursor, pb, samples.data(), count);

  std::int16_t env = static_cast<std::int16_t>((*pb)[PB_VOL_ENV + 0u]);
  const std::int16_t env_delta = static_cast<std::int16_t>((*pb)[PB_VOL_ENV + 1u]);
  for (std::size_t i = 0; i < count; ++i) {
    samples[i] = ClampS16((static_cast<std::int64_t>(samples[i]) * env) >> 15u);
    env = static_cast<std::int16_t>(env + env_delta);
  }
  (*pb)[PB_VOL_ENV + 0u] = static_cast<std::uint16_t>(env);

  // Newer AX PBs can request the DSP one-pole low-pass filter.  The classic
  // 6a696ce7 layout omits these four words entirely, so its expanded PB keeps
  // PB_LPF == 0 and naturally bypasses this path.
  if ((*pb)[PB_LPF + 0u] != 0u) {
    std::int16_t yn1 = static_cast<std::int16_t>((*pb)[PB_LPF + 1u]);
    const std::uint16_t a0 = (*pb)[PB_LPF + 2u];
    const std::int16_t b0 = static_cast<std::int16_t>((*pb)[PB_LPF + 3u]);
    for (std::size_t i = 0; i < count; ++i) {
      const std::int64_t filtered =
          static_cast<std::int64_t>(a0) * samples[i] +
          static_cast<std::int64_t>(b0) * yn1;
      yn1 = ClampS16(filtered >> 15u);
      samples[i] = yn1;
    }
    (*pb)[PB_LPF + 1u] = static_cast<std::uint16_t>(yn1);
  }

  const std::uint16_t control = (*pb)[PB_MIXER_CONTROL];

  auto mix_slot = [&](Channel channel, std::size_t vol_index, std::size_t dpop_index,
                      bool enabled, bool ramp) {
    if (!enabled) return;
    std::uint16_t volume = (*pb)[PB_MIXER + vol_index * 2u + 0u];
    const std::uint16_t delta = (*pb)[PB_MIXER + vol_index * 2u + 1u];
    auto dpop = static_cast<std::int16_t>((*pb)[PB_DPOP + dpop_index]);
    MixVoiceChannel(&mix_[channel], samples.data(), output_offset, count, &volume, delta, ramp, &dpop);
    (*pb)[PB_MIXER + vol_index * 2u + 0u] = volume;
    (*pb)[PB_DPOP + dpop_index] = static_cast<std::uint16_t>(dpop);
  };

  // PB mixer order: MainL, MainR, AuxAL, AuxAR, AuxBL, AuxBR, AuxBS, MainS, AuxAS.
  if (old_no_lpf_layout_) {
    // Old 0x4e8a8b21/6a696ce7 mixer-control encoding.
    const bool dpl2 = (control & 0x0010u) != 0u;
    const bool aux_a_lr = dpl2 ? ((control & 0x0007u) == 1u) : ((control & 0x0001u) != 0u);
    const bool aux_b_lr = dpl2 ? ((control & 0x0006u) == 0u) : ((control & 0x0002u) != 0u);
    const bool surround = !dpl2 && ((control & 0x0004u) != 0u);
    const bool ramp_all = (control & 0x0008u) != 0u;

    mix_slot(MainL, 0u, 0u, true, ramp_all);
    mix_slot(MainR, 1u, 3u, true, ramp_all);
    mix_slot(AuxAL, 2u, 1u, aux_a_lr, ramp_all);
    mix_slot(AuxAR, 3u, 4u, aux_a_lr, ramp_all);
    mix_slot(AuxBL, 4u, 2u, aux_b_lr, ramp_all);
    mix_slot(AuxBR, 5u, 5u, aux_b_lr, ramp_all);
    mix_slot(MainS, 7u, 6u, surround, ramp_all);
    // DPL2 explicitly enables AUXA surround when mixer_control[2:0] == 1.
    mix_slot(AuxAS, 8u, 7u, (dpl2 && aux_a_lr) || (surround && aux_a_lr), ramp_all);
    mix_slot(AuxBS, 6u, 8u, surround && aux_b_lr, ramp_all);
  } else {
    // Newer GameCube AX mixer-control encoding.  Ramp bits are per channel
    // group instead of the single old-AX MIX_ALL_RAMPS flag.
    const bool main_l = (control & 0x0001u) != 0u;
    const bool main_r = (control & 0x0002u) != 0u;
    const bool main_s = (control & 0x0004u) != 0u;
    const bool main_ramp = (control & 0x0008u) != 0u;
    const bool auxa_l = (control & 0x0010u) != 0u;
    const bool auxa_r = (control & 0x0020u) != 0u;
    const bool auxa_lr_ramp = (control & 0x0040u) != 0u;
    const bool auxa_s = (control & 0x0080u) != 0u;
    const bool auxa_s_ramp = (control & 0x0100u) != 0u;
    const bool auxb_l = (control & 0x0200u) != 0u;
    const bool auxb_r = (control & 0x0400u) != 0u;
    const bool auxb_lr_ramp = (control & 0x0800u) != 0u;
    const bool auxb_s = (control & 0x1000u) != 0u;
    const bool auxb_s_ramp = (control & 0x2000u) != 0u;

    mix_slot(MainL, 0u, 0u, main_l, main_ramp);
    mix_slot(MainR, 1u, 3u, main_r, main_ramp);
    mix_slot(MainS, 7u, 6u, main_s, main_ramp);
    mix_slot(AuxAL, 2u, 1u, auxa_l, auxa_lr_ramp);
    mix_slot(AuxAR, 3u, 4u, auxa_r, auxa_lr_ramp);
    mix_slot(AuxAS, 8u, 7u, auxa_s, auxa_s_ramp);
    mix_slot(AuxBL, 4u, 2u, auxb_l, auxb_lr_ramp);
    mix_slot(AuxBR, 5u, 5u, auxb_r, auxb_lr_ramp);
    mix_slot(AuxBS, 6u, 8u, auxb_s, auxb_s_ramp);
  }

  (*pb)[PB_AUDIO + 6u] = static_cast<std::uint16_t>(cursor.current >> 16u);
  (*pb)[PB_AUDIO + 7u] = static_cast<std::uint16_t>(cursor.current);
  (*pb)[PB_ADPCM + 17u] = cursor.pred_scale;
  (*pb)[PB_ADPCM + 18u] = static_cast<std::uint16_t>(cursor.yn1);
  (*pb)[PB_ADPCM + 19u] = static_cast<std::uint16_t>(cursor.yn2);
  (*pb)[PB_LOOP_COUNTER] = cursor.loop_counter;
  ++voices_mixed_;
}

void NativeAXHLE::ProcessPbList(std::uint32_t first_pb) {
  std::uint32_t address = first_pb;
  static std::uint32_t state_trace_budget = 24u;
  for (std::uint32_t voice = 0; address != 0u && voice < 1024u; ++voice) {
    PbWords pb{};
    if (!ReadPb(address, &pb)) break;

    const bool was_running = pb[PB_RUNNING] == 1u;
    const std::uint32_t cur_before = Join(pb[PB_AUDIO + 6u], pb[PB_AUDIO + 7u]);
    const std::uint16_t phase_before = pb[PB_SRC + 2u];
    const std::int16_t yn1_before = static_cast<std::int16_t>(pb[PB_ADPCM + 18u]);
    const std::int16_t yn2_before = static_cast<std::int16_t>(pb[PB_ADPCM + 19u]);

    for (std::uint32_t ms = 0; ms < 5u; ++ms) {
      ApplyPbUpdates(ms, &pb);
      ProcessVoice(&pb, ms * 32u, 32u);
    }

    const std::uint32_t cur_after = Join(pb[PB_AUDIO + 6u], pb[PB_AUDIO + 7u]);
    const std::uint16_t phase_after = pb[PB_SRC + 2u];
    if (was_running && state_trace_budget != 0u) {
      std::fprintf(stderr,
                   "GEKKOAOT_NATIVE_AX_STATE_V51_4 pb=%08x fmt=%04x cur=%08x->%08x "
                   "phase=%04x->%04x yn=%d,%d->%d,%d running=%u\n",
                   address, pb[PB_AUDIO + 1u], cur_before, cur_after, phase_before, phase_after,
                   static_cast<int>(yn1_before), static_cast<int>(yn2_before),
                   static_cast<int>(static_cast<std::int16_t>(pb[PB_ADPCM + 18u])),
                   static_cast<int>(static_cast<std::int16_t>(pb[PB_ADPCM + 19u])),
                   static_cast<unsigned>(pb[PB_RUNNING]));
      --state_trace_budget;
    }

    WritePb(address, pb);
    // PB updates are allowed to touch any canonical PB word, including the
    // next pointer, so follow the post-processed value just like the uCode.
    const std::uint32_t next = Join(pb[PB_NEXT_HI], pb[PB_NEXT_LO]);
    if (next == address) break;
    address = next;
  }
}

bool NativeAXHLE::ProcessCommandList(std::uint32_t address, std::uint16_t byte_size) {
  std::array<std::uint16_t, 512> words{};
  std::size_t word_count = 0;
  if (!LoadCommandList(address, byte_size, &words, &word_count)) return false;

  ClearMixBuffers();
  std::uint32_t pb_address = 0u;
  std::size_t index = 0u;
  std::uint32_t chained_lists = 0u;
  std::uint32_t command_mask = 0u;

  while (index < word_count && chained_lists < 16u) {
    const std::uint16_t cmd = words[index++];
    if (cmd < 32u) command_mask |= (1u << cmd);
    auto take16 = [&](std::uint16_t* v) -> bool {
      if (!v || index >= word_count) return false;
      *v = words[index++];
      return true;
    };
    auto take32 = [&](std::uint32_t* v) -> bool {
      std::uint16_t hi = 0, lo = 0;
      if (!v || !take16(&hi) || !take16(&lo)) return false;
      *v = Join(hi, lo);
      return true;
    };

    switch (cmd) {
    case CMD_SETUP: {
      std::uint32_t init = 0;
      if (!take32(&init)) return false;
      SetupProcessing(init);
      break;
    }
    case CMD_DL_AND_VOL_MIX: {
      std::uint32_t src = 0;
      std::uint16_t main = 0, aux_a = 0, aux_b = 0;
      if (!take32(&src) || !take16(&main) || !take16(&aux_a) || !take16(&aux_b)) return false;
      DownloadAndMix(src, main, aux_a, aux_b);
      break;
    }
    case CMD_PB_ADDR:
      if (!take32(&pb_address)) return false;
      break;
    case CMD_PROCESS:
      ProcessPbList(pb_address);
      break;
    case CMD_MIX_AUXA:
    case CMD_MIX_AUXB: {
      std::uint32_t write = 0, read = 0;
      if (!take32(&write) || !take32(&read)) return false;
      MixAux(cmd == CMD_MIX_AUXA ? AuxAL : AuxBL, write, read, true);
      break;
    }
    case CMD_UPLOAD_LRS: {
      std::uint32_t dst = 0;
      if (!take32(&dst)) return false;
      UploadLrs(dst);
      break;
    }
    case CMD_SET_LR: {
      std::uint32_t src = 0;
      if (!take32(&src)) return false;
      SetMainLr(src);
      break;
    }
    case CMD_UNK_08:
      if (index + 10u > word_count) return false;
      index += 10u;
      break;
    case CMD_MIX_AUXB_NOWRITE: {
      std::uint32_t read = 0;
      if (!take32(&read)) return false;
      MixAux(AuxBL, 0u, read, false);
      break;
    }
    case CMD_UNK_0A:
    case CMD_UNK_0B:
    case CMD_UNK_0C:
      break;
    case CMD_MORE: {
      std::uint32_t next = 0;
      std::uint16_t next_size = 0;
      if (!take32(&next) || !take16(&next_size)) return false;
      if (!LoadCommandList(next, next_size, &words, &word_count)) return false;
      index = 0u;
      ++chained_lists;
      break;
    }
    case CMD_OUTPUT: {
      std::uint32_t surround = 0, stereo = 0;
      if (!take32(&surround) || !take32(&stereo)) return false;
      OutputSamples(stereo, surround);
      break;
    }
    case CMD_END: {
      ++command_lists_;
      static std::uint32_t last_command_mask = 0xffffffffu;
      if (command_mask != last_command_mask) {
        last_command_mask = command_mask;
        std::fprintf(stderr,
                     "GEKKOAOT_NATIVE_AX_COMMANDS_V51_4 mask=%08x lists=%llu\n",
                     command_mask, static_cast<unsigned long long>(command_lists_));
      }
      return true;
    }
    case CMD_MIX_AUXB_LR: {
      std::uint32_t write = 0, read = 0;
      if (!take32(&write) || !take32(&read)) return false;

      // Command 0x10 is NOT the regular 3-channel AUXB command: upload only
      // AUXB L/R, then replace AUXB L/R with the downloaded L/R while adding
      // those samples to MAIN L/R.
      for (std::size_t i = 0; i < kSamplesPerFrame; ++i) {
        WriteBeS32(write + static_cast<std::uint32_t>(i * 4u), mix_[AuxBL][i]);
        WriteBeS32(write + static_cast<std::uint32_t>((kSamplesPerFrame + i) * 4u),
                   mix_[AuxBR][i]);
      }
      for (std::size_t i = 0; i < kSamplesPerFrame; ++i) {
        std::int32_t l = 0, r = 0;
        if (!ReadBeS32(read + static_cast<std::uint32_t>(i * 4u), &l) ||
            !ReadBeS32(read + static_cast<std::uint32_t>((kSamplesPerFrame + i) * 4u), &r))
          break;
        mix_[AuxBL][i] = l;
        mix_[AuxBR][i] = r;
        mix_[MainL][i] = ClampS32(static_cast<std::int64_t>(mix_[MainL][i]) + l);
        mix_[MainR][i] = ClampS32(static_cast<std::int64_t>(mix_[MainR][i]) + r);
      }
      break;
    }
    case CMD_SET_OPPOSITE_LR: {
      std::uint32_t src = 0;
      if (!take32(&src)) return false;
      for (std::size_t i = 0; i < kSamplesPerFrame; ++i) {
        std::int32_t sample = 0;
        if (!ReadBeS32(src + static_cast<std::uint32_t>(i * 4u), &sample)) break;
        mix_[MainL][i] = ClampS32(-static_cast<std::int64_t>(sample));
        mix_[MainR][i] = sample;
        mix_[MainS][i] = 0;
      }
      break;
    }
    case CMD_COMPRESSOR:
      // Newer GC AX uses: threshold, release_frames, table_addr_hi, table_addr_lo.
      // The classic old uCode does not implement this command, but consuming
      // the correct four words prevents command-list desynchronization.
      if (index + 4u > word_count) return false;
      index += 4u;
      break;
    case CMD_SEND_AUX_AND_MIX: {
      std::uint32_t auxa_lrs_up = 0, auxb_s_up = 0;
      std::uint32_t main_l_dl = 0, main_r_dl = 0, auxb_l_dl = 0, auxb_r_dl = 0;
      if (!take32(&auxa_lrs_up) || !take32(&auxb_s_up) ||
          !take32(&main_l_dl) || !take32(&main_r_dl) ||
          !take32(&auxb_l_dl) || !take32(&auxb_r_dl))
        return false;

      const std::array<Channel, 3> auxa{AuxAL, AuxAR, AuxAS};
      for (std::size_t ch = 0; ch < auxa.size(); ++ch) {
        for (std::size_t i = 0; i < kSamplesPerFrame; ++i)
          WriteBeS32(auxa_lrs_up + static_cast<std::uint32_t>((ch * kSamplesPerFrame + i) * 4u),
                     mix_[auxa[ch]][i]);
      }
      for (std::size_t i = 0; i < kSamplesPerFrame; ++i)
        WriteBeS32(auxb_s_up + static_cast<std::uint32_t>(i * 4u), mix_[AuxBS][i]);

      const std::array<Channel, 4> channels{MainL, MainR, AuxBL, AuxBR};
      const std::array<std::uint32_t, 4> addrs{main_l_dl, main_r_dl, auxb_l_dl, auxb_r_dl};
      for (std::size_t ch = 0; ch < channels.size(); ++ch) {
        for (std::size_t i = 0; i < kSamplesPerFrame; ++i) {
          std::int32_t sample = 0;
          if (!ReadBeS32(addrs[ch] + static_cast<std::uint32_t>(i * 4u), &sample)) break;
          mix_[channels[ch]][i] = ClampS32(
              static_cast<std::int64_t>(mix_[channels[ch]][i]) + sample);
        }
      }
      break;
    }
    default:
      std::fprintf(stderr,
                   "GEKKOAOT_NATIVE_AX_HLE_V50_WARN unknown_cmd=%04x index=%zu bytes=%u\n",
                   cmd, index - 1u, byte_size);
      return false;
    }
  }
  ++command_lists_;
  return true;
}

bool NativeAXHLE::HandleCpuMail(std::uint32_t raw_mail) {
  if (!active_) return false;

  switch (mail_state_) {
  case MailState::WaitingForCommandSize:
    if ((raw_mail & kAxCommandMask) == kAxCommandPrefix) {
      command_list_bytes_ = static_cast<std::uint16_t>(raw_mail);
      mail_state_ = MailState::WaitingForCommandAddress;
      return true;
    }
    return false;

  case MailState::WaitingForCommandAddress: {
    const bool ok = ProcessCommandList(raw_mail, command_list_bytes_);
    command_list_bytes_ = 0u;
    completion_cycles_ = kAxCompletionPpcCycles;
    mail_state_ = MailState::WaitingForTaskReply;
    if (!ok) {
      if (dsp_) dsp_->FlushAudioSink();
      std::fprintf(stderr,
                   "GEKKOAOT_NATIVE_AX_HLE_V51_1_WARN command_list_failed addr=%08x; "
                   "audio_queue=flushed yielding=1\n",
                   raw_mail);
    }
    return true;
  }

  case MailState::WaitingForTaskReply: {
    std::uint32_t task_mail = raw_mail;
    if ((task_mail & kTaskMailMask) != kTaskMailToDsp)
      task_mail = kTaskMailToDsp | (task_mail & 0xffffu);
    switch (task_mail) {
    case kMailContinue:
      mail_state_ = MailState::WaitingForCommandSize;
      return true;
    case kMailResume:
      if (dsp_) dsp_->PostDspMailbox(kDspResume, true);
      mail_state_ = MailState::WaitingForCommandSize;
      return true;
    case kMailNewUcode:
    case kMailReset:
      // The SDK task loader will own the following task-upload transcript.
      active_ = false;
      mail_state_ = MailState::WaitingForCommandSize;
      return true;
    default:
      return false;
    }
  }
  }
  return false;
}

void NativeAXHLE::AdvancePpcCycles(std::uint64_t cycles) {
  if (!active_ || completion_cycles_ == 0u || cycles == 0u) return;
  if (cycles < completion_cycles_) {
    completion_cycles_ -= cycles;
    return;
  }
  completion_cycles_ = 0u;
  if (dsp_) dsp_->PostDspMailbox(kDspYield, true);
}

} // namespace GekkoAOT::DSP
