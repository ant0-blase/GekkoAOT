// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
namespace GekkoAOT::GX {
// Optional C ABI payload. Parameters and byte position only; no guest/host
// pointers, CPUState or native endian ambiguity. Owned by the host queue slot.
struct NativeArrayCommand {std::uint32_t offset,index,base,stride;};
static_assert(sizeof(NativeArrayCommand)==16);
// v317: ordered host operations. Matrix bytes are immutable upload data, not
// instructions: only the compatibility adapter observes their old wire header.
enum class NativeSdkKind : std::uint32_t { Array, Matrix };
struct NativeSdkCommand {
  std::uint32_t offset;
  NativeSdkKind kind;
  std::uint32_t a,b,c;
};
static_assert(sizeof(NativeSdkCommand)==20);
inline bool NativeMatrixRange(std::uint32_t address,std::uint32_t words) noexcept {
  if(address<0x100u)
    return !(address&3u) && (words==8u||words==12u) && words<=0x100u-address;
  if(address>=0x400u&&address<0x460u)
    return (address-0x400u)%3u==0u && words==9u && words<=0x460u-address;
  return address>=0x500u&&address<0x5f0u && (address-0x500u)%12u==0u &&
         words==12u && words<=0x5f0u-address;
}
inline std::uint32_t NativeSdkWireBytes(const NativeSdkCommand& command) noexcept {
  return command.kind==NativeSdkKind::Array?12u:5u+4u*command.b;
}
// Ordered typed SDK operations at positions in the compatibility transport.
// Only the adapter checks encoding. The native renderer receives parameters,
// without decoding CP commands. Unsupported/mutated/fractured command streams
// fall through byte-for-byte, including incomplete commands across bursts.
class NativeCommandStream {
public:
  struct ArrayBinding {std::uint32_t index,base,stride;};
private:
  struct Pending {
    ArrayBinding binding{};
    std::uint32_t matrix_address=0,matrix_words=0,size=12;
    std::uint64_t position=0;
    std::array<std::uint8_t,12> expected{};
    std::array<std::uint8_t,53> captured{};
    unsigned count=0;
  };
  std::array<Pending,16> pending_{};
  unsigned head_=0,count_=0;
  bool direct_spans_=true;
  std::uint64_t position_=0,native_=0,matrices_=0,fallback_=0,declined_=0;
public:
  void Reset() noexcept {*this={};}
  void SetDirectSpansEnabled(bool enabled) noexcept {direct_spans_=enabled;}
  std::uint64_t NativeCalls() const noexcept {return native_;}
  std::uint64_t MatrixCalls() const noexcept {return matrices_;}
  std::uint64_t FallbackCalls() const noexcept {return fallback_;}
  std::uint64_t DeclinedHints() const noexcept {return declined_;}
  bool Stage(ArrayBinding binding,unsigned before) noexcept {
    if(binding.index>=16 || before>=32 || count_==pending_.size()) {++declined_;return false;}
    const auto at=position_+before;
    if(count_) {
      const auto& last=pending_[(head_+count_-1)%pending_.size()];
      if(at<last.position+last.size) {++declined_;return false;}
    }
    binding.base&=0x3fffffffu;binding.stride&=255;
    auto& item=pending_[(head_+count_)%pending_.size()];
    item.binding=binding;item.position=at;item.count=0;item.matrix_words=0;item.size=12;
    const auto base=binding.base;
    item.expected={8,std::uint8_t(0xa0|binding.index),std::uint8_t(base>>24),std::uint8_t(base>>16),
      std::uint8_t(base>>8),std::uint8_t(base),8,std::uint8_t(0xb0|binding.index),0,0,0,std::uint8_t(binding.stride)};
    ++count_;return true;
  }
  bool StageMatrix(std::uint32_t address,std::uint32_t words,unsigned before) noexcept {
    if(!NativeMatrixRange(address,words)||before>=32u||count_==pending_.size()) {
      ++declined_;return false;
    }
    const auto at=position_+before;
    if(count_) {
      const auto& last=pending_[(head_+count_-1)%pending_.size()];
      if(at<last.position+last.size) {++declined_;return false;}
    }
    auto& item=pending_[(head_+count_)%pending_.size()];
    item.position=at;item.count=0;item.matrix_address=address;item.matrix_words=words;item.size=5u+4u*words;
    item.expected={0x10u,0u,std::uint8_t(words-1u),std::uint8_t(address>>8u),std::uint8_t(address)};
    ++count_;return true;
  }
  template<class Raw,class Boundary,class Native,class Matrix>
  bool Consume(const std::uint8_t* bytes,std::size_t size,Raw&& raw,Boundary&& boundary,
               Native&& native,Matrix&& matrix) {
    if(!bytes&&size)return false;
    while(size) {
      if(!count_) {if(!raw(bytes,size))return false;position_+=size;return true;}
      auto& item=pending_[head_];
      if(position_<item.position) {
        const auto prefix=std::min<std::size_t>(size,item.position-position_);
        if(!raw(bytes,prefix))return false;
        position_+=prefix;bytes+=prefix;size-=prefix;continue;
      }
      // Complete immutable commands already belong to this batch. Borrow the
      // upload span for the duration of the call; only fragmented commands need
      // a temporary copy. No pointer survives consumption or queue-slot reuse.
      if(direct_spans_ && !item.count && size>=item.size) {
        const auto length=item.size;
        const bool matches=std::memcmp(bytes,item.expected.data(),item.matrix_words?5u:12u)==0;
        if(matches&&boundary()) {
          if(item.matrix_words) {
            if(!matrix(item.matrix_address,item.matrix_words,bytes+5u))return false;
            ++matrices_;
          } else if(!native(item.binding))return false;
          ++native_;
        } else {
          if(!raw(bytes,length))return false;
          ++fallback_;
        }
        position_+=length;bytes+=length;size-=length;
        head_=(head_+1)%pending_.size();--count_;
        continue;
      }
      const auto chunk=std::min<std::size_t>(size,item.size-item.count);
      std::memcpy(item.captured.data()+item.count,bytes,chunk);
      item.count+=static_cast<unsigned>(chunk);position_+=chunk;bytes+=chunk;size-=chunk;
      if(item.count==item.size) {
        const bool matches=std::memcmp(item.captured.data(),item.expected.data(),
                                      item.matrix_words?5u:12u)==0;
        if(matches && boundary()) {
          if(item.matrix_words) {
            if(!matrix(item.matrix_address,item.matrix_words,item.captured.data()+5u))return false;
            ++matrices_;
          } else if(!native(item.binding))return false;
          ++native_;
        } else {
          if(!raw(item.captured.data(),item.size))return false;
          ++fallback_;
        }
        head_=(head_+1)%pending_.size();--count_;
      }
    }
    return true;
  }
  template<class Raw,class Boundary,class Native>
  bool Consume(const std::uint8_t* bytes,std::size_t size,Raw&& raw,Boundary&& boundary,Native&& native) {
    return Consume(bytes,size,raw,boundary,native,
                   [](std::uint32_t,std::uint32_t,const std::uint8_t*){return false;});
  }
};
} // namespace GekkoAOT::GX
