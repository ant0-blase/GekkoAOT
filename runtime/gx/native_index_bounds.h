// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#if !defined(GEKKOAOT_INDEX_BOUNDS_FORCE_SCALAR) && (defined(__SSE2__) || defined(_M_X64))
#include <emmintrin.h>
#define GEKKOAOT_INDEX_BOUNDS_SSE2 1
#elif !defined(GEKKOAOT_INDEX_BOUNDS_FORCE_SCALAR) && (defined(__ARM_NEON) || defined(__ARM_NEON__))
#include <arm_neon.h>
#define GEKKOAOT_INDEX_BOUNDS_NEON 1
#endif

namespace GekkoAOT::GX {
// VCD/VAT compilation validates each field once. Execute scans the immutable
// command bytes field-by-field: index width and NBT3 count are outside the
// vertex loop. It never caches vertex contents or guest array mappings.
class NativeIndexBounds {
public:
  static constexpr unsigned MaxFields=12;
  struct Field { std::uint16_t offset; std::uint8_t width,count; };
  struct Bounds { std::uint32_t minimum=65535,maximum=0; };
  bool Compile(unsigned stride,std::span<const Field> fields) noexcept {
    *this={};
    if(!stride || stride>65535 || fields.empty() || fields.size()>MaxFields)return false;
    for(const auto& field:fields)
      if((field.width!=1 && field.width!=2) || (field.count!=1 && field.count!=3) ||
         unsigned(field.offset)+unsigned(field.width)*field.count>stride)return false;
    stride_=stride;size_=fields.size();
    for(unsigned i=0;i<size_;++i)fields_[i]=fields[i];
    return true;
  }
  bool Execute(std::span<const std::uint8_t> bytes,unsigned count,std::uint16_t skipped,
               std::span<Bounds> bounds) const noexcept {
    if(!size_ || count>65535 || bounds.size()<size_ || std::uint64_t(count)*stride_!=bytes.size())return false;
    for(unsigned i=0;i<size_;++i) {
      if(skipped&(1u<<i))continue;
      const auto& field=fields_[i];
      // Avoid arithmetic on a null data pointer for a legal empty packet.
      if(!count) {bounds[i]={};continue;}
      const auto* data=bytes.data();
      if(field.width==1) bounds[i]=field.count==1 ? Scan<1,1>(data,count,field.offset) : Scan<1,3>(data,count,field.offset);
      else bounds[i]=field.count==1 ? Scan<2,1>(data,count,field.offset) : Scan<2,3>(data,count,field.offset);
    }
    return true;
  }
private:
  template<unsigned Width> static std::uint16_t Read(const std::uint8_t* p) noexcept {
    if constexpr(Width==1)return p[0];
    else return (std::uint16_t(p[0])<<8)|p[1];
  }
  template<unsigned Width,unsigned Indices> Bounds Scan(const std::uint8_t* p,unsigned count,unsigned offset) const noexcept {
    Bounds result;
    unsigned v=0;
#if defined(GEKKOAOT_INDEX_BOUNDS_SSE2)
    const auto bias=_mm_set1_epi16(static_cast<short>(0x8000));
    auto low=_mm_set1_epi16(32767),high=bias;
    for(;v+8u<=count;v+=8u,p+=8u*stride_) {
      for(unsigned n=0;n<Indices;++n) {
        const auto* q=p+offset+n*Width;
        const auto values=_mm_set_epi16(
          static_cast<short>(Read<Width>(q+7u*stride_)),static_cast<short>(Read<Width>(q+6u*stride_)),
          static_cast<short>(Read<Width>(q+5u*stride_)),static_cast<short>(Read<Width>(q+4u*stride_)),
          static_cast<short>(Read<Width>(q+3u*stride_)),static_cast<short>(Read<Width>(q+2u*stride_)),
          static_cast<short>(Read<Width>(q+stride_)),static_cast<short>(Read<Width>(q)));
        const auto biased=_mm_xor_si128(values,bias);
        low=_mm_min_epi16(low,biased);high=_mm_max_epi16(high,biased);
      }
    }
    // Only lane zero is used. Every folded source lane represents a real
    // index; zero-filled upper lanes never enter the final reduction.
    low=_mm_min_epi16(low,_mm_srli_si128(low,8));high=_mm_max_epi16(high,_mm_srli_si128(high,8));
    low=_mm_min_epi16(low,_mm_srli_si128(low,4));high=_mm_max_epi16(high,_mm_srli_si128(high,4));
    low=_mm_min_epi16(low,_mm_srli_si128(low,2));high=_mm_max_epi16(high,_mm_srli_si128(high,2));
    result.minimum=std::uint16_t(_mm_cvtsi128_si32(low))^0x8000u;
    result.maximum=std::uint16_t(_mm_cvtsi128_si32(high))^0x8000u;
#elif defined(GEKKOAOT_INDEX_BOUNDS_NEON)
    auto low=vdupq_n_u16(65535),high=vdupq_n_u16(0);
    for(;v+8u<=count;v+=8u,p+=8u*stride_) {
      for(unsigned n=0;n<Indices;++n) {
        const auto* q=p+offset+n*Width;
        const std::uint16_t lanes[]={Read<Width>(q),Read<Width>(q+stride_),Read<Width>(q+2u*stride_),Read<Width>(q+3u*stride_),
                                    Read<Width>(q+4u*stride_),Read<Width>(q+5u*stride_),Read<Width>(q+6u*stride_),Read<Width>(q+7u*stride_)};
        const auto values=vld1q_u16(lanes);low=vminq_u16(low,values);high=vmaxq_u16(high,values);
      }
    }
    std::uint16_t lows[8],highs[8];vst1q_u16(lows,low);vst1q_u16(highs,high);
    for(unsigned lane=0;lane<8;++lane) {
      if(lows[lane]<result.minimum)result.minimum=lows[lane];
      if(highs[lane]>result.maximum)result.maximum=highs[lane];
    }
#endif
    for(;v<count;++v,p+=stride_)for(unsigned n=0;n<Indices;++n) {
      const auto value=Read<Width>(p+offset+n*Width);
      if(value<result.minimum)result.minimum=value;
      if(value>result.maximum)result.maximum=value;
    }
    return result;
  }
  std::array<Field,MaxFields> fields_{};
  std::uint16_t stride_=0;
  std::uint8_t size_=0;
};
} // namespace GekkoAOT::GX
