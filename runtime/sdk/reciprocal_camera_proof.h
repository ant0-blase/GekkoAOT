// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <optional>
namespace GekkoAOTSdk {
struct ReciprocalFrustumProvider {
  std::uint32_t consumer, projection_builder, projection_sdk, planes;
  std::int16_t camera_sda;
};
// Prove the entire six-plane consumer, its projection builder and the SDK
// submission leaf. Bounds stay live on the consumer's stack until traversal.
// This binds a data repair to an actual submission, including cached AOT code.
template<class Read>
inline std::optional<ReciprocalFrustumProvider> ProveReciprocalCameraFrustum(std::uint32_t pc,Read read) noexcept {
  const auto hash=[&](std::uint32_t address,unsigned size,std::uint64_t expected){
    const auto* bytes=read(address,size);if(!bytes)return false;
    std::uint64_t h=0xcbf29ce484222325ull;
    for(unsigned i=0;i<size;++i)h=(h^bytes[i])*0x100000001b3ull;
    return h==expected;
  };
  const auto* code=read(pc,840);
  if(!code || code[0]!=0x94 || code[1]!=0x21 || code[2]!=0xff || code[3]!=0xa8 ||
     !hash(pc,840,0xe8ef09a2b1f9033aull))return {};
  const auto word=[](const auto* p){return(std::uint32_t(p[0])<<24)|(std::uint32_t(p[1])<<16)|(std::uint32_t(p[2])<<8)|p[3];};
  const auto call=[&](std::uint32_t at){const auto* p=read(at,4);if(!p)return std::uint32_t(0);
    const auto w=word(p);if((w&0xfc000003u)!=0x48000001u)return std::uint32_t(0);
    return at+static_cast<std::int32_t>((w&0x03fffffcu)^0x02000000u)-0x02000000u;};
  const auto builder=call(pc+0x2c4);
  if(!hash(builder,1368,0xf1df35923fa09d1eull))return {};
  const auto sdk=call(builder+0x3c8);
  if(!hash(sdk,164,0xb17b497f59ef76fbull))return {};
  const auto store=word(code+0x4c);
  if((store&0xffff0000u)!=0x93ed0000u)return {};
  // lis r11 / addi r30 in the proven perspective-plane branch.
  const auto planes=(word(code+0x1b8)<<16)+std::int16_t(word(code+0x1bc));
  return ReciprocalFrustumProvider{pc,builder,sdk,planes,static_cast<std::int16_t>(store)};
}
}
