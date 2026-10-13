// SPDX-License-Identifier: GPL-3.0-or-later
#include "dsp/native/native_jaudio_hle.h"

#include "dsp/native/native_dsp.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <limits>

namespace GekkoAOT::DSP {
namespace {
constexpr std::uint16_t kCommandSetup = 0x8100u;
constexpr std::uint16_t kCommandSync = 0x8200u;
constexpr std::uint16_t kCommandWait = 0x8000u;
constexpr std::uint16_t kCommandIpl = 0x8b00u;
constexpr std::uint16_t kCommandAgb = 0x8c00u;

constexpr std::size_t V_ENABLED = 0x000u;
constexpr std::size_t V_DONE = 0x002u;
constexpr std::size_t V_RATIO = 0x004u;
constexpr std::size_t V_RESET = 0x008u;
constexpr std::size_t V_END_REACHED = 0x00au;
constexpr std::size_t V_CONSTANT = 0x00cu;
constexpr std::size_t V_MIX = 0x010u;
constexpr std::size_t V_DOLBY_POS = 0x050u;
constexpr std::size_t V_DOLBY_REVERB = 0x052u;
constexpr std::size_t V_DOLBY_CURRENT = 0x054u;
constexpr std::size_t V_DOLBY_TARGET = 0x056u;
constexpr std::size_t V_DOLBY_ENABLE = 0x058u;
constexpr std::size_t V_FRAC = 0x060u;
constexpr std::size_t V_AFC_CACHED = 0x064u;
constexpr std::size_t V_CONSTANT_SAMPLE = 0x066u;
constexpr std::size_t V_POSITION = 0x068u;
constexpr std::size_t V_SAMPLES_BEFORE_LOOP = 0x06cu;
constexpr std::size_t V_ARAM = 0x070u;
constexpr std::size_t V_REMAINING = 0x074u;
constexpr std::size_t V_SRC_HISTORY = 0x078u;
constexpr std::size_t V_FIR_HISTORY = 0x080u;
constexpr std::size_t V_BIQUAD_HISTORY = 0x0a8u;
constexpr std::size_t V_AFC_SAMPLES = 0x0b0u;
constexpr std::size_t V_LPF_HISTORY = 0x0d0u;
constexpr std::size_t V_SOURCE_TYPE = 0x100u;
constexpr std::size_t V_LOOPING = 0x102u;
constexpr std::size_t V_LOOP_YN1 = 0x104u;
constexpr std::size_t V_LOOP_YN2 = 0x106u;
constexpr std::size_t V_FILTER_MODE = 0x108u;
constexpr std::size_t V_END_REQUESTED = 0x10au;
constexpr std::size_t V_LOOP_ADDRESS = 0x110u;
constexpr std::size_t V_LOOP_END = 0x114u;
constexpr std::size_t V_BASE = 0x118u;
constexpr std::size_t V_FIR_COEFS = 0x120u;
constexpr std::size_t V_BIQUAD_COEFS = 0x148u;
constexpr std::size_t V_LPF_COEF = 0x150u;

constexpr std::size_t M_ID = 0u;
constexpr std::size_t M_TARGET = 2u;
constexpr std::size_t M_CURRENT = 4u;

constexpr std::size_t FX_BYTES = 0x20u;
constexpr std::size_t FX_ENABLED = 0x00u;
constexpr std::size_t FX_RING_SIZE = 0x02u;
constexpr std::size_t FX_RING_PTR = 0x04u;
constexpr std::size_t FX_DEST = 0x08u;
constexpr std::size_t FX_COEFS = 0x10u;
constexpr std::size_t FX_COUNT = 4u;

constexpr std::uint16_t SRC_AFC_2BIT = 5u;
constexpr std::uint16_t SRC_PCM8 = 8u;
constexpr std::uint16_t SRC_AFC_4BIT = 9u;
constexpr std::uint16_t SRC_PCM16 = 16u;
constexpr std::uint16_t SRC_PCM16_MRAM = 33u;

constexpr double kPi = 3.1415926535897932384626433832795;

// DSP mixing buses wrap at 16 bits; the DAC gain saturates afterwards.
std::int32_t BusSample(std::int64_t value) {
  const auto bits = static_cast<std::uint16_t>(value);
  return bits < 0x8000u ? bits : static_cast<std::int32_t>(bits) - 0x10000;
}
std::int16_t RampMix(std::array<std::int32_t, 80>& bus,
                     const std::array<std::int32_t, 80>& samples,
                     std::int16_t current, std::int16_t delta) {
  std::int64_t gain = static_cast<std::int64_t>(current) * 65536;
  const std::int32_t step = static_cast<std::int32_t>(delta) * 65536 / 80;
  for (std::size_t i = 0; i < bus.size(); ++i) {
    bus[i] = BusSample(bus[i] + ((samples[i] * (gain >> 16u)) >> 16u));
    gain += step;
  }
  return static_cast<std::int16_t>(BusSample(gain >> 16u));
}


std::int32_t SignExtend2(std::uint8_t v) {
  v &= 3u;
  return (v & 2u) ? static_cast<std::int32_t>(v) - 4 : static_cast<std::int32_t>(v);
}
std::int32_t SignExtend4(std::uint8_t v) {
  v &= 15u;
  return (v & 8u) ? static_cast<std::int32_t>(v) - 16 : static_cast<std::int32_t>(v);
}
} // namespace

std::uint16_t NativeJAudioHLE::LoadBe16(const std::uint8_t* p) {
  return static_cast<std::uint16_t>((static_cast<std::uint16_t>(p[0]) << 8u) | p[1]);
}
std::uint32_t NativeJAudioHLE::LoadBe32(const std::uint8_t* p) {
  return (static_cast<std::uint32_t>(p[0]) << 24u) |
         (static_cast<std::uint32_t>(p[1]) << 16u) |
         (static_cast<std::uint32_t>(p[2]) << 8u) | p[3];
}
void NativeJAudioHLE::StoreBe16(std::uint8_t* p, std::uint16_t value) {
  p[0] = static_cast<std::uint8_t>(value >> 8u);
  p[1] = static_cast<std::uint8_t>(value);
}
void NativeJAudioHLE::StoreBe32(std::uint8_t* p, std::uint32_t value) {
  p[0] = static_cast<std::uint8_t>(value >> 24u);
  p[1] = static_cast<std::uint8_t>(value >> 16u);
  p[2] = static_cast<std::uint8_t>(value >> 8u);
  p[3] = static_cast<std::uint8_t>(value);
}
std::int16_t NativeJAudioHLE::Clamp16(std::int64_t value) {
  return static_cast<std::int16_t>(std::clamp<std::int64_t>(value, -32768, 32767));
}

std::uint16_t NativeJAudioHLE::Voice::U16(std::size_t off) const {
  return NativeJAudioHLE::LoadBe16(bytes.data() + off);
}
std::int16_t NativeJAudioHLE::Voice::S16(std::size_t off) const {
  return static_cast<std::int16_t>(U16(off));
}
std::uint32_t NativeJAudioHLE::Voice::U32(std::size_t off) const {
  return NativeJAudioHLE::LoadBe32(bytes.data() + off);
}
void NativeJAudioHLE::Voice::SetU16(std::size_t off, std::uint16_t value) {
  NativeJAudioHLE::StoreBe16(bytes.data() + off, value);
}
void NativeJAudioHLE::Voice::SetS16(std::size_t off, std::int16_t value) {
  SetU16(off, static_cast<std::uint16_t>(value));
}
void NativeJAudioHLE::Voice::SetU32(std::size_t off, std::uint32_t value) {
  NativeJAudioHLE::StoreBe32(bytes.data() + off, value);
}

void NativeJAudioHLE::Reset() {
  active_ = false;
  standard_task_ = standard_ready_ = dolby_louder_ = standard_blocked_ = false;
  standard_state_ = StandardState::Waiting;
  standard_replies_.clear();
  standard_pending_packets_.clear();
  standard_image_hash_ = standard_upload_seen_ = 0;
  standard_upload_.fill(0);
  standard_packet_.fill(0);
  standard_masks_.fill(0);
  standard_expected_ = standard_seen_ = standard_frames_ = standard_frame_ = 0;
  standard_voice_ = standard_max_voice_ = standard_left_ = standard_right_ = 0;
  voice_count_ = 0;
  vpb_address_ = 0;
  resample_table_address_ = 0;
  afc_table_address_ = 0;
  fx_table_address_ = 0;
  master_level_ = 0x4000u;
  tables_ready_ = false;
  resample_coefficients_.fill(0);
  patterns_.fill(0);
  afc_coefficients_.fill(0);
  for (auto& bus : buses_) bus.fill(0);
  previous_back_left_.fill(0);
  previous_back_right_.fill(0);
  for (auto& h : fx_history_) h.fill(0);
  fx_cursor_.fill(0);
  for (std::size_t i = 0; i < surround_sine_.size(); ++i) {
    const double angle = static_cast<double>(i) * (kPi * 0.5) /
                         static_cast<double>(surround_sine_.size() - 1u);
    surround_sine_[i] = static_cast<std::int16_t>(std::sin(angle) * 32767.0);
  }
  setup_packets_ = 0;
  sync_frames_ = 0;
  voices_mixed_ = 0;
  unsupported_sources_ = 0;
  voice_slots_scanned_ = 0;
  voice_full_loads_ = 0;
  voice_slots_skipped_ = 0;
  output_clips_ = 0;
}

bool NativeJAudioHLE::ReadMain(std::uint32_t address, void* dst, std::size_t size) const {
  return dsp_ && dst && size != 0u && size <= std::numeric_limits<std::uint32_t>::max() &&
         dsp_->CoreReadMain(address, static_cast<std::uint8_t*>(dst), static_cast<std::uint32_t>(size));
}
bool NativeJAudioHLE::WriteMain(std::uint32_t address, const void* src, std::size_t size) {
  return dsp_ && src && size != 0u && size <= std::numeric_limits<std::uint32_t>::max() &&
         dsp_->CoreWriteMain(address, static_cast<const std::uint8_t*>(src), static_cast<std::uint32_t>(size));
}
bool NativeJAudioHLE::ReadAram(std::uint32_t address, void* dst, std::size_t size) const {
  return dsp_ && dst && size != 0u && size <= std::numeric_limits<std::uint32_t>::max() &&
         dsp_->CoreReadAram(address, static_cast<std::uint8_t*>(dst),
                            static_cast<std::uint32_t>(size));
}
bool NativeJAudioHLE::ReadMainS16(std::uint32_t address, std::int16_t* dst, std::size_t count) const {
  if (!dst || count == 0u) return false;
  std::array<std::uint8_t, 512u> raw{};
  std::size_t done = 0u;
  while (done < count) {
    const std::size_t samples = std::min<std::size_t>(count - done, raw.size() / 2u);
    const std::size_t bytes = samples * 2u;
    if (!ReadMain(address + static_cast<std::uint32_t>(done * 2u), raw.data(), bytes)) return false;
    for (std::size_t i = 0; i < samples; ++i)
      dst[done + i] = static_cast<std::int16_t>(LoadBe16(raw.data() + i * 2u));
    done += samples;
  }
  return true;
}
bool NativeJAudioHLE::WriteMainS16(std::uint32_t address, const std::int16_t* src, std::size_t count) {
  if (!src || count == 0u) return false;
  std::array<std::uint8_t, 512u> raw{};
  std::size_t done = 0u;
  while (done < count) {
    const std::size_t samples = std::min<std::size_t>(count - done, raw.size() / 2u);
    const std::size_t bytes = samples * 2u;
    for (std::size_t i = 0; i < samples; ++i)
      StoreBe16(raw.data() + i * 2u, static_cast<std::uint16_t>(src[done + i]));
    if (!WriteMain(address + static_cast<std::uint32_t>(done * 2u), raw.data(), bytes)) return false;
    done += samples;
  }
  return true;
}

bool NativeJAudioHLE::LoadVoice(std::uint32_t index, Voice* voice) const {
  if (!voice || index >= voice_count_) return false;
  return ReadMain(vpb_address_ + index * static_cast<std::uint32_t>(kVpbBytes),
                  voice->bytes.data(), voice->bytes.size());
}
bool NativeJAudioHLE::StoreVoice(std::uint32_t index, const Voice& voice) {
  if (index >= voice_count_) return false;
  // Nintendo's JAudio DSP writes back only the mutable first 0x80 words of a
  // normal 0xC0-word VPB. Keep the PPC-owned tail untouched.
  return WriteMain(vpb_address_ + index * static_cast<std::uint32_t>(kVpbBytes),
                   voice.bytes.data(), kVpbWritebackBytes);
}

bool NativeJAudioHLE::LoadTables() {
  std::array<std::uint8_t, 1024> src{};
  std::array<std::uint8_t, 64> afc{};
  if (!ReadMain(resample_table_address_, src.data(), src.size()) ||
      !ReadMain(afc_table_address_, afc.data(), afc.size())) {
    tables_ready_ = false;
    return false;
  }
  for (std::size_t i = 0; i < 256u; ++i)
    resample_coefficients_[i] = static_cast<std::int16_t>(LoadBe16(src.data() + i * 2u));
  for (std::size_t i = 0; i < 256u; ++i)
    patterns_[i] = static_cast<std::int16_t>(LoadBe16(src.data() + (256u + i) * 2u));
  for (std::size_t i = 0; i < 32u; ++i)
    afc_coefficients_[i] = static_cast<std::int16_t>(LoadBe16(afc.data() + i * 2u));
  if (standard_task_) {
    std::array<std::uint8_t, 256> sine{};
    if (!ReadMain(resample_table_address_ + 0x400u, sine.data(), sine.size())) return false;
    for (std::size_t i = 0; i < surround_sine_.size(); ++i)
      surround_sine_[i] = static_cast<std::int16_t>(LoadBe16(sine.data() + 2u * i));
  }
  tables_ready_ = true;
  return true;
}

bool NativeJAudioHLE::Setup(const std::uint32_t* words, std::size_t count) {
  if (!words || count < 5u) return false;
  const std::uint32_t voices = words[0] & 0xffffu;
  if (voices == 0u || voices > kMaxVoices) return false;
  voice_count_ = voices;
  vpb_address_ = words[1] & 0x3fffffffu;
  resample_table_address_ = words[2] & 0x3fffffffu;
  afc_table_address_ = words[3] & 0x3fffffffu;
  fx_table_address_ = words[4] & 0x3fffffffu;
  fx_cursor_.fill(0);
  for (auto& h : fx_history_) h.fill(0);
  previous_back_left_.fill(0);
  previous_back_right_.fill(0);
  active_ = LoadTables();
  ++setup_packets_;
  std::fprintf(stderr,
               "GEKKOAOT_NATIVE_JAUDIO_HLE_V69=1 phase=setup voices=%u vpb=%08x src=%08x afc=%08x fx=%08x tables=%s\n",
               voice_count_, vpb_address_, resample_table_address_, afc_table_address_,
               fx_table_address_, tables_ready_ ? "guest" : "invalid");
  std::fprintf(stderr,
               "GEKKOAOT_NATIVE_JAUDIO_V245=1 pcm16-mram=1 vpb=active-direct writeback=0x%zx dac=front-lr-only trace-hot=off\n",
               kVpbWritebackBytes);
  return true;
}

NativeJAudioHLE::MixBuffer* NativeJAudioHLE::BusForId(std::uint16_t id) {
  switch (id) {
  case 0x0d00: return &buses_[FrontLeft];
  case 0x0d60: return &buses_[FrontRight];
  case 0x0f40: return &buses_[BackLeft];
  case 0x0ca0: return &buses_[BackRight];
  case 0x0e80: return &buses_[FrontLeftWet];
  case 0x0ee0: return &buses_[FrontRightWet];
  case 0x0c00: return &buses_[BackLeftWet];
  case 0x0c50: return &buses_[BackRightWet];
  case 0x0dc0: return &buses_[ExtraWet0];
  case 0x0e20: return &buses_[ExtraWet1];
  case 0x09a0: return &buses_[Extra0];
  case 0x0fa0: return &buses_[Extra1];
  case 0x0b00: return &buses_[Extra2];
  default: return nullptr;
  }
}

void NativeJAudioHLE::ReadEffects() {
  if (fx_table_address_ == 0u) return;
  static constexpr std::array<Bus, 4> kFeedback = {ExtraWet0, ExtraWet1, FrontLeftWet, FrontRightWet};
  for (std::size_t fx = 0; fx < FX_COUNT; ++fx) {
    std::array<std::uint8_t, FX_BYTES> raw{};
    if (!ReadMain(fx_table_address_ + static_cast<std::uint32_t>(fx * FX_BYTES), raw.data(), raw.size()))
      continue;
    const std::uint16_t enabled = LoadBe16(raw.data() + FX_ENABLED);
    const std::uint16_t ring_size = LoadBe16(raw.data() + FX_RING_SIZE);
    const std::uint32_t ring_ptr = LoadBe32(raw.data() + FX_RING_PTR);
    if (enabled == 0u || ring_size == 0u || ring_ptr == 0u) continue;

    const std::size_t frame = fx_cursor_[fx] % ring_size;
    std::array<std::int16_t, kSamplesPerSubframe> ring{};
    const std::uint32_t ring_address = ring_ptr + static_cast<std::uint32_t>(frame * kSamplesPerSubframe * 2u);
    if (!ReadMainS16(ring_address, ring.data(), ring.size())) continue;

    std::array<std::int16_t, kSamplesPerSubframe + 8u> work{};
    std::copy(fx_history_[fx].begin(), fx_history_[fx].end(), work.begin());
    std::copy(ring.begin(), ring.end(), work.begin() + 8u);
    std::copy(work.end() - 8u, work.end(), fx_history_[fx].begin());

    auto filter = [&]() {
      for (std::size_t i = 0; i < kSamplesPerSubframe; ++i) {
        std::int64_t sum = 0;
        for (std::size_t tap = 0; tap < 8u; ++tap)
          sum += static_cast<std::int64_t>(work[i + tap]) *
                 static_cast<std::int16_t>(LoadBe16(raw.data() + FX_COEFS + tap * 2u));
        work[i] = Clamp16(sum >> 15u);
      }
    };
    if ((enabled & 1u) != 0u) filter();

    for (std::size_t d = 0; d < 2u; ++d) {
      const std::uint16_t bus_id = LoadBe16(raw.data() + FX_DEST + d * 4u);
      const std::uint16_t volume = LoadBe16(raw.data() + FX_DEST + d * 4u + 2u);
      auto* bus = BusForId(bus_id);
      if (!bus || bus_id == 0u || volume == 0) continue;
      for (std::size_t i = 0; i < kSamplesPerSubframe; ++i)
        (*bus)[i] = BusSample((*bus)[i] + Clamp16((static_cast<std::int64_t>(work[i]) * volume) >> 15u));
    }
    if ((enabled & 2u) != 0u) filter();
    for (std::size_t i = 0; i < kSamplesPerSubframe; ++i)
      buses_[kFeedback[fx]][i] = work[i];
  }
}

void NativeJAudioHLE::WriteEffects() {
  if (fx_table_address_ == 0u) return;
  static constexpr std::array<Bus, 4> kFeedback = {ExtraWet0, ExtraWet1, FrontLeftWet, FrontRightWet};
  for (std::size_t fx = 0; fx < FX_COUNT; ++fx) {
    std::array<std::uint8_t, FX_BYTES> raw{};
    if (!ReadMain(fx_table_address_ + static_cast<std::uint32_t>(fx * FX_BYTES), raw.data(), raw.size()))
      continue;
    const std::uint16_t enabled = LoadBe16(raw.data() + FX_ENABLED);
    const std::uint16_t ring_size = LoadBe16(raw.data() + FX_RING_SIZE);
    const std::uint32_t ring_ptr = LoadBe32(raw.data() + FX_RING_PTR);
    if (enabled == 0u || ring_size == 0u || ring_ptr == 0u) continue;
    const std::size_t frame = fx_cursor_[fx] % ring_size;
    std::array<std::int16_t, kSamplesPerSubframe> out{};
    for (std::size_t i = 0; i < out.size(); ++i) out[i] = Clamp16(buses_[kFeedback[fx]][i]);
    const std::uint32_t ring_address = ring_ptr + static_cast<std::uint32_t>(frame * kSamplesPerSubframe * 2u);
    if (WriteMainS16(ring_address, out.data(), out.size()))
      fx_cursor_[fx] = static_cast<std::uint16_t>((frame + 1u) % ring_size);
  }
}

void NativeJAudioHLE::BeginSubframe() {
  const MixBuffer prev_bl = previous_back_left_;
  const MixBuffer prev_br = previous_back_right_;
  std::array<std::int16_t, kSamplesPerSubframe> prev_bl_wet{};
  std::array<std::int16_t, kSamplesPerSubframe> prev_br_wet{};
  for (std::size_t i = 0; i < kSamplesPerSubframe; ++i) {
    prev_bl_wet[i] = Clamp16(buses_[BackLeftWet][i]);
    prev_br_wet[i] = Clamp16(buses_[BackRightWet][i]);
  }
  buses_[FrontLeft].fill(0);
  buses_[FrontRight].fill(0);
  for (std::size_t i = 0; i < kSamplesPerSubframe; ++i) {
    buses_[BackLeft][i] = static_cast<std::int32_t>(
        (static_cast<std::int64_t>(prev_bl[i]) * 0x6784) >> 15u);
    buses_[BackRight][i] = static_cast<std::int32_t>(
        (static_cast<std::int64_t>(prev_br[i]) * 0x6784) >> 15u);
  }
  ReadEffects();
  for (std::size_t i = 0; i < kSamplesPerSubframe; ++i) {
    buses_[FrontLeftWet][i] = BusSample(buses_[FrontLeftWet][i] + Clamp16((static_cast<std::int64_t>(prev_bl_wet[i]) * 0x7fff) >> 15u));
    buses_[FrontRightWet][i] = BusSample(buses_[FrontRightWet][i] + Clamp16((static_cast<std::int64_t>(prev_bl_wet[i]) * 0xb820) >> 15u));
    if (i >= kSamplesPerSubframe / 2u) {
      const auto j = i - kSamplesPerSubframe / 2u;
      buses_[FrontLeftWet][j] = BusSample(buses_[FrontLeftWet][j] + Clamp16((static_cast<std::int64_t>(prev_br_wet[i]) * 0xb820) >> 15u));
      buses_[FrontRightWet][j] = BusSample(buses_[FrontRightWet][j] + Clamp16((static_cast<std::int64_t>(prev_br_wet[i]) * 0x7fff) >> 15u));
    }
  }
  buses_[BackLeftWet].fill(0);
  buses_[BackRightWet].fill(0);

  // The setup SRC table also carries four 64-sample oscillator patterns. The
  // last two are stateful in the light-JAudio microcode; evolve them once per
  // subframe so oscillator voices remain deterministic without embedding the
  // original table in the host binary.
  std::int16_t* p2 = patterns_.data() + 128u;
  std::int32_t y2 = p2[62], y1 = p2[63];
  for (std::size_t i = 0; i < 64u; i += 2u) {
    std::int64_t value = static_cast<std::int64_t>(y2) * y1 -
                         static_cast<std::int64_t>(p2[i]) * 65536;
    y2 = y1; y1 = p2[i]; p2[i] = static_cast<std::int16_t>(value >> 16u);
    value = 2 * (static_cast<std::int64_t>(y2) * y1 +
                 static_cast<std::int64_t>(p2[i + 1u]) * 65536);
    y2 = y1; y1 = p2[i + 1u]; p2[i + 1u] = static_cast<std::int16_t>(value >> 16u);
  }
  std::int16_t* p3 = patterns_.data() + 192u;
  y2 = p3[62]; y1 = p3[63];
  const std::int16_t base = static_cast<std::int16_t>(y1);
  std::int32_t step = p3[0] + static_cast<std::int32_t>(
      (static_cast<std::int64_t>(y1) * y2 + static_cast<std::int64_t>(y2) * 65536 + y1) >> 16u);
  step = (step & 0x1ff) | 0x2000;
  for (std::size_t i = 0; i < 64u; ++i)
    p3[i] = static_cast<std::int16_t>(base + static_cast<std::int32_t>(i + 1u) * step);
}

void NativeJAudioHLE::FinishSubframe(std::int16_t* left, std::int16_t* right) {
  if (!left || !right) return;

  // The JAudio DAC ucode writes the front L/R mix buffers after applying the
  // 4.12 DSP mixer level. Back/wet/extra buffers feed those fronts through
  // the normal mixer/reverb graph; adding them again here double-counts FX
  // and rear energy and is a common source of clipping/muddy output.
  for (std::size_t i = 0; i < kSamplesPerSubframe; ++i) {
    const std::int64_t l =
        (static_cast<std::int64_t>(buses_[FrontLeft][i]) * master_level_) >> 12u;
    const std::int64_t r =
        (static_cast<std::int64_t>(buses_[FrontRight][i]) * master_level_) >> 12u;
    output_clips_ += (l < -32768 || l > 32767) ? 1u : 0u;
    output_clips_ += (r < -32768 || r > 32767) ? 1u : 0u;
    left[i] = Clamp16(l);
    right[i] = Clamp16(r);
  }

  // The rear dry buffers are persistent across frames and decay by 0x6784 in
  // BeginSubframe(), matching the JAudio renderer's feedback behavior.
  previous_back_left_ = buses_[BackLeft];
  previous_back_right_ = buses_[BackRight];
  WriteEffects();
}

std::uint16_t NativeJAudioHLE::NeededRawSamples(const Voice& voice) const {
  const std::uint32_t needed =
      (static_cast<std::uint32_t>(voice.U16(V_FRAC)) +
       static_cast<std::uint32_t>(kSamplesPerSubframe) * voice.U16(V_RATIO)) >> 12u;
  return static_cast<std::uint16_t>(std::min<std::uint32_t>(needed, kRawSamples - 20u));
}

void NativeJAudioHLE::Resample(Voice* voice, const std::int16_t* source, MixBuffer* output) {
  if (!voice || !source || !output) return;
  std::uint32_t position = voice->U16(V_FRAC);
  const std::uint32_t ratio = voice->U16(V_RATIO);
  if ((ratio >> 12u) >= 4u) {
    for (auto& sample : *output) {
      position += ratio;
      sample = source[std::min<std::size_t>(position >> 12u, kRawSamples - 1u)];
    }
  } else {
    for (auto& sample : *output) {
      const std::size_t phase = ((position & 0xfffu) >> 6u) * 4u;
      const std::size_t input = std::min<std::size_t>(position >> 12u, kRawSamples - 4u);
      std::int64_t value = 0;
      for (std::size_t tap = 0; tap < 4u; ++tap)
        value += 2 * static_cast<std::int64_t>(resample_coefficients_[phase + tap]) * source[input + tap];
      sample = Clamp16(value >> 16u);
      position += ratio;
    }
  }
  const std::size_t tail = std::min<std::size_t>(position >> 12u, kRawSamples - 4u);
  for (std::size_t i = 0; i < 4u; ++i) voice->SetS16(V_SRC_HISTORY + i * 2u, source[tail + i]);
  voice->SetS16(V_CONSTANT_SAMPLE, Clamp16(output->back()));
  voice->SetU16(V_FRAC, static_cast<std::uint16_t>(position & 0xfffu));
}

void NativeJAudioHLE::LoadPcm(Voice* voice, std::int16_t* output, std::uint16_t count,
                              std::uint8_t bytes_per_sample) {
  if (!voice || !output) return;
  if (voice->U16(V_DONE) != 0u) { std::fill(output, output + count, 0); return; }
  if (voice->U16(V_RESET) != 0u) {
    const std::uint32_t current = voice->U32(V_POSITION);
    const std::uint32_t end = voice->U32(V_LOOP_END);
    voice->SetU32(V_REMAINING, end > current ? end - current : 0u);
    const std::uint64_t address = static_cast<std::uint64_t>(voice->U32(V_BASE)) +
                                  static_cast<std::uint64_t>(current) * bytes_per_sample;
    voice->SetU32(V_ARAM, static_cast<std::uint32_t>(address));
  }
  voice->SetU16(V_END_REACHED, 0u);
  while (count != 0u) {
    if (voice->U16(V_END_REACHED) != 0u || voice->U32(V_REMAINING) == 0u) {
      voice->SetU16(V_END_REACHED, 0u);
      if (voice->U16(V_LOOPING) == 0u) {
        std::fill(output, output + count, 0);
        voice->SetU16(V_DONE, 1u);
        return;
      }
      const std::uint32_t loop = voice->U32(V_LOOP_ADDRESS);
      const std::uint32_t end = voice->U32(V_LOOP_END);
      if (end <= loop) { std::fill(output, output + count, 0); voice->SetU16(V_DONE, 1u); return; }
      voice->SetU32(V_POSITION, loop);
      voice->SetU32(V_REMAINING, end - loop);
      voice->SetU32(V_ARAM, voice->U32(V_BASE) + loop * bytes_per_sample);
    }
    const std::uint16_t take = static_cast<std::uint16_t>(
        std::min<std::uint32_t>(voice->U32(V_REMAINING), count));
    if (take == 0u) { voice->SetU16(V_DONE, 1u); std::fill(output, output + count, 0); return; }
    std::array<std::uint8_t, kRawSamples * 2u> raw{};
    const std::size_t raw_bytes = static_cast<std::size_t>(take) * bytes_per_sample;
    if (raw_bytes > raw.size() || !ReadAram(voice->U32(V_ARAM), raw.data(), raw_bytes)) {
      voice->SetU16(V_DONE, 1u); std::fill(output, output + count, 0); return;
    }
    if (bytes_per_sample == 1u) {
      for (std::size_t i = 0; i < take; ++i)
        *output++ = static_cast<std::int16_t>(static_cast<std::int8_t>(raw[i])) * 256;
    } else {
      for (std::size_t i = 0; i < take; ++i)
        *output++ = static_cast<std::int16_t>(LoadBe16(raw.data() + i * 2u));
    }
    voice->SetU32(V_REMAINING, voice->U32(V_REMAINING) - take);
    voice->SetU32(V_ARAM, voice->U32(V_ARAM) + static_cast<std::uint32_t>(take) * bytes_per_sample);
    voice->SetU32(V_POSITION, voice->U32(V_POSITION) + take);
    count = static_cast<std::uint16_t>(count - take);
    if (voice->U32(V_REMAINING) == 0u) voice->SetU16(V_END_REACHED, 1u);
  }
}

void NativeJAudioHLE::LoadPcmMram(Voice* voice, std::int16_t* output,
                                   std::uint16_t count) {
  if (!voice || !output || count == 0u) return;
  if (voice->U16(V_DONE) != 0u) {
    std::fill(output, output + count, 0);
    return;
  }

  // JAudio source type 33 is raw big-endian PCM16 in main RAM. Unlike the
  // ARAM PCM sources, its stream cursor uses the high 16-bit position field
  // directly and may switch the base pointer at the loop boundary.
  std::uint16_t current = voice->U16(V_POSITION);
  std::uint32_t base = voice->U32(V_BASE);
  std::uint32_t remaining = voice->U32(V_REMAINING);

  auto read_samples = [&](std::uint32_t address, std::int16_t* dst,
                          std::uint32_t samples) -> bool {
    if (samples == 0u) return true;
    return ReadMainS16(address, dst, samples);
  };

  if (count > remaining) {
    std::int16_t last = 0;
    if (remaining != 0u) {
      if (!read_samples(base + static_cast<std::uint32_t>(current) * 2u,
                        output, remaining)) {
        std::fill(output, output + count, 0);
        voice->SetU16(V_DONE, 1u);
        return;
      }
      last = output[remaining - 1u];
    }
    std::fill(output + remaining, output + count, last);
    current = static_cast<std::uint16_t>(current + remaining);
    voice->SetU16(V_POSITION, current);
    voice->SetU32(V_REMAINING, 0u);
    voice->SetU16(V_DONE, 1u);
    return;
  }

  voice->SetU32(V_REMAINING, remaining - count);
  const std::uint16_t loop_start = voice->U16(V_LOOP_END);
  const std::uint16_t before_loop =
      static_cast<std::uint16_t>(loop_start - current);
  voice->SetU16(V_SAMPLES_BEFORE_LOOP, before_loop);

  if (count <= before_loop) {
    if (!read_samples(base + static_cast<std::uint32_t>(current) * 2u,
                      output, count)) {
      std::fill(output, output + count, 0);
      voice->SetU16(V_DONE, 1u);
      return;
    }
    voice->SetU16(V_POSITION, static_cast<std::uint16_t>(current + count));
    return;
  }

  const std::uint16_t first = before_loop;
  if (first != 0u &&
      !read_samples(base + static_cast<std::uint32_t>(current) * 2u,
                    output, first)) {
    std::fill(output, output + count, 0);
    voice->SetU16(V_DONE, 1u);
    return;
  }

  const std::uint32_t loop_base = voice->U32(V_LOOP_ADDRESS);
  const std::uint16_t second = static_cast<std::uint16_t>(count - first);
  voice->SetU32(V_BASE, loop_base);
  voice->SetU16(V_POSITION, second);
  if (second != 0u && !read_samples(loop_base, output + first, second)) {
    std::fill(output + first, output + count, 0);
    voice->SetU16(V_DONE, 1u);
  }
}

bool NativeJAudioHLE::DecodeAfcBlocks(Voice* voice, std::int16_t* output, std::size_t blocks) {
  if (!voice || !output || blocks == 0u) return blocks == 0u;
  const std::uint16_t source_type = voice->U16(V_SOURCE_TYPE);
  const std::uint8_t block_bytes = source_type == SRC_AFC_4BIT ? 9u : 5u;
  std::array<std::uint8_t, ((kRawSamples + 15u) / 16u) * 9u> raw{};
  const std::size_t raw_bytes = blocks * block_bytes;
  if (raw_bytes > raw.size() || !ReadAram(voice->U32(V_ARAM), raw.data(), raw_bytes)) return false;
  voice->SetU32(V_ARAM, voice->U32(V_ARAM) + static_cast<std::uint32_t>(raw_bytes));

  std::int32_t yn2 = voice->S16(V_AFC_SAMPLES + 14u * 2u);
  std::int32_t yn1 = voice->S16(V_AFC_SAMPLES + 15u * 2u);
  const std::uint8_t* source = raw.data();
  for (std::size_t block = 0; block < blocks; ++block) {
    const std::uint8_t header = source[0];
    const std::uint32_t shift = header >> 4u;
    const std::int32_t scale = static_cast<std::int16_t>(1u << shift);
    const std::size_t predictor = header & 0x0fu;
    const std::int32_t c0 = afc_coefficients_[predictor * 2u];
    const std::int32_t c1 = afc_coefficients_[predictor * 2u + 1u];
    const std::uint8_t* packed = source + 1u;
    for (std::size_t i = 0; i < 16u; ++i) {
      std::int32_t q = 0;
      if (block_bytes == 9u) {
        const std::uint8_t b = packed[i >> 1u];
        q = SignExtend4((i & 1u) ? b : static_cast<std::uint8_t>(b >> 4u));
      } else {
        const std::uint8_t b = packed[i >> 2u];
        const std::uint8_t bits = static_cast<std::uint8_t>((b >> (6u - ((i & 3u) * 2u))) & 3u);
        q = SignExtend2(bits) * 4;
      }
      const std::int64_t sum = (static_cast<std::int64_t>(q) * scale * 2048) +
                               static_cast<std::int64_t>(yn1) * c0 +
                               static_cast<std::int64_t>(yn2) * c1;
      const std::int16_t sample = Clamp16(sum >> 11u);
      *output++ = sample;
      yn2 = yn1;
      yn1 = sample;
    }
    source += block_bytes;
  }
  voice->SetS16(V_AFC_SAMPLES + 14u * 2u, static_cast<std::int16_t>(yn2));
  voice->SetS16(V_AFC_SAMPLES + 15u * 2u, static_cast<std::int16_t>(yn1));
  return true;
}

void NativeJAudioHLE::LoadAfc(Voice* voice, std::int16_t* output, std::uint16_t count) {
  if (!voice || !output) return;
  if (voice->U16(V_RESET) != 0u) {
    voice->SetS16(V_AFC_SAMPLES + 14u * 2u, 0);
    voice->SetS16(V_AFC_SAMPLES + 15u * 2u, 0);
    voice->SetU16(V_AFC_CACHED, 0u);
    voice->SetU32(V_REMAINING, voice->U32(V_LOOP_END));
    voice->SetU32(V_ARAM, voice->U32(V_BASE));
  }
  if (voice->U16(V_DONE) != 0u) { std::fill(output, output + count, 0); return; }

  while (count != 0u) {
    const std::uint16_t cached_total = std::min<std::uint16_t>(voice->U16(V_AFC_CACHED), 16u);
    const std::uint16_t cached = std::min<std::uint16_t>(cached_total, count);
    for (std::uint16_t i = 0; i < cached; ++i) {
      const std::size_t index = 16u - cached_total + i;
      *output++ = voice->S16(V_AFC_SAMPLES + index * 2u);
    }
    voice->SetU16(V_AFC_CACHED, static_cast<std::uint16_t>(cached_total - cached));
    count = static_cast<std::uint16_t>(count - cached);
    if (count == 0u) return;

    const std::uint32_t remaining = voice->U32(V_REMAINING);
    if (remaining != 0u) {
      const std::uint32_t wanted = std::min<std::uint32_t>(remaining, count);
      const std::uint16_t blocks = static_cast<std::uint16_t>((wanted + 15u) >> 4u);
      std::array<std::int16_t, 16u * 64u> decoded{};
      if (blocks > 64u || !DecodeAfcBlocks(voice, decoded.data(), blocks)) {
        std::fill(output, output + count, 0); voice->SetU16(V_DONE, 1u); return;
      }
      const std::uint32_t decoded_count = static_cast<std::uint32_t>(blocks) * 16u;
      const std::uint32_t usable = std::min<std::uint32_t>(remaining, decoded_count);
      const std::uint32_t take = std::min<std::uint32_t>(count, usable);
      std::copy(decoded.begin(), decoded.begin() + take, output);
      output += take;
      count = static_cast<std::uint16_t>(count - take);
      voice->SetU32(V_REMAINING, remaining - usable);
      if (usable > take) {
        const std::uint32_t cache_count = std::min<std::uint32_t>(16u, usable - take);
        for (std::uint32_t i = 0; i < 16u; ++i)
          voice->SetS16(V_AFC_SAMPLES + i * 2u, decoded[decoded_count - 16u + i]);
        // At a non-aligned end, the valid tail must be right-aligned for the
        // next cache read. Loop restart supplies its own predictor history.
        if (usable < decoded_count) {
          for (std::uint32_t i = 0; i < cache_count; ++i)
            voice->SetS16(V_AFC_SAMPLES + (16u - cache_count + i) * 2u, decoded[take + i]);
        }
        voice->SetU16(V_AFC_CACHED, static_cast<std::uint16_t>(cache_count));
      }
      if (count == 0u) return;
    }

    if (voice->U32(V_REMAINING) == 0u) {
      if (voice->U16(V_LOOPING) == 0u) {
        voice->SetU16(V_DONE, 1u); std::fill(output, output + count, 0); return;
      }
      const std::uint32_t loop_sample = voice->U32(V_LOOP_ADDRESS);
      const std::uint32_t end_sample = voice->U32(V_LOOP_END);
      if (end_sample <= loop_sample) {
        voice->SetU16(V_DONE, 1u); std::fill(output, output + count, 0); return;
      }
      const std::uint32_t source_type = voice->U16(V_SOURCE_TYPE);
      voice->SetU32(V_ARAM, voice->U32(V_BASE) + (loop_sample >> 4u) * source_type);
      voice->SetS16(V_AFC_SAMPLES + 14u * 2u, voice->S16(V_LOOP_YN2));
      voice->SetS16(V_AFC_SAMPLES + 15u * 2u, voice->S16(V_LOOP_YN1));
      std::array<std::int16_t, 16> loop_block{};
      if (!DecodeAfcBlocks(voice, loop_block.data(), 1u)) {
        voice->SetU16(V_DONE, 1u); std::fill(output, output + count, 0); return;
      }
      const std::uint32_t skip = loop_sample & 15u;
      const std::uint32_t cached_count = std::min(16u - skip, end_sample - loop_sample);
      for (std::uint32_t i = 0; i < 16u; ++i)
        voice->SetS16(V_AFC_SAMPLES + i * 2u, loop_block[i]);
      if (cached_count < 16u - skip)
        for (std::uint32_t i = 0; i < cached_count; ++i)
          voice->SetS16(V_AFC_SAMPLES + (16u - cached_count + i) * 2u, loop_block[skip + i]);
      voice->SetU16(V_AFC_CACHED, static_cast<std::uint16_t>(cached_count));
      voice->SetU32(V_REMAINING, end_sample - loop_sample - cached_count);
    }
  }
}

void NativeJAudioHLE::LoadPattern(Voice* voice, MixBuffer* output) {
  if (!voice || !output) return;
  const std::uint16_t source_type = voice->U16(V_SOURCE_TYPE);
  if (source_type == 0u || source_type == 3u) {
    const std::uint32_t shift = source_type == 0u ? 1u : 2u;
    const std::uint32_t mask = (1u << shift) - 1u;
    const std::uint32_t ratio = static_cast<std::uint32_t>(voice->U16(V_RATIO)) << (shift - 1u);
    std::uint32_t pos = static_cast<std::uint32_t>(voice->U16(V_FRAC)) << shift;
    for (auto& s : *output) { s = ((pos >> 16u) & mask) ? -0x4000 : 0x4000; pos += ratio; }
    voice->SetU16(V_FRAC, static_cast<std::uint16_t>((pos >> shift) & 0xffffu));
    return;
  }
  if (source_type == 1u) {
    std::uint32_t pos = voice->U16(V_FRAC);
    for (auto& s : *output) { s = static_cast<std::int16_t>(pos); pos += voice->U16(V_RATIO) >> 1u; }
    voice->SetU16(V_FRAC, static_cast<std::uint16_t>(pos));
    return;
  }
  std::size_t bank = 0u;
  bool modulated = false;
  if (source_type == 4u) bank = 1u;
  else if (source_type == 10u) modulated = true;
  else if (source_type == 11u) bank = 2u;
  else if (source_type == 12u) bank = 3u;
  else if (source_type != 7u) { output->fill(0); return; }
  std::uint32_t position = static_cast<std::uint32_t>(voice->U16(V_FRAC)) << 6u;
  const std::uint32_t step = static_cast<std::uint32_t>(voice->U16(V_RATIO)) << 5u;
  for (std::size_t i = 0; i < output->size(); ++i) {
    (*output)[i] = patterns_[bank * 64u + ((position >> 16u) & 63u)];
    position = (position + step) % (64u << 16u);
    if (modulated) {
      const std::int64_t adjusted = (static_cast<std::int64_t>(position) << 10u) +
                                    static_cast<std::int64_t>(buses_[BackRight][i]) * voice->U16(V_RATIO);
      position = static_cast<std::uint32_t>(std::max<std::int64_t>(0, adjusted >> 10u)) % (64u << 16u);
    }
  }
  voice->SetU16(V_FRAC, static_cast<std::uint16_t>(position >> 6u));
}

void NativeJAudioHLE::LoadInput(std::uint32_t index, Voice* voice, MixBuffer* output) {
  (void)index;
  if (!voice || !output) return;
  if (voice->U16(V_CONSTANT) != 0u) { output->fill(voice->S16(V_CONSTANT_SAMPLE)); return; }
  const std::uint16_t type = voice->U16(V_SOURCE_TYPE);
  if (type == 0u || type == 1u || type == 3u || type == 4u || type == 7u ||
      type == 10u || type == 11u || type == 12u) {
    LoadPattern(voice, output);
    return;
  }
  std::array<std::int16_t, kRawSamples + 16u> raw{};
  for (std::size_t i = 0; i < 4u; ++i) raw[i] = voice->S16(V_SRC_HISTORY + i * 2u);
  const std::uint16_t needed = NeededRawSamples(*voice);
  if (type == SRC_AFC_2BIT || type == SRC_AFC_4BIT)
    LoadAfc(voice, raw.data() + 4u, needed);
  else if (type == SRC_PCM8)
    LoadPcm(voice, raw.data() + 4u, needed, 1u);
  else if (type == SRC_PCM16)
    LoadPcm(voice, raw.data() + 4u, needed, 2u);
  else if (type == SRC_PCM16_MRAM)
    LoadPcmMram(voice, raw.data() + 4u, needed);
  else {
    output->fill(0);
    if (unsupported_sources_++ < 16u)
      std::fprintf(stderr, "GEKKOAOT_NATIVE_JAUDIO_HLE_V69_WARN unsupported_source=%u\n", type);
    return;
  }
  Resample(voice, raw.data(), output);
}

void NativeJAudioHLE::ApplyLowPass(Voice* voice, MixBuffer* samples) {
  std::int32_t yn1 = voice->U16(V_RESET) ? 0 : voice->S16(V_LPF_HISTORY);
  std::int32_t xn1 = voice->U16(V_RESET) ? 0 : voice->S16(V_LPF_HISTORY + 2u);
  const std::int32_t c = voice->S16(V_LPF_COEF);
  for (auto& sample : *samples) {
    const std::int32_t xn0 = sample;
    const std::int16_t yn0 = Clamp16(static_cast<std::int64_t>(yn1) +
                                      ((static_cast<std::int64_t>(xn0 - xn1) * c) >> 7u));
    sample = yn0; yn1 = yn0; xn1 = xn0;
  }
  voice->SetS16(V_LPF_HISTORY, static_cast<std::int16_t>(yn1));
  voice->SetS16(V_LPF_HISTORY + 2u, static_cast<std::int16_t>(xn1));
}

void NativeJAudioHLE::ApplyFir(Voice* voice, MixBuffer* samples) {
  const std::size_t taps = std::min<std::size_t>(voice->U16(V_FILTER_MODE) & 0x1fu, 20u);
  if (taps == 0u) return;
  std::array<std::int16_t, 20u + kSamplesPerSubframe> input{};
  for (std::size_t i = 0; i < 20u; ++i) input[i] = voice->S16(V_FIR_HISTORY + i * 2u);
  for (std::size_t i = 0; i < kSamplesPerSubframe; ++i) input[20u + i] = Clamp16((*samples)[i]);
  for (std::size_t i = 0; i < kSamplesPerSubframe; ++i) {
    std::int64_t sum = 0;
    for (std::size_t tap = 0; tap < taps; ++tap)
      sum += static_cast<std::int64_t>(input[20u + i - tap]) * voice->S16(V_FIR_COEFS + tap * 2u);
    (*samples)[i] = Clamp16(sum >> 15u);
  }
  for (std::size_t i = 0; i < 20u; ++i)
    voice->SetS16(V_FIR_HISTORY + i * 2u, input[kSamplesPerSubframe + i]);
}

void NativeJAudioHLE::ApplyBiquad(Voice* voice, MixBuffer* samples) {
  std::int32_t xn1 = voice->S16(V_BIQUAD_HISTORY + 0u);
  std::int32_t xn2 = voice->S16(V_BIQUAD_HISTORY + 2u);
  std::int32_t yn1 = voice->S16(V_BIQUAD_HISTORY + 4u);
  std::int32_t yn2 = voice->S16(V_BIQUAD_HISTORY + 6u);
  for (auto& sample : *samples) {
    const std::int32_t xn0 = sample;
    const std::int64_t sum = static_cast<std::int64_t>(voice->S16(V_BIQUAD_COEFS + 0u)) * xn1 +
                             static_cast<std::int64_t>(voice->S16(V_BIQUAD_COEFS + 2u)) * xn2 +
                             static_cast<std::int64_t>(voice->S16(V_BIQUAD_COEFS + 4u)) * yn1 +
                             static_cast<std::int64_t>(voice->S16(V_BIQUAD_COEFS + 6u)) * yn2;
    const std::int16_t yn0 = Clamp16(sum >> 15u);
    sample = yn0; xn2 = xn1; xn1 = xn0; yn2 = yn1; yn1 = yn0;
  }
  voice->SetS16(V_BIQUAD_HISTORY + 0u, static_cast<std::int16_t>(xn1));
  voice->SetS16(V_BIQUAD_HISTORY + 2u, static_cast<std::int16_t>(xn2));
  voice->SetS16(V_BIQUAD_HISTORY + 4u, static_cast<std::int16_t>(yn1));
  voice->SetS16(V_BIQUAD_HISTORY + 6u, static_cast<std::int16_t>(yn2));
}

void NativeJAudioHLE::MixVoice(Voice* voice, const MixBuffer& samples) {
  if (!voice) return;
  if (voice->U16(V_DOLBY_ENABLE) != 0u) {
    if (voice->U16(V_END_REQUESTED) != 0u) {
      const std::int16_t next = static_cast<std::int16_t>(voice->S16(V_DOLBY_CURRENT) / 2);
      voice->SetS16(V_DOLBY_TARGET, next);
      if (next == 0) voice->SetU16(V_DONE, 1u);
    }
    const std::uint16_t pos = voice->U16(V_DOLBY_POS);
    const std::size_t x = (pos >> 8u) & 0x7fu, y = pos & 0x7fu;
    const std::int32_t right = surround_sine_[x], left = surround_sine_[x ^ 0x7fu];
    const std::int32_t back = surround_sine_[y], front = surround_sine_[y ^ 0x7fu];
    const unsigned shift = dolby_louder_ ? 15u : 16u;
    const std::array<std::int16_t,4> quad = {
      static_cast<std::int16_t>((left * front) >> shift),
      static_cast<std::int16_t>((left * back) >> shift),
      static_cast<std::int16_t>((right * front) >> shift),
      static_cast<std::int16_t>((right * back) >> shift)};
    const std::array<Bus,4> dry = {FrontLeft, BackLeft, FrontRight, BackRight};
    const std::array<Bus,4> wet = {FrontLeftWet, BackLeftWet, FrontRightWet, BackRightWet};
    const std::int16_t current = voice->S16(V_DOLBY_CURRENT), target = voice->S16(V_DOLBY_TARGET);
    const std::int16_t delta = static_cast<std::int16_t>(target - current);
    const std::int32_t reverb = voice->S16(V_DOLBY_REVERB);
    for (std::size_t q = 0; q < 4u; ++q) {
      const auto start = static_cast<std::int16_t>((quad[q] * current) >> shift);
      const auto step = static_cast<std::int16_t>((static_cast<std::uint16_t>(quad[q]) * delta) >> shift);
      const auto wet_start = static_cast<std::int16_t>((start * reverb) >> shift);
      const auto wet_step = static_cast<std::int16_t>((step * reverb) >> shift);
      RampMix(buses_[dry[q]], samples, start, step);
      RampMix(buses_[wet[q]], samples, wet_start, wet_step);
    }
    voice->SetS16(V_DOLBY_CURRENT, static_cast<std::int16_t>(target));
    return;
  }

  if (voice->U16(V_END_REQUESTED) != 0u) {
    bool silent = true;
    for (std::size_t m = 0; m < kMixerCount; ++m) {
      const std::size_t off = V_MIX + m * 8u;
      const std::int16_t target = static_cast<std::int16_t>(voice->S16(off + M_CURRENT) / 2);
      voice->SetS16(off + M_TARGET, target);
      silent = silent && target == 0;
    }
    if (silent) voice->SetU16(V_DONE, 1u);
  }

  for (std::size_t m = 0; m < kMixerCount; ++m) {
    const std::size_t off = V_MIX + m * 8u;
    const std::uint16_t id = voice->U16(off + M_ID);
    auto* bus = BusForId(id);
    if (!bus || id == 0u) continue;
    const std::int16_t current = voice->S16(off + M_CURRENT);
    const std::int16_t delta = static_cast<std::int16_t>(voice->S16(off + M_TARGET) - current);
    voice->SetS16(off + M_CURRENT, RampMix(*bus, samples, current, delta));
  }
}

void NativeJAudioHLE::RenderVoice(std::uint32_t index, Voice* voice) {
  if (!voice || voice->U16(V_ENABLED) == 0u || voice->U16(V_DONE) != 0u) return;
  MixBuffer input{};
  LoadInput(index, voice, &input);
  if (voice->S16(V_LPF_COEF) != 0) ApplyLowPass(voice, &input);
  if ((voice->U16(V_FILTER_MODE) & 0x1fu) != 0u) ApplyFir(voice, &input);
  if ((voice->U16(V_FILTER_MODE) & 0x20u) != 0u) ApplyBiquad(voice, &input);
  static const bool trace = [] { const char* v = std::getenv("GEKKOAOT_TRACE_DSP"); return v && std::strcmp(v,"0") != 0; }();
  static unsigned trace_voices = 0;
  if (trace && trace_voices++ < 32u) {
    const auto [lo,hi] = std::minmax_element(input.begin(), input.end());
    std::fprintf(stderr,"GEKKOAOT_JAUDIO_VOICE index=%u type=%u constant=%u ratio=%04x min=%d max=%d pos=%08x remaining=%u dolby=%u gains=%04x/%04x\n",index,voice->U16(V_SOURCE_TYPE),voice->U16(V_CONSTANT),voice->U16(V_RATIO),*lo,*hi,voice->U32(V_POSITION),voice->U32(V_REMAINING),voice->U16(V_DOLBY_ENABLE),voice->U16(V_MIX+M_CURRENT),voice->U16(V_MIX+M_TARGET));
  }
  MixVoice(voice, input);
  if (voice->U16(V_CONSTANT) == 0u) voice->SetU16(V_RESET, 0u);
  ++voices_mixed_;
}

void NativeJAudioHLE::RenderVoiceRange(std::uint32_t begin, std::uint32_t end,
                                      const std::array<std::uint16_t, 4>* mask, std::uint8_t* table) {
  // VPBs are linear in MEM1. Borrow one contiguous span per DSP work item so
  // inactive voice checks are two BE16 loads instead of Resolve+384B memcpy.
  // If the table is not directly resolvable (future paged/MMU path), retain
  // the exact CoreReadMain/CoreWriteMain fallback.
  const std::uint32_t table_bytes =
      voice_count_ * static_cast<std::uint32_t>(kVpbBytes);
  std::uint8_t* const vpb_table =
      table ? table : (dsp_ ? dsp_->CoreResolveMain(vpb_address_, table_bytes) : nullptr);

    for (std::uint32_t voice_index = begin; voice_index < std::min(end, voice_count_); ++voice_index) {
      if (mask && ((*mask)[voice_index >> 4u] & (1u << (15u - (voice_index & 15u)))) == 0u) continue;
      ++voice_slots_scanned_;
      Voice voice{};

      if (vpb_table) {
        std::uint8_t* const raw =
            vpb_table + voice_index * static_cast<std::uint32_t>(kVpbBytes);
        if (LoadBe16(raw + V_ENABLED) == 0u || LoadBe16(raw + V_DONE) != 0u) {
          ++voice_slots_skipped_;
          continue;
        }
        std::memcpy(voice.bytes.data(), raw, kVpbBytes);
        ++voice_full_loads_;
        RenderVoice(voice_index, &voice);
        std::memcpy(raw, voice.bytes.data(), kVpbWritebackBytes);
      } else {
        // Conservative fallback for non-contiguous/paged guest memory.
        std::array<std::uint8_t, 4> header{};
        const std::uint32_t voice_address =
            vpb_address_ + voice_index * static_cast<std::uint32_t>(kVpbBytes);
        if (!ReadMain(voice_address, header.data(), header.size())) continue;
        if (LoadBe16(header.data() + V_ENABLED) == 0u ||
            LoadBe16(header.data() + V_DONE) != 0u) {
          ++voice_slots_skipped_;
          continue;
        }
        if (!LoadVoice(voice_index, &voice)) continue;
        ++voice_full_loads_;
        RenderVoice(voice_index, &voice);
        (void)StoreVoice(voice_index, voice);
      }
    }
}

bool NativeJAudioHLE::Sync(const std::uint32_t* words, std::size_t count) {
  if (!active_ || !tables_ready_ || !words || count < 3u) return false;
  const std::uint32_t subframes = std::clamp<std::uint32_t>((words[0] >> 16u) & 0xffu, 1u,
                                                            static_cast<std::uint32_t>(kMaxSubframes));
  master_level_ = static_cast<std::uint16_t>(words[0]);
  const std::uint32_t left_address = words[1] & 0x3fffffffu;
  const std::uint32_t right_address = words[2] & 0x3fffffffu;
  if (left_address == 0u || right_address == 0u) return false;

  std::array<std::int16_t, kMaxSubframes * kSamplesPerSubframe> left{};
  std::array<std::int16_t, kMaxSubframes * kSamplesPerSubframe> right{};
  const std::size_t output_samples = static_cast<std::size_t>(subframes) * kSamplesPerSubframe;

  auto* vpb_table = dsp_ ? dsp_->CoreResolveMain(vpb_address_, voice_count_ * kVpbBytes) : nullptr;
  for (std::uint32_t sf = 0; sf < subframes; ++sf) {
    BeginSubframe();
    RenderVoiceRange(0u, voice_count_, nullptr, vpb_table);
    FinishSubframe(left.data() + sf * kSamplesPerSubframe,
                   right.data() + sf * kSamplesPerSubframe);
  }
  if (!WriteMainS16(left_address, left.data(), output_samples) ||
      !WriteMainS16(right_address, right.data(), output_samples)) return false;
  ++sync_frames_;
  if (sync_frames_ <= 8u || (sync_frames_ % 256u) == 0u)
    std::fprintf(stderr,
                 "GEKKOAOT_NATIVE_JAUDIO_V245=1 phase=sync frame=%llu subframes=%u samples=%zu master=%04x voices_total=%llu scanned=%llu full=%llu skipped=%llu clips=%llu vpb_path=%s out=%08x/%08x\n",
                 static_cast<unsigned long long>(sync_frames_), subframes, output_samples, master_level_,
                 static_cast<unsigned long long>(voices_mixed_),
                 static_cast<unsigned long long>(voice_slots_scanned_),
                 static_cast<unsigned long long>(voice_full_loads_),
                 static_cast<unsigned long long>(voice_slots_skipped_),
                 static_cast<unsigned long long>(output_clips_),
                 vpb_table ? "direct" : "copy-fallback", left_address, right_address);
  return true;
}

// Protocol and microcode profiles cross-checked against Dolphin Zelda.cpp
// (GPL-2.0-or-later). No guest microcode is embedded in this native renderer.
bool NativeJAudioHLE::ProbeStandardTask(std::uint32_t address, std::uint32_t bytes) {
  Reset();
  if (!dsp_ || bytes == 0u || bytes > 0x10000u) return false;
  const auto* code = dsp_->CoreResolveMain(address, bytes);
  if (!code) return false;
  std::uint32_t hash = 0;
  for (std::uint32_t i = 0; i < bytes; ++i) {
    hash ^= code[i];
    hash = (hash << 3u) | (hash >> 29u);
  }
  if (hash != 0x2fcdf1ecu && hash != 0x86840740u) return false;
  standard_task_ = true;
  standard_image_hash_ = hash;
  dolby_louder_ = hash == 0x2fcdf1ecu;
  std::fprintf(stderr, "GEKKOAOT_NATIVE_JAUDIO_STANDARD_V246=1 hash=%08x protocol=voice-sync dolby_louder=%u\n", hash, unsigned(dolby_louder_));
  return true;
}

void NativeJAudioHLE::StartStandardTask() {
  standard_ready_ = true;
  standard_replies_.push_back({0xf3551111u, false});
}

void NativeJAudioHLE::QueueStandardAck(std::uint16_t token) {
  standard_replies_.push_back({0xdcd10004u, true, 0x2000u});
  standard_replies_.push_back({0xf3550000u | token, false});
}

void NativeJAudioHLE::AdvanceStandardMail(std::uint64_t cycles) {
  if (!standard_ready_ || !dsp_ || dsp_->DspMailboxPending() || standard_replies_.empty()) return;
  auto& queued = standard_replies_.front();
  // Sticky DSP_CONTROL causes must be acknowledged before the next IRQ.
  // Posting it inside syncDSP would let the old handler clear the new cause.
  if (queued.irq && (dsp_->Control() & 0x80u) != 0u) return;
  if (cycles < queued.delay) { queued.delay -= cycles; return; }
  const auto reply = queued;
  standard_replies_.pop_front();
  dsp_->PostDspMailbox(reply.value, reply.irq);
}

std::uint64_t NativeJAudioHLE::CyclesUntilStandardMail() const {
  if (!standard_ready_ || !dsp_ || dsp_->DspMailboxPending() || standard_replies_.empty())
    return std::numeric_limits<std::uint64_t>::max();
  return std::max<std::uint64_t>(standard_replies_.front().delay, 1u);
}

void NativeJAudioHLE::RenderStandard() {
  while (standard_frame_ < standard_frames_) {
    if (standard_voice_ == 0u && standard_max_voice_ == 0u) BeginSubframe();
    const auto end = std::min(standard_max_voice_, voice_count_);
    RenderVoiceRange(standard_voice_, end, &standard_masks_);
    standard_voice_ = end;
    if (standard_voice_ < voice_count_) return;
    std::array<std::int16_t, kSamplesPerSubframe> left{}, right{};
    FinishSubframe(left.data(), right.data());
    (void)WriteMainS16(standard_left_, left.data(), left.size());
    (void)WriteMainS16(standard_right_, right.data(), right.size());
    standard_left_ += kSamplesPerSubframe * 2u;
    standard_right_ += kSamplesPerSubframe * 2u;
    QueueStandardAck(static_cast<std::uint16_t>(0xff00u | standard_frame_));
    ++standard_frame_;
    ++sync_frames_;
    if (sync_frames_ <= 8u || (sync_frames_ & 255u) == 0u)
      std::fprintf(stderr, "GEKKOAOT_JAUDIO_STANDARD_RENDER frame=%llu voices=%llu clips=%llu\n", static_cast<unsigned long long>(sync_frames_), static_cast<unsigned long long>(voices_mixed_), static_cast<unsigned long long>(output_clips_));
    standard_voice_ = standard_max_voice_ = 0;
    if (standard_frame_ < standard_frames_) {
      BeginSubframe();
      return;
    }
  }
  standard_replies_.push_back({0xdcd10003u, true});
  standard_blocked_ = true;
}

bool NativeJAudioHLE::HandleStandardMail(std::uint32_t mail) {
  if (!standard_task_ || !standard_ready_) return false;
  const auto payload = mail & 0x7fffffffu;
  static const bool trace = [] { const char* value = std::getenv("GEKKOAOT_TRACE_DSP"); return value && std::strcmp(value, "0") != 0; }();
  static unsigned trace_mails = 0;
  if (trace && trace_mails++ < 1024u)
    std::fprintf(stderr, "GEKKOAOT_JAUDIO_STANDARD_MAIL state=%u mail=%08x packet=%u/%u frame=%u/%u voice=%u blocked=%u\n", unsigned(standard_state_), mail, standard_seen_, standard_expected_, standard_frame_, standard_frames_, standard_voice_, unsigned(standard_blocked_));
  switch (standard_state_) {
  case StandardState::Upload: {
    standard_upload_[standard_upload_seen_++] = payload;
    if (standard_upload_seen_ != standard_upload_.size()) return true;
    const auto bytes = standard_upload_[4] & 0xffffu;
    const auto* code = dsp_->CoreResolveMain(standard_upload_[3], bytes);
    std::uint32_t hash = 0;
    if (code) for (std::uint32_t i = 0; i < bytes; ++i) {
      hash ^= code[i]; hash = (hash << 3u) | (hash >> 29u);
    }
    // JAS yields and restores the same renderer task at its resume entry.
    // Preserve native renderer state rather than rebooting it. A different
    // image or a DRAM download is not admitted by this self-restore path.
    if (!code || bytes == 0u || hash != standard_image_hash_ ||
        (standard_upload_[1] & 0xffffu) != 0u || standard_upload_[5] != 0u) {
      standard_state_ = StandardState::Halted;
      std::fprintf(stderr, "GEKKOAOT_JAUDIO_STANDARD_UPLOAD_UNSUPPORTED hash=%08x bytes=%u\n", hash, bytes);
      return true;
    }
    standard_state_ = StandardState::Waiting;
    standard_blocked_ = false;
    standard_replies_.push_back({0xdcd10001u, true, 0x2000u});
    RunPendingStandardPackets();
    return true;
  }
  case StandardState::Halted: return true;
  case StandardState::SyncMask: {
    const auto group = (payload >> 16u) & 0xffu;
    if (group >= standard_masks_.size()) { standard_state_ = StandardState::Halted; return true; }
    standard_masks_[group] = static_cast<std::uint16_t>(payload);
    standard_max_voice_ = (group + 1u) * 16u;
    RenderStandard();
    standard_state_ = StandardState::Waiting;
    return true;
  }
  case StandardState::Waiting:
    if ((payload & 0xffff0000u) == 0x4dd10000u) {
      // CONTINUE releases the next work packet after DSP_FRAME_END.
      if ((payload & 0xffffu) == 1u) {
        standard_state_ = StandardState::Upload;
        standard_upload_seen_ = 0;
      } else if ((payload & 0xffffu) == 3u) {
        standard_blocked_ = false;
        RunPendingStandardPackets();
      }
      else standard_state_ = StandardState::Halted;
      return true;
    }
    if ((payload & 0xffffu) == 0u) {
      if (standard_frame_ < standard_frames_) standard_state_ = StandardState::SyncMask;
      return true;
    }
    standard_expected_ = payload & 0xffffu;
    standard_seen_ = 0;
    if (standard_expected_ > standard_packet_.size()) {
      standard_state_ = StandardState::Halted;
      return true;
    }
    standard_state_ = StandardState::Packet;
    return true;
  case StandardState::Packet:
    standard_packet_[standard_seen_++] = mail;
    if (standard_seen_ != standard_expected_) return true;
    standard_state_ = StandardState::Waiting;
    if (standard_blocked_ || standard_frame_ < standard_frames_) {
      if (standard_pending_packets_.size() >= 64u) standard_state_ = StandardState::Halted;
      else standard_pending_packets_.push_back({standard_packet_, standard_expected_});
    } else ExecuteStandardPacket(standard_packet_, standard_expected_);
    return true;
  }
  return true;
}

void NativeJAudioHLE::ExecuteStandardPacket(const std::array<std::uint32_t, 32>& packet, unsigned count) {
    const auto token = static_cast<std::uint16_t>(packet[0] >> 16u);
    switch (token & 0x7f00u) {
    case 0x0100u:
      if (Setup(packet.data(), count)) QueueStandardAck(token);
      break;
    case 0x0200u:
      if (active_ && count == 3u) {
        master_level_ = static_cast<std::uint16_t>(packet[0]);
        standard_frames_ = token & 0xffu;
        standard_frame_ = standard_voice_ = standard_max_voice_ = 0u;
        standard_masks_.fill(0);
        standard_left_ = packet[1] & 0x3fffffffu;
        standard_right_ = packet[2] & 0x3fffffffu;
        RenderStandard();
      }
      break;
    case 0x0000u: case 0x0300u: case 0x0cu << 8u: case 0x0eu << 8u: case 0x0fu << 8u:
      QueueStandardAck(token);
      break;
    case 0x0d00u:
      // GameCube ARAM-base command has no effect on the ARAM device.
      QueueStandardAck(token);
      break;
    default: standard_state_ = StandardState::Halted; break;
    }
}

void NativeJAudioHLE::RunPendingStandardPackets() {
  while (!standard_blocked_ && standard_frame_ == standard_frames_ &&
         standard_state_ != StandardState::Halted && !standard_pending_packets_.empty()) {
    auto next = standard_pending_packets_.front();
    standard_pending_packets_.pop_front();
    ExecuteStandardPacket(next.words, next.count);
  }
}

bool NativeJAudioHLE::HandlePacket(const std::uint32_t* words, std::size_t count) {
  if (!words || count == 0u) return false;
  const std::uint16_t token = static_cast<std::uint16_t>(words[0] >> 16u);
  const std::uint16_t family = token & 0xff00u;
  if (family == kCommandSetup) return Setup(words, count);
  if (family == kCommandSync) return Sync(words, count);
  if (family == kCommandWait || family == kCommandIpl || family == kCommandAgb) return true;
  return false;
}

} // namespace GekkoAOT::DSP
