#pragma once
// SPDX-License-Identifier: GPL-3.0-or-later
#include "hw/exi/native_card.h"
#include "native/disc_io_worker.h"
#include <array>
#include <span>
#include <vector>
namespace GekkoAOT::Card {
// Owner: runtime thread. A persistence worker only reads a busy slot's image.
// Files remain standard RAW directory/BAT chains, including banner/icon data.
class SdkCard final {
public:
  static constexpr unsigned Block = 8192, Files = 127;
  enum Error : int {
    Ready = 0,
    Busy = -1,
    NoCard = -3,
    NoFile = -4,
    Io = -5,
    Broken = -6,
    Exists = -7,
    NoEntries = -8,
    NoSpace = -9,
    NoPerm = -10,
    Limit = -11,
    NameTooLong = -12,
    Encoding = -13,
    Cancelled = -14,
    Fatal = -128
  };
  struct Info {
    int file = -1;
    std::uint32_t offset = 0;
    std::int32_t length = 0;
    std::uint16_t block = 0;
  };
  SdkCard() = default;
  ~SdkCard();
  int Mount(NativeHW::EXI::HostCard *);
  int Unmount();
  int Result() const { return pending_ ? Busy : result_; }
  int Check();
  bool MetadataCopiesValid() const;
  int Format(unsigned encoding);
  int Synchronize();
  int Open(std::span<const std::uint8_t> name, Info &) const;
  int FastOpen(int file, Info &) const;
  int Read(Info &, std::span<std::uint8_t>, std::uint32_t offset);
  int PrepareRead(Info &, std::uint32_t bytes, std::uint32_t offset) const;
  int Write(Info &, std::span<const std::uint8_t>, std::uint32_t offset);
  int Create(std::span<const std::uint8_t> name, std::uint32_t bytes,
             std::uint32_t time, Info &);
  int Delete(int file);
  void SetTimestamp(int file, std::uint32_t seconds);
  int GetStatus(int file, std::span<std::uint8_t, 108>) const;
  int SetStatus(int file, std::span<const std::uint8_t, 108>);
  int FreeBytes() const;
  int FreeFiles() const;
  unsigned Capacity() const { return image_ ? image_->Image().size() : 0; }
  unsigned Encode() const;
  unsigned DirectoryBlock() const { return dir_; }
  unsigned AllocationBlock() const { return bat_; }
  bool Mounted() const { return mounted_; }
  void SetDisk(std::span<const std::uint8_t, 6> id, bool unrestricted = false);
  // Persistence has no per-request thread, queue allocation, lock or guest
  // callback. Complete() publishes results on the runtime thread only.
  bool Submit();
  bool Complete(int &result);
  int Wait();
  void Cancel() { cancelled_ = pending_; }
  bool Mutated() const { return transaction_; }

private:
  struct File {
    std::vector<std::uint16_t> blocks;
  };
  int Access(int file, bool write) const;
  int Validate();
  void Begin(std::uint32_t offset = 0, std::uint32_t size = 0);
  void Finish(bool success);
  void UpdateMetadata(bool fat_changed = false);
  std::uint8_t *Entry(unsigned file) const;
  std::uint8_t *Fat() const;
  static bool Persist(void *, std::uint64_t, void *, std::uint32_t);
  NativeHW::EXI::HostCard *image_ = nullptr;
  std::array<File, Files> files_;
  std::array<std::uint8_t, 6> disk_{};
  bool unrestricted_ = false, mounted_ = false, pending_ = false,
       transaction_ = false;
  std::atomic<bool> cancelled_{false};
  int result_ = NoCard;
  unsigned dir_ = 0, bat_ = 0;
  std::uint64_t revision_ = 0;
  std::vector<std::uint8_t> undo_;
  std::uint32_t undo_offset_ = 0, undo_size_ = 0;
  int undo_file_ = -1;
  bool worker_started_ = false;
  Native::DiscIoWorker worker_;
  std::vector<std::uint8_t> worker_unused_;
};
} // namespace GekkoAOT::Card
