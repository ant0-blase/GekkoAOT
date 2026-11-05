#pragma once
// Structural fingerprints of SDK and runtime-library functions: size, two
// 64-bit hashes of the normalized body, a 64-bit hash of its first three
// normalized words (StructuralPrefixHash) and call/branch/return/frame
// counters. No instruction word, code byte or address is stored. Fingerprints
// shared by two different names are excluded.
#include <array>
#include <cstdint>
namespace GekkoAOTSdk {
struct DebugCorpusSignatureV33 { const char* name; std::uint32_t size; std::uint64_t h1; std::uint64_t h2; std::uint64_t prefix_hash; std::uint16_t direct_calls; std::uint16_t branches; std::uint16_t returns; std::uint16_t stack_frame; };
inline constexpr std::array<DebugCorpusSignatureV33, 32> kDebugCorpusSignaturesV33 = {
  DebugCorpusSignatureV33{"DCFlushRangeNoSync", 44u, 0xaef9a2c20e2f9760ull, 0x045791c34d6be9f8ull, 0xc27d559c992b12dbull, 0u, 3u, 1u, 0u},
  DebugCorpusSignatureV33{"DCFlushRangeNoSync", 48u, 0x3b22fa00afe33e47ull, 0xb7a78dd990f4a962ull, 0x23309b6504e780c1ull, 0u, 4u, 1u, 0u},
  DebugCorpusSignatureV33{"DCStoreRangeNoSync", 44u, 0xada4a58f1f9ebba0ull, 0xf115cba76da964e7ull, 0xc27d559c992b12dbull, 0u, 3u, 1u, 0u},
  DebugCorpusSignatureV33{"DCStoreRangeNoSync", 48u, 0x39cdf09065526287ull, 0x451e43a122cf8cb3ull, 0x23309b6504e780c1ull, 0u, 4u, 1u, 0u},
  DebugCorpusSignatureV33{"DCTouchRange", 44u, 0xa1a7b8657c8701e0ull, 0x23b302464024a0d6ull, 0xc27d559c992b12dbull, 0u, 3u, 1u, 0u},
  DebugCorpusSignatureV33{"DCTouchRange", 48u, 0x4d20d8210af264c7ull, 0x4cd2c723252e7b20ull, 0x23309b6504e780c1ull, 0u, 4u, 1u, 0u},
  DebugCorpusSignatureV33{"DCZeroRange", 44u, 0xc0b99014367e6a20ull, 0xa818762bc65fc73dull, 0xc27d559c992b12dbull, 0u, 3u, 1u, 0u},
  DebugCorpusSignatureV33{"DCZeroRange", 48u, 0x2c7c10f48fa9cd07ull, 0x30d9ddd9cb6765e9ull, 0x23309b6504e780c1ull, 0u, 4u, 1u, 0u},
  DebugCorpusSignatureV33{"ICInvalidateRange", 52u, 0xebda63793f69c348ull, 0xd75484ce70b08bd3ull, 0xc27d559c992b12dbull, 0u, 4u, 1u, 0u},
  DebugCorpusSignatureV33{"ICInvalidateRange", 56u, 0x2a72f2561d7bde47ull, 0xd551a2072452e0e5ull, 0x23309b6504e780c1ull, 0u, 5u, 1u, 0u},
  DebugCorpusSignatureV33{"strcspn", 208u, 0xc316f5c157c6bceeull, 0x59bbb012ca4b0682ull, 0x961991f0319d5526ull, 0u, 6u, 1u, 48u},
  DebugCorpusSignatureV33{"strcspn", 112u, 0x4d8aacecaba7de8full, 0xf9bc9e087417bec1ull, 0x8af996c805d55972ull, 0u, 8u, 1u, 0u},
  DebugCorpusSignatureV33{"strcspn", 208u, 0xab442f8d8977d686ull, 0x24d306fe6447f278ull, 0x961991f0319d5526ull, 0u, 6u, 1u, 48u},
  DebugCorpusSignatureV33{"strncat", 76u, 0x97b9cbf68368ca17ull, 0x52b4cdefa52e7238ull, 0xfa4dabd9dc8af7a1ull, 0u, 6u, 1u, 0u},
  DebugCorpusSignatureV33{"strncat", 172u, 0x3a2e95ee93a05d28ull, 0x92b290a5ef386626ull, 0x47a1247a949e1694ull, 0u, 10u, 1u, 0u},
  DebugCorpusSignatureV33{"strncat", 100u, 0x6ae95d3bdad79fe4ull, 0xb44800e77a695703ull, 0x453588c8fdf74e5cull, 0u, 6u, 1u, 16u},
  DebugCorpusSignatureV33{"strnlen", 56u, 0x49078eb6245da72cull, 0xdc38782ace8b9638ull, 0x1970dac965e1ec69ull, 0u, 5u, 2u, 0u},
  DebugCorpusSignatureV33{"strnlen", 68u, 0xfbb90f797254508aull, 0x5265d98d8930a4e9ull, 0xa8638aa4a799c18aull, 0u, 5u, 1u, 24u},
  DebugCorpusSignatureV33{"strnlen", 64u, 0x4c4207310fb1786cull, 0xd627aad2db8ac9bbull, 0x72ae870c91486313ull, 0u, 5u, 1u, 0u},
  DebugCorpusSignatureV33{"strpbrk", 212u, 0xb4f576bdc9f82838ull, 0xef9431c9fc7eb024ull, 0x961991f0319d5526ull, 0u, 7u, 1u, 48u},
  DebugCorpusSignatureV33{"strpbrk", 128u, 0x6554d09d67545837ull, 0x5785882dbceafc0full, 0x635945238a634523ull, 0u, 10u, 2u, 0u},
  DebugCorpusSignatureV33{"strpbrk", 212u, 0xedd1894a47bcf840ull, 0x8cf31af782f84c25ull, 0x961991f0319d5526ull, 0u, 7u, 1u, 48u},
  DebugCorpusSignatureV33{"strstr", 104u, 0xaa3f92363125846full, 0x9d47aa489557afb7ull, 0x871ce63695291358ull, 0u, 10u, 3u, 0u},
  DebugCorpusSignatureV33{"strstr", 104u, 0xa2d358fa2e98d67full, 0x1bda4cf248397449ull, 0x39e167ad39ab0ba6ull, 0u, 8u, 2u, 0u},
  DebugCorpusSignatureV33{"strstr", 108u, 0x165138a08a15cd31ull, 0xa494a6ae521a6512ull, 0x4160d90889e6b9d6ull, 0u, 10u, 3u, 0u},
  DebugCorpusSignatureV33{"strstr", 96u, 0x914e6bfb6cd9c3d8ull, 0x244aeef9fcf7be80ull, 0x9cf8834adf4a1cc3ull, 0u, 7u, 1u, 0u},
  DebugCorpusSignatureV33{"strstr", 124u, 0x446f0146f605f939ull, 0xaa25fa09b7aa85edull, 0xda688cdce986becaull, 0u, 10u, 1u, 48u},
  DebugCorpusSignatureV33{"strstr", 108u, 0x38b948426bdeae0eull, 0x5ae595959fcdd04bull, 0x39e167ad39ab0ba6ull, 0u, 8u, 3u, 0u},
  // Common retail SDK matrix upload leaves (64-76 independent definitions
  // per variant were fingerprinted).
  DebugCorpusSignatureV33{"GXLoadPosMtxImm", 80u, 0x1c417c1ebc97f3dcull, 0x192ed99e943ec46full, 0x6c1345ff7920e65dull, 0u, 1u, 1u, 0u},
  DebugCorpusSignatureV33{"GXLoadPosMtxImm", 60u, 0x4707e9cc846d588dull, 0x59364d5b4301273cull, 0xca9275b8a4242f54ull, 1u, 2u, 1u, 8u},
  DebugCorpusSignatureV33{"GXLoadTexMtxImm", 180u, 0x72b61e4d929adabdull, 0x8fcaee17262ce1e9ull, 0x8a98c2f3b53b2a6dull, 0u, 7u, 2u, 0u},
  DebugCorpusSignatureV33{"GXLoadTexMtxImm", 132u, 0xfb9bf52dda4a248bull, 0xc287cb1237365c55ull, 0xca9275b8a4242f54ull, 2u, 9u, 1u, 8u},
};
} // namespace GekkoAOTSdk
