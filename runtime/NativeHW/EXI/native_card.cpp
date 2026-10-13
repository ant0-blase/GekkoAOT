// SPDX-License-Identifier: GPL-3.0-or-later
#include "NativeHW/EXI/native_card.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>

namespace GekkoAOT::NativeHW::EXI {
namespace {
void Put16(std::uint8_t* p, std::uint16_t v) {
  p[0] = static_cast<std::uint8_t>(v >> 8); p[1] = static_cast<std::uint8_t>(v);
}
void Put64(std::uint8_t* p, std::uint64_t v) {
  for (unsigned i = 0; i < 8; ++i) p[i] = static_cast<std::uint8_t>(v >> (56u - i * 8u));
}
void Checksum(std::uint8_t* data, std::size_t first, std::size_t end,
              std::size_t checksum) {
  std::uint16_t sum = 0, inverse = 0;
  for (std::size_t i = first; i < end; i += 2) {
    const std::uint16_t word = (std::uint16_t(data[i]) << 8) | data[i + 1];
    sum = static_cast<std::uint16_t>(sum + word);
    inverse = static_cast<std::uint16_t>(inverse + (word ^ 0xffffu));
  }
  Put16(data + checksum, sum == 0xffffu ? 0u : sum);
  Put16(data + checksum + 2u, inverse == 0xffffu ? 0u : inverse);
}
}

HostCard::HostCard(std::filesystem::path path, unsigned slot) : path_(std::move(path)) {
  if (path_.empty() || slot > 1u) return;
  std::error_code error;
  const bool exists = std::filesystem::exists(path_, error);
  if (error) return;
  bytes_.resize(kCardSize);
  if (exists) {
    if (std::filesystem::file_size(path_, error) != kCardSize || error) return;
    std::ifstream file(path_, std::ios::binary);
    if (!file || !file.read(reinterpret_cast<char*>(bytes_.data()), bytes_.size())) return;
  } else {
    Format(slot);
    std::filesystem::create_directories(path_.parent_path(), error);
    if (error) return;
    std::ofstream file(path_, std::ios::binary);
    if (!file || !file.write(reinterpret_cast<const char*>(bytes_.data()), bytes_.size())) return;
  }
  ready_ = true;
}


HostCard::~HostCard() {
  (void)FlushDirty();
}

std::array<std::uint8_t, 12> HostCard::FlashId() const {
  std::array<std::uint8_t, 12> id{};
  if (!ready_) return id;
  std::uint64_t seed = 0;
  for (unsigned i = 0; i < 8; ++i) seed = (seed << 8u) | bytes_[0x0cu + i];
  for (unsigned i = 0; i < id.size(); ++i) {
    seed = (seed * 0x41c64e6dull + 0x3039ull) >> 16u;
    id[i] = static_cast<std::uint8_t>(bytes_[i] - static_cast<std::uint8_t>(seed));
    seed = ((seed * 0x41c64e6dull + 0x3039ull) >> 16u) & 0x7fffull;
  }
  return id;
}

void HostCard::Format(unsigned slot) {
  std::fill(bytes_.begin(), bytes_.end(), 0xffu);
  auto* header = bytes_.data();
  const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
  const std::uint64_t seconds = now > 0x386d4380 ? static_cast<std::uint64_t>(now - 0x386d4380) : 0;
  const std::uint64_t format_ticks = seconds * 40500000ull + slot;
  // The system SRAM starts with zero flash IDs; encoding those into the card
  // header keeps the host-created card and the guest CARD unlock data paired.
  std::uint64_t seed = format_ticks;
  for (unsigned i = 0; i < 12; ++i) {
    seed = (seed * 0x41c64e6dull + 0x3039ull) >> 16;
    header[i] = static_cast<std::uint8_t>(seed);
    seed = ((seed * 0x41c64e6dull + 0x3039ull) >> 16) & 0x7fffull;
  }
  Put64(header + 0x0c, format_ticks);
  std::fill(header + 0x14, header + 0x20, 0);
  Put16(header + 0x20, static_cast<std::uint16_t>(slot));
  Put16(header + 0x22, 16u);
  Put16(header + 0x24, 0u);
  Checksum(header, 0, 0x1fc, 0x1fc);

  for (unsigned block = 1; block <= 2; ++block) {
    auto* dir = bytes_.data() + block * kBlockSize;
    Put16(dir + 0x1ffa, 0u);
    Checksum(dir, 0, 0x1ffc, 0x1ffc);
  }
  for (unsigned block = 3; block <= 4; ++block) {
    auto* bat = bytes_.data() + block * kBlockSize;
    std::fill(bat, bat + kBlockSize, 0);
    Put16(bat + 6, 251u);
    Put16(bat + 8, 4u);
    Checksum(bat, 4, kBlockSize, 0);
  }
}

bool HostCard::Persist(std::uint32_t offset, std::uint32_t length) {
  if (!ready_ || offset > kCardSize || length > kCardSize - offset) return false;
  std::fstream file(path_, std::ios::binary | std::ios::in | std::ios::out);
  if (!file) return false;
  file.seekp(offset);
  file.write(reinterpret_cast<const char*>(bytes_.data() + offset), length);
  file.flush();
  return static_cast<bool>(file);
}

void HostCard::MarkDirty(std::uint32_t offset, std::uint32_t length) {
  if (!ready_ || length == 0u || offset >= kCardSize) return;
  const std::uint32_t end = std::min<std::uint32_t>(kCardSize, offset + length);
  dirty_begin_ = std::min(dirty_begin_, offset);
  dirty_end_ = std::max(dirty_end_, end);
}

bool HostCard::FlushDirty() {
  if (dirty_begin_ >= dirty_end_) return true;
  const std::uint32_t begin = dirty_begin_;
  const std::uint32_t length = dirty_end_ - dirty_begin_;
  if (!Persist(begin, length)) return false;
  dirty_begin_ = kCardSize;
  dirty_end_ = 0u;
  return true;
}

bool HostCard::Select(bool selected) {
  if (!ready_ || selected_ == selected) return false;
  if (!selected_) {
    position_ = 0;
    address_ = 0;
    command_ = 0;
    selected_ = true;
    dma_programmed_ = false;
    return false;
  }
  bool command_done = false;
  if (command_ == 0xf2u && (position_ > 5u || dma_programmed_)) {
    if (position_ > 5u) {
      const std::uint32_t count = std::min(position_ - 5u, 128u);
      for (std::uint32_t i = 0; i < count; ++i) {
        const std::uint32_t offset = (address_ & ~0x1ffu) | ((address_ + i) & 0x1ffu);
        bytes_[offset % kCardSize] = program_[i];
      }
      const std::uint32_t page = (address_ & ~0x1ffu) % kCardSize;
      MarkDirty(page, 0x200u);
    }
    if (!FlushDirty()) status_ |= 0x08u;
    command_done = true;
  } else if (command_ == 0xf1u && position_ > 2u) {
    const std::uint32_t offset = (address_ & ~(kBlockSize - 1u)) % kCardSize;
    std::fill_n(bytes_.data() + offset, kBlockSize, 0xffu);
    MarkDirty(offset, kBlockSize);
    if (!FlushDirty()) status_ |= 0x10u;
    command_done = true;
  }
  selected_ = false;
  dma_programmed_ = false;
  return command_done;
}

bool HostCard::TransferByte(std::uint8_t direction, std::uint8_t* value) {
  if (!value || direction > 1u) return false;
  if (position_ == 0u) {
    if (direction != 1u) return false;
    switch (*value) {
    case 0x00: case 0x52: case 0x53: case 0x81: case 0x82:
    case 0x83: case 0x85: case 0x86: case 0x87: case 0x88:
    case 0x89: case 0xf1: case 0xf2: case 0xf3: case 0xf4:
      command_ = *value;
      if (command_ == 0x89u) {
        status_ = static_cast<std::uint8_t>((status_ & ~0x18u) | 0x01u);
        position_ = 0;
      } else {
        ++position_;
      }
      return true;
    default: return false;
    }
  }
  std::uint8_t result = 0xffu;
  switch (command_) {
  case 0x00: // Nintendo ID and capacity
    result = position_ == 1u ? 0x80u :
             static_cast<std::uint8_t>(0x10u >> ((3u - ((position_ - 2u) & 3u)) * 8u));
    break;
  case 0x52: // Set address, then read card bytes.
  case 0xf2: // Set address, then stage a page program.
    if (position_ == 1u) address_ = static_cast<std::uint32_t>(*value) << 17u;
    if (position_ == 2u) address_ |= static_cast<std::uint32_t>(*value) << 9u;
    if (position_ == 3u) address_ |= static_cast<std::uint32_t>(*value & 3u) << 7u;
    if (position_ == 4u) address_ |= *value & 0x7fu;
    if (command_ == 0x52u && position_ > 1u) {
      result = bytes_[address_ % kCardSize];
      if (position_ >= 9u) address_ = (address_ & ~0x1ffu) | ((address_ + 1u) & 0x1ffu);
    }
    if (command_ == 0xf2u && position_ >= 5u)
      program_[(position_ - 5u) & 0x7fu] = *value;
    break;
  case 0xf1:
    if (position_ == 1u) address_ = static_cast<std::uint32_t>(*value) << 17u;
    if (position_ == 2u) address_ |= static_cast<std::uint32_t>(*value) << 9u;
    break;
  case 0x83: result = status_; break;
  case 0x85: // Nintendo card ID: dummy high byte, then C2 21.
    result = position_ == 1u || !(position_ & 1u) ? 0xc2u : 0x21u;
    break;
  case 0x53: case 0x81: case 0x82: case 0x86:
  case 0x87: case 0x88: case 0xf3: case 0xf4: break;
  default: return false;
  }
  if (direction == 0u) *value = result;
  ++position_;
  return true;
}

bool HostCard::Immediate(std::uint8_t direction, std::uint8_t length, std::uint32_t* data) {
  if (!ready_ || !data || length == 0u || length > 4u || direction > 1u) return false;
  if (!selected_) Select(true);
  const std::uint32_t input = *data;
  const std::uint32_t before = position_;
  std::uint32_t output = 0;
  for (unsigned i = 0; i < length; ++i) {
    std::uint8_t value = static_cast<std::uint8_t>(*data >> (24u - i * 8u));
    if (!TransferByte(direction, &value)) return false;
    output |= static_cast<std::uint32_t>(value) << (24u - i * 8u);
  }
  if (direction == 0u) *data = output;
  static unsigned trace_count = 0;
  static const bool trace = std::getenv("GEKKOAOT_NATIVE_HW_EXI_TRACE") != nullptr;
  if (trace && (command_ != 0x52u || trace_count++ < 24u))
    std::fprintf(stderr, "GEKKOAOT_NATIVE_HW_EXI_TRACE imm dir=%u len=%u pos=%u cmd=%02x in=%08x out=%08x addr=%08x\n",
                 direction, length, before, command_, input, *data, address_);
  return true;
}

bool HostCard::Dma(std::uint8_t direction, std::uint8_t* memory, std::uint32_t length) {
  if (!ready_ || !selected_ || !memory || length == 0u || length > kCardSize ||
      address_ > kCardSize || length > kCardSize - address_) return false;
  static unsigned trace_count = 0;
  static const bool trace = std::getenv("GEKKOAOT_NATIVE_HW_EXI_TRACE") != nullptr;
  if (trace && (command_ != 0x52u || trace_count++ < 24u))
    std::fprintf(stderr, "GEKKOAOT_NATIVE_HW_EXI_TRACE dma dir=%u len=%u pos=%u cmd=%02x addr=%08x\n",
                 direction, length, position_, command_, address_);
  if (direction == 0u && command_ == 0x52u) {
    std::copy_n(bytes_.data() + address_, length, memory);
    return true;
  }
  if (direction == 1u && command_ == 0xf2u) {
    std::copy_n(memory, length, bytes_.data() + address_);
    MarkDirty(address_, length);
    dma_programmed_ = true;
    return true;
  }
  return false;
}
} // namespace GekkoAOT::NativeHW::EXI
