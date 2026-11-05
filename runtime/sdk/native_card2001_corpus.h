#pragma once
// SPDX-License-Identifier: GPL-3.0-or-later
// Legacy 0x108 CARDControl SDK ABI. API identities validated against the
// public dolsdk2001 CARD sources and adjacent public/synchronous callers.
// Relocation-free fingerprints only; no game addresses or library bytes.
// Native storage admission additionally requires ProveLegacyCardControl.
#include "sdk/debug_corpus_signatures_v33.h"
#include <array>
namespace GekkoAOTSdk {
inline constexpr std::array<DebugCorpusSignatureV33,12> kNativeCard2001Corpus = {{
  DebugCorpusSignatureV33{"CARDInit", 144u, 0xd2cc0c2d068ad56bull, 0x436ac21eb71cd62bull, 0x739936d71a247ce0ull, 6u, 9u, 1u, 24u},
  DebugCorpusSignatureV33{"__CARDSetDiskID", 28u, 0xa918994611043447ull, 0xcef58fd7a4640b17ull, 0xd398227e70c5e1aaull, 0u, 3u, 1u, 0u},
  DebugCorpusSignatureV33{"CARDProbeEx", 356u, 0xbe110d9fac58cb34ull, 0x188efd5ebc69f53eull, 0xbb707da0c06cd597ull, 5u, 28u, 1u, 48u},
  DebugCorpusSignatureV33{"CARDMountAsync", 392u, 0x5fa1788208623cd8ull, 0x72a92e46aba5bfe0ull, 0xbb707da0c06cd597ull, 11u, 27u, 1u, 48u},
  DebugCorpusSignatureV33{"CARDMount", 72u, 0x7b865c8925cec9b7ull, 0x47cf252d901cc6f3ull, 0xa50993d5d11bf0ccull, 2u, 5u, 1u, 32u},
  DebugCorpusSignatureV33{"CARDUnmount", 172u, 0x502044c745eab7c2ull, 0xbf428ec272960077ull, 0x641da951169743adull, 6u, 10u, 1u, 32u},
  DebugCorpusSignatureV33{"CARDOpen", 372u, 0x100936a9c4f3b12cull, 0x1d79d71d8a435263ull, 0xe3bb8852d1b6cd57ull, 7u, 26u, 1u, 56u},
  DebugCorpusSignatureV33{"CARDCreateAsync", 536u, 0x39e61b17885589d9ull, 0x0c95df17bafdeb1aull, 0x568e6ffd8309328bull, 13u, 38u, 1u, 72u},
  DebugCorpusSignatureV33{"CARDReadAsync", 324u, 0x131ccaf45d9401d1ull, 0x309edd3fe4f718dbull, 0xf5b9ae3eb8c51ac1ull, 8u, 21u, 1u, 56u},
  DebugCorpusSignatureV33{"CARDWriteAsync", 272u, 0x54c75e578f729d60ull, 0x76a5aa6aeceecb74ull, 0xf96a0b0ad700e265ull, 8u, 19u, 1u, 56u},
  DebugCorpusSignatureV33{"CARDGetStatus", 296u, 0x0a7e1b09bb7f33ffull, 0x9cd442cfa6b18aabull, 0x12651f43163fc4abull, 9u, 17u, 1u, 40u},
  DebugCorpusSignatureV33{"CARDSetStatusAsync", 368u, 0x077cecc95694bc96ull, 0x0bebd6be59a6739dull, 0xbb707da0c06cd597ull, 9u, 23u, 1u, 48u},
}};
}
