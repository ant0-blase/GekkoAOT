// SPDX-License-Identifier: GPL-3.0-or-later
#include "card/sdk_card.h"
#include <algorithm>
#include <cstring>
#include <limits>
#include <thread>
namespace GekkoAOT::Card {
namespace {
unsigned U16(const std::uint8_t *p) { return (unsigned(p[0]) << 8) | p[1]; }
void P16(std::uint8_t *p, unsigned v) {
  p[0] = v >> 8;
  p[1] = v;
}
void P32(std::uint8_t *p, std::uint32_t v) {
  p[0] = v >> 24;
  p[1] = v >> 16;
  p[2] = v >> 8;
  p[3] = v;
}
void Sum(std::uint8_t *p, unsigned first, unsigned end, unsigned target) {
  std::uint16_t a = 0, b = 0;
  for (auto i = first; i < end; i += 2) {
    const auto w = U16(p + i);
    a += w;
    b += std::uint16_t(~w);
  }
  P16(p + target, a == 65535 ? 0 : a);
  P16(p + target + 2, b == 65535 ? 0 : b);
}
bool ValidSum(const std::uint8_t *p, unsigned first, unsigned end,
              unsigned target) {
  std::uint16_t a = 0, b = 0;
  for (auto i = first; i < end; i += 2) {
    const auto w = U16(p + i);
    a += w;
    b += std::uint16_t(~w);
  }
  return U16(p + target) == (a == 65535 ? 0 : a) &&
         U16(p + target + 2) == (b == 65535 ? 0 : b);
}
bool Newer(unsigned a, unsigned b) { return std::int16_t(a - b) > 0; }
bool NameEqual(const std::uint8_t *p, std::span<const std::uint8_t> name) {
  return name.size() <= 32 && std::equal(name.begin(), name.end(), p) &&
         (name.size() == 32 || p[name.size()] == 0);
}
} // namespace
SdkCard::~SdkCard() {
  if (pending_)
    Wait();
  worker_.Stop();
}
std::uint8_t *SdkCard::Entry(unsigned file) const {
  return image_->Image().data() + dir_ * Block + file * 64;
}
std::uint8_t *SdkCard::Fat() const {
  return image_->Image().data() + bat_ * Block;
}
void SdkCard::SetDisk(std::span<const std::uint8_t, 6> id, bool unrestricted) {
  std::copy(id.begin(), id.end(), disk_.begin());
  unrestricted_ = unrestricted;
}
unsigned SdkCard::Encode() const {
  return image_ && image_->Ready() ? U16(image_->Image().data() + 0x24) : 0;
}
int SdkCard::Mount(NativeHW::EXI::HostCard *image) {
  if (pending_)
    return Busy;
  image_ = image;
  mounted_ = false;
  if (!image_ || !image_->Ready())
    return result_ = NoCard;
  const auto r = Validate();
  if (r < 0)
    return result_ = r;
  // Read-only mounts need only metadata capacity. Grow for an actual write
  // range if necessary instead of reserving an entire RAW image per slot.
  undo_.reserve(5 * Block);
  if (!worker_started_) {
    worker_.Start(this, &SdkCard::Persist);
    worker_started_ = true;
  }
  mounted_ = true;
  revision_ = image_->Revision();
  return result_ = Ready;
}
int SdkCard::Unmount() {
  if (pending_)
    return Busy;
  mounted_ = false;
  return result_ = Ready;
}
int SdkCard::Synchronize() {
  if (!mounted_)
    return NoCard;
  if (pending_)
    return Busy;
  if (image_->Revision() != revision_) {
    const auto r = Validate();
    if (r < 0) {
      mounted_ = false;
      return result_ = r;
    }
    revision_ = image_->Revision();
  }
  return Ready;
}
bool SdkCard::MetadataCopiesValid() const {
  if(!image_ || !image_->Ready())return false;
  const auto raw=image_->Image();if(raw.size()<5*Block)return false;
  for(unsigned n=1;n<=2;++n)
    if(!ValidSum(raw.data()+n*Block,0,0x1ffc,0x1ffc))return false;
  for(unsigned n=3;n<=4;++n)
    if(!ValidSum(raw.data()+n*Block,4,Block,0))return false;
  return true;
}
int SdkCard::Check() {
  if (pending_)
    return Busy;
  if (!mounted_)
    return NoCard;
  return result_ = Validate();
}
int SdkCard::Format(unsigned encoding) {
  if (pending_)
    return Busy;
  if (!image_ || !image_->Ready())
    return NoCard;
  if (encoding > 1)
    return Fatal;
  auto raw = image_->Image();
  Begin(5 * Block, raw.size() - 5 * Block);
  std::fill(raw.begin() + Block, raw.end(), 0xff);
  P16(raw.data() + 0x24, encoding);
  Sum(raw.data(), 0, 0x1fc, 0x1fc);
  for (unsigned n = 1; n <= 2; ++n) {
    auto *dir = raw.data() + n * Block;
    P16(dir + 0x1ffa, 0);
    Sum(dir, 0, 0x1ffc, 0x1ffc);
  }
  for (unsigned n = 3; n <= 4; ++n) {
    auto *bat = raw.data() + n * Block;
    std::fill_n(bat, Block, 0);
    P16(bat + 6, raw.size() / Block - 5);
    P16(bat + 8, 4);
    Sum(bat, 4, Block, 0);
  }
  const int r = Validate();
  mounted_ = r == Ready;
  return result_ = r;
}
int SdkCard::Validate() {
  const auto raw = image_->Image();
  const auto count = raw.size() / Block;
  if (count < 64 || count > 2048 || (count & (count - 1)) ||
      !ValidSum(raw.data(), 0, 0x1fc, 0x1fc) ||
      U16(raw.data() + 0x22) != raw.size() / 0x20000u)
    return Broken;
  const auto *d1 = raw.data() + Block;
  const auto *d2 = d1 + Block;
  const auto *b1 = raw.data() + 3 * Block;
  const auto *b2 = b1 + Block;
  const bool vd1 = ValidSum(d1, 0, 0x1ffc, 0x1ffc),
             vd2 = ValidSum(d2, 0, 0x1ffc, 0x1ffc);
  const bool vb1 = ValidSum(b1, 4, Block, 0), vb2 = ValidSum(b2, 4, Block, 0);
  if ((!vd1 && !vd2) || (!vb1 && !vb2))
    return Broken;
  dir_ = (!vd1 || (vd2 && Newer(U16(d2 + 0x1ffa), U16(d1 + 0x1ffa)))) ? 2 : 1;
  bat_ = (!vb1 || (vb2 && Newer(U16(b2 + 4), U16(b1 + 4)))) ? 4 : 3;
  std::array<bool, 2048> used{};
  unsigned owned = 0;
  const auto *fat = Fat();
  for (unsigned f = 0; f < Files; ++f) {
    auto &chain = files_[f].blocks;
    chain.clear();
    const auto *e = Entry(f);
    if (e[0] == 255)
      continue;
    const auto n = U16(e + 56);
    if (!n || n > count - 5)
      return Broken;
    chain.reserve(n);
    auto block = U16(e + 54);
    for (unsigned j = 0; j < n; ++j) {
      if (block < 5 || block >= count || used[block])
        return Broken;
      used[block] = true;
      ++owned;
      chain.push_back(block);
      block = U16(fat + 10 + 2 * (block - 5));
    }
    if (block != 65535)
      return Broken;
  }
  unsigned free = 0;
  for (unsigned block = 5; block < count; ++block) {
    const auto v = U16(fat + 10 + 2 * (block - 5));
    if (!v)
      ++free;
    else if (!used[block])
      return Broken;
  }
  if (free != U16(fat + 6) || free + owned != count - 5)
    return Broken;
  return Ready;
}
int SdkCard::Access(int file, bool write) const {
  if (!mounted_)
    return NoCard;
  if (pending_)
    return Busy;
  if (file < 0 || file >= int(Files))
    return Fatal;
  const auto *e = Entry(file);
  if (e[0] == 255)
    return NoFile;
  if (unrestricted_ || std::equal(disk_.begin(), disk_.end(), e))
    return Ready;
  // SDK shared-file permissions (global/company) and public read bit.
  const bool noneGame = std::all_of(e, e + 4, [](auto c) { return c == 0; });
  const bool noneMaker = e[4] == 0 && e[5] == 0;
  if (((e[52] & 0x20) && noneGame && noneMaker) ||
      ((e[52] & 0x40) && noneGame && e[4] == disk_[4] && e[5] == disk_[5]))
    return Ready;
  if (!write && (e[52] & 4))
    return Ready;
  return NoPerm;
}
int SdkCard::FastOpen(int file, Info &info) const {
  const int r = Access(file, false);
  if (r < 0)
    return r;
  info = {file, 0, 0, files_[file].blocks.front()};
  return Ready;
}
int SdkCard::Open(std::span<const std::uint8_t> name, Info &info) const {
  if (!mounted_)
    return NoCard;
  if (pending_)
    return Busy;
  if (name.size() > 32)
    return NameTooLong;
  for (unsigned f = 0; f < Files; ++f) {
    const auto *e = Entry(f);
    if (e[0] != 255 &&
        (unrestricted_ || std::equal(disk_.begin(), disk_.end(), e)) &&
        NameEqual(e + 8, name))
      return FastOpen(f, info);
  }
  return NoFile;
}
int SdkCard::PrepareRead(Info &info, std::uint32_t bytes, std::uint32_t offset) const {
  const int r = Access(info.file, false);
  if (r < 0)
    return r;
  if (!bytes || (bytes & 511u) || (offset & 511u))
    return Fatal;
  const auto &chain = files_[info.file].blocks;
  const auto size = chain.size() * Block;
  if (offset > size || bytes > size - offset)
    return Limit;
  info.offset=offset;info.length=bytes;info.block=chain[offset/Block];
  return Ready;
}
int SdkCard::Read(Info &info, std::span<std::uint8_t> out,
                  std::uint32_t offset) {
  if(out.size()>std::numeric_limits<std::uint32_t>::max())return Fatal;
  const auto r=PrepareRead(info,out.size(),offset);if(r<0)return r;
  const auto &chain=files_[info.file].blocks;
  auto remaining = out.size();
  auto *dst = out.data();
  auto pos = offset;
  while (remaining) {
    const auto n = std::min<std::size_t>(remaining, Block - (pos % Block));
    std::memcpy(
        dst, image_->Image().data() + chain[pos / Block] * Block + pos % Block,
        n);
    pos += n;
    dst += n;
    remaining -= n;
  }
  const auto last = (pos - 1) / Block;
  info.offset = std::max<std::uint32_t>(offset, last * Block);
  info.block = chain[last];
  info.length = static_cast<std::int32_t>(pos) -
                static_cast<std::int32_t>((last + 1) * Block);
  return result_ = Ready;
}
void SdkCard::Begin(std::uint32_t offset, std::uint32_t size) {
  const auto raw = image_->Image();
  undo_.resize(5 * Block + size);
  std::memcpy(undo_.data(), raw.data(), 5 * Block);
  if (size)
    std::memcpy(undo_.data() + 5 * Block, raw.data() + offset, size);
  undo_file_ = -1;
  undo_offset_ = offset;
  undo_size_ = size;
  transaction_ = true;
}
int SdkCard::Write(Info &info, std::span<const std::uint8_t> in,
                   std::uint32_t offset) {
  const int r = Access(info.file, true);
  if (r < 0)
    return r;
  if (in.empty() || (in.size() % Block) || (offset % Block))
    return Fatal;
  const auto &chain = files_[info.file].blocks;
  const auto size = chain.size() * Block;
  if (offset > size || in.size() > size - offset)
    return Limit;
  // Back up just the modified file blocks, not the entire card image. Chains
  // may be fragmented, so undo storage is linear in the requested payload.
  undo_.resize(5 * Block + in.size());
  std::memcpy(undo_.data(), image_->Image().data(), 5 * Block);
  for (unsigned n = 0; n < in.size() / Block; ++n)
    std::memcpy(undo_.data() + 5 * Block + n * Block,
                image_->Image().data() + chain[offset / Block + n] * Block,
                Block);
  undo_offset_ = offset;
  undo_size_ = in.size();
  transaction_ = true;
  // Encode file number with a flag to distinguish fragmented write undo.
  undo_file_ = info.file;
  for (unsigned n = 0; n < in.size() / Block; ++n)
    std::memcpy(image_->Image().data() + chain[offset / Block + n] * Block,
                in.data() + n * Block, Block);
  info.offset = offset + in.size() - Block;
  info.block = chain[info.offset / Block];
  info.length = 0;
  return result_ = Ready;
}
int SdkCard::Create(std::span<const std::uint8_t> name, std::uint32_t bytes,
                    std::uint32_t time, Info &info) {
  if (!mounted_)
    return NoCard;
  if (pending_)
    return Busy;
  if (name.size() > 32)
    return NameTooLong;
  if (!bytes || (bytes % Block))
    return Fatal;
  Info old;
  if (Open(name, old) == Ready)
    return Exists;
  unsigned file = 0;
  while (file < Files && Entry(file)[0] != 255)
    ++file;
  if (file == Files)
    return NoEntries;
  if (bytes > unsigned(FreeBytes()))
    return NoSpace;
  Begin();
  auto *fat = Fat();
  auto &chain = files_[file].blocks;
  chain.clear();
  chain.reserve(bytes / Block);
  for (unsigned block = 5;
       block < Capacity() / Block && chain.size() < bytes / Block; ++block)
    if (!U16(fat + 10 + 2 * (block - 5)))
      chain.push_back(block);
  if (chain.size() != bytes / Block) {
    Finish(false);
    return Broken;
  }
  for (unsigned i = 0; i < chain.size(); ++i)
    P16(fat + 10 + 2 * (chain[i] - 5),
        i + 1 < chain.size() ? chain[i + 1] : 65535);
  P16(fat + 6, U16(fat + 6) - chain.size());
  P16(fat + 8, chain.back());
  auto *e = Entry(file);
  std::fill_n(e, 64, 0);
  std::copy(disk_.begin(), disk_.end(), e);
  std::copy(name.begin(), name.end(), e + 8);
  P32(e + 40, time);
  P32(e + 44, 0xffffffffu);
  e[52] = 4;
  P16(e + 54, chain.front());
  P16(e + 56, chain.size());
  P16(e + 58, 65535);
  P32(e + 60, 0xffffffffu);
  // Allocation changes only directory/BAT metadata, as the SDK does.
  info = {int(file), 0, 0, chain.front()};
  UpdateMetadata(true);
  return result_ = Ready;
}
int SdkCard::Delete(int file) {
  const int r = Access(file, true);
  if (r < 0)
    return r;
  Begin();
  auto *fat = Fat();
  const auto n = files_[file].blocks.size();
  for (auto block : files_[file].blocks)
    P16(fat + 10 + 2 * (block - 5), 0);
  P16(fat + 6, U16(fat + 6) + n);
  std::fill_n(Entry(file), 64, 0xff);
  files_[file].blocks.clear();
  UpdateMetadata(true);
  return result_ = Ready;
}
void SdkCard::SetTimestamp(int file, std::uint32_t seconds) {
  if (transaction_ && file >= 0 && file < int(Files)) {
    P32(Entry(file) + 40, seconds);
    UpdateMetadata();
  }
}
void SdkCard::UpdateMetadata(bool fat_changed) {
  auto raw = image_->Image();
  auto *d = raw.data() + dir_ * Block;
  auto *b = Fat();
  P16(d + 0x1ffa, U16(d + 0x1ffa) + 1);
  if (fat_changed)
    P16(b + 4, U16(b + 4) + 1);
  Sum(d, 0, 0x1ffc, 0x1ffc);
  if (fat_changed)
    Sum(b, 4, Block, 0);
  std::memcpy(raw.data() + (dir_ == 1 ? 2 : 1) * Block, d, Block);
  if (fat_changed)
    std::memcpy(raw.data() + (bat_ == 3 ? 4 : 3) * Block, b, Block);
}
int SdkCard::FreeBytes() const {
  if (!mounted_)
    return NoCard;
  if (pending_)
    return Busy;
  return U16(Fat() + 6) * Block;
}
int SdkCard::FreeFiles() const {
  if (!mounted_)
    return NoCard;
  if (pending_)
    return Busy;
  unsigned n = 0;
  for (unsigned f = 0; f < Files; ++f)
    n += Entry(f)[0] == 255;
  return n;
}
int SdkCard::GetStatus(int file, std::span<std::uint8_t, 108> out) const {
  const int r = Access(file, false);
  if (r < 0)
    return r;
  const auto *e = Entry(file);
  std::fill(out.begin(), out.end(), 0);
  std::memcpy(out.data(), e + 8, 32);
  P32(out.data() + 32, U16(e + 56) * Block);
  std::memcpy(out.data() + 36, e + 40, 4);
  std::memcpy(out.data() + 40, e, 6);
  out[46] = e[7];
  std::memcpy(out.data() + 48, e + 44, 8);
  std::memcpy(out.data() + 56, e + 60, 4);
  // SDK banner/icon offsets inside the file; preserve raw graphics metadata.
  std::uint32_t offset = (std::uint32_t(e[44]) << 24) |
                         (std::uint32_t(e[45]) << 16) |
                         (std::uint32_t(e[46]) << 8) | e[47];
  if (offset == 0xffffffffu) {
    offset = 0;
    out[46] = 0;
    P16(out.data() + 52, 0);
    P16(out.data() + 54, 0);
  }
  const auto absent = 0xffffffffu;
  const auto banner = e[7] & 3u;
  P32(out.data() + 60, banner ? offset : absent);
  P32(out.data() + 64, banner == 1 ? offset + 3072 : absent);
  if (banner == 1)
    offset += 3584;
  else if (banner == 2)
    offset += 6144;
  bool ci8 = false;
  for (unsigned i = 0; i < 8; ++i) {
    const auto fmt = (U16(e + 48) >> (2 * i)) & 3;
    P32(out.data() + 68 + 4 * i, (fmt == 1 || fmt == 2) ? offset : absent);
    if (fmt == 1) {
      offset += 1024;
      ci8 = true;
    } else if (fmt == 2)
      offset += 2048;
  }
  P32(out.data() + 100, ci8 ? offset : absent);
  if (ci8)
    offset += 512;
  P32(out.data() + 104, offset);
  return Ready;
}
int SdkCard::SetStatus(int file, std::span<const std::uint8_t, 108> in) {
  const int r = Access(file, true);
  if (r < 0)
    return r;
  const auto u32 = [](const std::uint8_t *p) {
    return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) |
           (std::uint32_t(p[2]) << 8) | p[3];
  };
  const auto icon = u32(in.data() + 48), comment = u32(in.data() + 56);
  if ((icon != 0xffffffffu && icon >= 512) ||
      (comment != 0xffffffffu && (comment % Block) > Block - 64))
    return Fatal;
  Begin();
  auto *e = Entry(file);
  e[7] = in[46];
  std::memcpy(e + 44, in.data() + 48, 8);
  std::memcpy(e + 60, in.data() + 56, 4);
  std::memcpy(e + 40, in.data() + 36, 4);
  UpdateMetadata();
  return result_ = Ready;
}
bool SdkCard::Persist(void *self, std::uint64_t, void *, std::uint32_t) {
  auto *card = static_cast<SdkCard *>(self);
  return card->image_->CommitImage(&card->cancelled_);
}
bool SdkCard::Submit() {
  if (pending_ || !transaction_)
    return false;
  pending_ = worker_.Submit(1, 0, 0);
  cancelled_ = false;
  return pending_;
}
void SdkCard::Finish(bool success) {
  if (transaction_ && !success) {
    auto raw = image_->Image();
    if (undo_size_) {
      if (undo_file_ >= 0) {
        const auto file = undo_file_;
        const auto offset = undo_offset_;
        for (unsigned n = 0; n < undo_size_ / Block; ++n)
          std::memcpy(raw.data() +
                          files_[file].blocks[offset / Block + n] * Block,
                      undo_.data() + 5 * Block + n * Block, Block);
      } else
        std::memcpy(raw.data() + undo_offset_, undo_.data() + 5 * Block,
                    undo_size_);
    }
    std::memcpy(raw.data(), undo_.data(), 5 * Block);
    Validate();
  }
  if (success) {
    image_->Changed();
    revision_ = image_->Revision();
  }
  transaction_ = false;
  undo_size_ = 0;
  undo_.clear();
}
bool SdkCard::Complete(int &result) {
  if (!pending_)
    return false;
  Native::DiscIoWorker::Result r;
  if (!worker_.TryCollect(r, worker_unused_))
    return false;
  pending_ = false;
  Finish(r.ok);
  result_ = cancelled_ ? Cancelled : (r.ok ? Ready : Io);
  cancelled_ = false;
  result = result_;
  return true;
}
int SdkCard::Wait() {
  if (!pending_)
    return result_;
  Native::DiscIoWorker::Result r;
  if (!worker_.WaitCollect(r, worker_unused_))
    return Io;
  pending_ = false;
  Finish(r.ok);
  result_ = cancelled_ ? Cancelled : (r.ok ? Ready : Io);
  cancelled_ = false;
  return result_;
}
} // namespace GekkoAOT::Card
