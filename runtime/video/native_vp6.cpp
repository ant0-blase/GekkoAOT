// SPDX-License-Identifier: GPL-3.0-or-later
#include "video/native_vp6.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <limits>
#include <utility>

#if defined(GEKKOAOT_HAVE_FFMPEG_VP6)
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/pixdesc.h>
}
#endif

namespace GekkoAOT::Video {
namespace {

#if defined(GEKKOAOT_HAVE_FFMPEG_VP6)
std::uint8_t Clamp8(int value) {
  return static_cast<std::uint8_t>(std::clamp(value, 0, 255));
}

std::string AvError(int error) {
  char text[AV_ERROR_MAX_STRING_SIZE]{};
  av_strerror(error, text, sizeof(text));
  return text;
}

bool ConvertFrameToRgba(const AVFrame* source, NativeVP6Frame* output,
                        std::string* error) {
  if (!source || !output || source->width <= 0 || source->height <= 0) {
    if (error) *error = "invalid decoded VP6 frame";
    return false;
  }
  const AVPixFmtDescriptor* desc =
      av_pix_fmt_desc_get(static_cast<AVPixelFormat>(source->format));
  if (!desc || (desc->flags & AV_PIX_FMT_FLAG_RGB) || desc->nb_components < 3 ||
      !source->data[0] || !source->data[1] || !source->data[2]) {
    if (error) *error = "unsupported VP6 pixel format";
    return false;
  }

  const auto width = static_cast<std::uint32_t>(source->width);
  const auto height = static_cast<std::uint32_t>(source->height);
  if (width > 8192u || height > 8192u ||
      static_cast<std::uint64_t>(width) * height > (1ull << 26)) {
    if (error) *error = "VP6 frame dimensions are unreasonable";
    return false;
  }

  output->width = width;
  output->height = height;
  output->rgba.resize(static_cast<std::size_t>(width) * height * 4u);

  const int hsub = desc->log2_chroma_w;
  const int vsub = desc->log2_chroma_h;
  const bool full_range = source->color_range == AVCOL_RANGE_JPEG;
  for (std::uint32_t y = 0; y < height; ++y) {
    const auto* yrow = source->data[0] + static_cast<std::ptrdiff_t>(y) * source->linesize[0];
    const auto* urow = source->data[1] +
                       static_cast<std::ptrdiff_t>(y >> vsub) * source->linesize[1];
    const auto* vrow = source->data[2] +
                       static_cast<std::ptrdiff_t>(y >> vsub) * source->linesize[2];
    auto* dst = output->rgba.data() + static_cast<std::size_t>(y) * width * 4u;
    for (std::uint32_t x = 0; x < width; ++x) {
      const int yy = yrow[x];
      const int uu = static_cast<int>(urow[x >> hsub]) - 128;
      const int vv = static_cast<int>(vrow[x >> hsub]) - 128;
      int r, g, b;
      if (full_range) {
        r = yy + ((359 * vv) >> 8);
        g = yy - ((88 * uu + 183 * vv) >> 8);
        b = yy + ((454 * uu) >> 8);
      } else {
        const int c = std::max(yy - 16, 0);
        r = (298 * c + 409 * vv + 128) >> 8;
        g = (298 * c - 100 * uu - 208 * vv + 128) >> 8;
        b = (298 * c + 516 * uu + 128) >> 8;
      }
      dst[x * 4u + 0u] = Clamp8(r);
      dst[x * 4u + 1u] = Clamp8(g);
      dst[x * 4u + 2u] = Clamp8(b);
      dst[x * 4u + 3u] = 0xffu;
    }
  }
  return true;
}
#endif

} // namespace

struct NativeVP6Decoder::Impl {
#if defined(GEKKOAOT_HAVE_FFMPEG_VP6)
  AVCodecContext* context = nullptr;
  AVFrame* frame = nullptr;
  AVPacket* packet = nullptr;
#endif
};

NativeVP6Decoder::NativeVP6Decoder() = default;
NativeVP6Decoder::~NativeVP6Decoder() { Close(); }
NativeVP6Decoder::NativeVP6Decoder(NativeVP6Decoder&&) noexcept = default;
NativeVP6Decoder& NativeVP6Decoder::operator=(NativeVP6Decoder&&) noexcept = default;

bool NativeVP6Decoder::BackendAvailable() {
#if defined(GEKKOAOT_HAVE_FFMPEG_VP6)
  return avcodec_find_decoder(AV_CODEC_ID_VP6) != nullptr ||
         avcodec_find_decoder(AV_CODEC_ID_VP6F) != nullptr;
#else
  return false;
#endif
}

bool NativeVP6Decoder::Open(VP6Variant variant) {
  Close();
  last_error_.clear();
#if defined(GEKKOAOT_HAVE_FFMPEG_VP6)
  const AVCodecID id = variant == VP6Variant::VP6F ? AV_CODEC_ID_VP6F : AV_CODEC_ID_VP6;
  const AVCodec* codec = avcodec_find_decoder(id);
  if (!codec) {
    last_error_ = "FFmpeg VP6 decoder is unavailable";
    return false;
  }
  auto impl = std::make_unique<Impl>();
  impl->context = avcodec_alloc_context3(codec);
  impl->frame = av_frame_alloc();
  impl->packet = av_packet_alloc();
  if (!impl->context || !impl->frame || !impl->packet) {
    last_error_ = "cannot allocate FFmpeg VP6 decoder state";
    if (impl->packet) av_packet_free(&impl->packet);
    if (impl->frame) av_frame_free(&impl->frame);
    if (impl->context) avcodec_free_context(&impl->context);
    return false;
  }
  const int rc = avcodec_open2(impl->context, codec, nullptr);
  if (rc < 0) {
    last_error_ = "avcodec_open2(VP6): " + AvError(rc);
    av_packet_free(&impl->packet);
    av_frame_free(&impl->frame);
    avcodec_free_context(&impl->context);
    return false;
  }
  impl_ = std::move(impl);
  return true;
#else
  (void)variant;
  last_error_ = "GekkoAOT was built without libavcodec VP6 support";
  return false;
#endif
}

void NativeVP6Decoder::Flush() {
  last_error_.clear();
#if defined(GEKKOAOT_HAVE_FFMPEG_VP6)
  if (impl_ && impl_->context) avcodec_flush_buffers(impl_->context);
#endif
}

void NativeVP6Decoder::Close() {
#if defined(GEKKOAOT_HAVE_FFMPEG_VP6)
  if (impl_) {
    if (impl_->packet) av_packet_free(&impl_->packet);
    if (impl_->frame) av_frame_free(&impl_->frame);
    if (impl_->context) avcodec_free_context(&impl_->context);
  }
#endif
  impl_.reset();
}

bool NativeVP6Decoder::Decode(std::span<const std::uint8_t> bytes,
                              NativeVP6Frame* output) {
  last_error_.clear();
  if (!output) {
    last_error_ = "null VP6 output frame";
    return false;
  }
  output->width = output->height = 0;
  output->rgba.clear();
#if defined(GEKKOAOT_HAVE_FFMPEG_VP6)
  if (!impl_ || !impl_->context || !impl_->frame || !impl_->packet) {
    last_error_ = "VP6 decoder is not open";
    return false;
  }
  if (bytes.empty()) {
    last_error_ = "empty VP6 packet";
    return false;
  }
  if (bytes.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    last_error_ = "VP6 packet is too large";
    return false;
  }

  av_packet_unref(impl_->packet);
  const int alloc = av_new_packet(impl_->packet, static_cast<int>(bytes.size()));
  if (alloc < 0) {
    last_error_ = "av_new_packet(VP6): " + AvError(alloc);
    return false;
  }
  std::memcpy(impl_->packet->data, bytes.data(), bytes.size());
  int rc = avcodec_send_packet(impl_->context, impl_->packet);
  if (rc < 0) {
    last_error_ = "avcodec_send_packet(VP6): " + AvError(rc);
    return false;
  }
  rc = avcodec_receive_frame(impl_->context, impl_->frame);
  if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF)
    return false;
  if (rc < 0) {
    last_error_ = "avcodec_receive_frame(VP6): " + AvError(rc);
    return false;
  }
  return ConvertFrameToRgba(impl_->frame, output, &last_error_);
#else
  (void)bytes;
  last_error_ = "GekkoAOT was built without libavcodec VP6 support";
  return false;
#endif
}

} // namespace GekkoAOT::Video
