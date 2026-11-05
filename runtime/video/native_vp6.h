#pragma once
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace GekkoAOT::Video {

enum class VP6Variant : std::uint8_t {
  VP6,
  VP6F,
};

struct NativeVP6Frame {
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  // Host RGBA8.  The EA movie bridge can upload this directly or split it into
  // the exact texture layout required by a title-specific movie frontend.
  std::vector<std::uint8_t> rgba;
};

// Host-native VP6 decoder service.  This deliberately does not guess a game's
// movie-library ABI: an EA frontend must provide complete compressed packets.
// Keeping the codec boundary independent lets structural SDK/movie resolvers
// bind different EA revisions to one decoder without baking game IDs into it.
class NativeVP6Decoder {
public:
  NativeVP6Decoder();
  ~NativeVP6Decoder();
  NativeVP6Decoder(NativeVP6Decoder&&) noexcept;
  NativeVP6Decoder& operator=(NativeVP6Decoder&&) noexcept;
  NativeVP6Decoder(const NativeVP6Decoder&) = delete;
  NativeVP6Decoder& operator=(const NativeVP6Decoder&) = delete;

  static bool BackendAvailable();
  bool Open(VP6Variant variant = VP6Variant::VP6);
  void Flush();
  void Close();

  // Returns true only when this packet produced a complete decoded frame.
  // Some streams legitimately buffer a packet; callers may submit the next
  // packet and continue.  LastError() is empty for that non-error condition.
  bool Decode(std::span<const std::uint8_t> packet, NativeVP6Frame* frame);
  const std::string& LastError() const { return last_error_; }

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  std::string last_error_;
};

} // namespace GekkoAOT::Video
