#pragma once
// SPDX-License-Identifier: GPL-3.0-or-later
// Retail SDK 2001 PAD variants, identified against the public SDK Pad.c.
// Relocation-free instruction-class fingerprints, never game addresses/code.
// Admission additionally requires independently identified SI/OS/PAD callees.
#include "sdk/debug_corpus_signatures_v33.h"
#include <array>
namespace GekkoAOTSdk {
inline constexpr std::array<DebugCorpusSignatureV33,6> kNativePad2001Corpus = {{
  DebugCorpusSignatureV33{"PADReset", 364u, 0x06ae0ffa9ef2a59dull, 0x409abcabcfa23a08ull, 0x4f377926a35db2feull, 5u, 11u, 1u, 40u},
  DebugCorpusSignatureV33{"PADInit", 452u, 0x30d520f744a8c94dull, 0xfaf6026724aa9542ull, 0x7e35bb7ae22957cbull, 11u, 17u, 1u, 56u},
  DebugCorpusSignatureV33{"PADRead", 824u, 0xf0aa321b48d23f17ull, 0x26bab4f3b7eeb927ull, 0x4f377926a35db2feull, 20u, 46u, 1u, 72u},
  DebugCorpusSignatureV33{"PADControlMotor", 188u, 0x519180753c9428aeull, 0xc2144721c8fcd1a3ull, 0x641da951169743adull, 4u, 10u, 1u, 32u},
  DebugCorpusSignatureV33{"PADRecalibrate", 340u, 0xcc6aae49431697e1ull, 0x9b8f0dac3b1fff9dull, 0x4f377926a35db2feull, 5u, 10u, 1u, 40u},
  DebugCorpusSignatureV33{"PADSetAnalogMode", 120u, 0xb48e4e881362296cull, 0x7c28c3d5b9596e57ull, 0x739936d71a247ce0ull, 3u, 4u, 1u, 24u},
}};
}
