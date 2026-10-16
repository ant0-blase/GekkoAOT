// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <cmath>
#include <array>
#include <cstdint>
#include <span>

namespace GekkoAOT::GX {
// Infer an already-wide camera from GX's projection coefficients. Normalize
// split-screen views to the full retail canvas before comparing aspects.
inline float ProjectionAspect(float horizontal, float vertical,
                              float view_width, float view_height) noexcept {
  constexpr float original = 4.f / 3.f;
  if (!std::isfinite(horizontal) || !std::isfinite(vertical) ||
      horizontal == 0.f || view_width <= 0.f || view_height <= 0.f)
    return original;
  const float aspect = std::abs(vertical / horizontal) *
                       original / (view_width / view_height);
  // Keep invalid/skewed matrices on the original policy. Otherwise a game
  // with its own 16:10, 16:9 or ultrawide camera must not be widened twice.
  return std::isfinite(aspect) && aspect >= original * .975f && aspect <= 3.5f
             ? std::max(aspect, original) : original;
}

inline float PerspectiveXScale(float horizontal, float vertical,
                               float view_width, float view_height,
                               float host_aspect) noexcept {
  return std::isfinite(host_aspect) && host_aspect > 0.f
             ? ProjectionAspect(horizontal, vertical, view_width, view_height) / host_aspect
             : 1.f;
}

inline float UiXScale(float host_aspect) noexcept {
  return std::isfinite(host_aspect) && host_aspect > 0.f
             ? (4.f / 3.f) / host_aspect : 1.f;
}

// Orthographic geometry is scaled about its viewport centre. Its scissor must
// undergo exactly the same transform, or inset HUD screens lose one side.
inline float UiClipX(float x, float view_left, float view_width, float scale) noexcept {
  if (!std::isfinite(x) || !std::isfinite(view_left) ||
      !std::isfinite(view_width) || view_width <= 0.f ||
      !std::isfinite(scale) || scale <= 0.f) return x;
  const float centre = view_left + view_width * .5f;
  return centre + (x - centre) * scale;
}

inline bool IsScreenCopy(int copy_width, int copy_height,
                         float view_width, float view_height) noexcept {
  return view_width > 0.f && view_height > 0.f &&
         copy_width >= view_width * .95f &&
         copy_height >= view_height * .95f;
}

// A complete rectangular screen fill differs from an individual HUD sprite.
// Require all four distinct clip-space corners in primitive order; a bounding
// box alone would also accept a diamond or a self-intersecting quad.
inline bool IsScreenFillQuad(const std::array<std::array<float,2>,4>& corners,
                             bool strip) noexcept {
  std::array<unsigned,4> ids{};
  unsigned mask=0;
  for (unsigned i=0;i<4;++i) {
    const float x=corners[i][0],y=corners[i][1];
    if (!std::isfinite(x)||!std::isfinite(y)||
        std::abs(std::abs(x)-1.f)>.025f||std::abs(std::abs(y)-1.f)>.025f) return false;
    // Clockwise corner numbering, independent of axis direction.
    ids[i]= y<0.f ? (x<0.f ? 0u:1u) : (x<0.f ? 3u:2u);
    mask|=1u<<ids[i];
  }
  if (mask!=15u) return false;
  if (strip) return ((ids[1]+2u)&3u)==ids[2];
  const unsigned step=(ids[1]+4u-ids[0])&3u;
  if (step!=1u&&step!=3u) return false;
  for (unsigned i=2;i<4;++i)
    if (((ids[i-1]+step)&3u)!=ids[i]) return false;
  return true;
}

// v343: orthographic geometry whose every vertex lies on (or beyond) the left
// or right clip edge spans the full GameCube width. Callers admit it only when
// it samples no texture or a single-colour one (letterbox bars, fades,
// flashes): it has no aspect to preserve and extends to the host width.
// Textured art (menu borders, title screens, banners) keeps the original aspect.
inline bool IsEdgeAnchoredSpan(std::span<const std::array<float,2>> vertices) noexcept {
  bool left=false,right=false;
  float low=2.f,high=-2.f;
  for (const auto& v : vertices) {
    const float x=v[0],y=v[1],ax=std::abs(x);
    if (!std::isfinite(x)||!std::isfinite(y)||ax<.975f||ax>2.f) return false;
    (x<0.f ? left : right)=true;
    low=std::min(low,y); high=std::max(high,y);
  }
  return left&&right&&high>=-1.025f&&low<=1.025f;
}

// v353: an edge-anchored span that also covers the full height is a whole-
// screen composite (a bloom/blur result added back over the frame).
inline bool IsFullScreenSpan(std::span<const std::array<float,2>> vertices) noexcept {
  if (!IsEdgeAnchoredSpan(vertices)) return false;
  float low=2.f, high=-2.f;
  for (const auto& v : vertices) { low=std::min(low,v[1]); high=std::max(high,v[1]); }
  return low<=-.975f && high>=.975f;
}

// v353: CMPR blocks hold two RGB565 endpoints and 2-bit texel indices. A flat
// colour may still use differing index bits (the indices are irrelevant when
// the endpoints match), so compare decoded texels rather than raw bytes.
inline bool IsUniformCmpr(const std::uint8_t* bytes, std::size_t size) noexcept {
  const auto rgba = [](unsigned c) -> std::uint32_t {
    const unsigned r=(c>>11)&31u, g=(c>>5)&63u, b=c&31u;
    return ((r<<3|r>>2)<<24) | ((g<<2|g>>4)<<16) | ((b<<3|b>>2)<<8) | 0xffu;
  };
  const auto mix = [](std::uint32_t a, std::uint32_t b, unsigned wa, unsigned wb) -> std::uint32_t {
    std::uint32_t out = 0xffu;
    for (unsigned shift : {24u, 16u, 8u})
      out |= (((((a>>shift)&255u)*wa + ((b>>shift)&255u)*wb) / (wa+wb)) & 255u) << shift;
    return out;
  };
  std::uint32_t first = 0;
  bool have = false;
  for (std::size_t off = 0; off + 8u <= size; off += 8u) {
    const unsigned c0 = (unsigned(bytes[off])<<8)|bytes[off+1], c1 = (unsigned(bytes[off+2])<<8)|bytes[off+3];
    std::uint32_t palette[4] = {rgba(c0), rgba(c1), 0u, 0u};
    if (c0 > c1) {
      palette[2] = mix(palette[0], palette[1], 2u, 1u);
      palette[3] = mix(palette[0], palette[1], 1u, 2u);
    } else {
      palette[2] = mix(palette[0], palette[1], 1u, 1u);
      palette[3] = 0u; // transparent black
    }
    for (unsigned i = 0; i < 16u; ++i) {
      const std::uint32_t texel = palette[(bytes[off + 4u + i/4u] >> (6u - 2u*(i%4u))) & 3u];
      if (!have) { first = texel; have = true; }
      else if (texel != first) return false;
    }
  }
  return have;
}

// v353: every texel fully opaque (alpha independent of the texture content).
// Formats without an alpha channel are opaque; CMPR is opaque unless a block
// with c0 <= c1 selects its transparent index 3; RGB5A3 texels with the top
// bit set are opaque. Other formats are not proven.
inline bool IsOpaqueTexture(const std::uint8_t* bytes, std::size_t size, unsigned gx_format) noexcept {
  if (gx_format == 0u || gx_format == 1u || gx_format == 4u) return true; // I4, I8, RGB565
  if (!bytes) return false;
  if (gx_format == 5u) {
    for (std::size_t i = 0; i + 1u < size; i += 2u) if ((bytes[i] & 0x80u) == 0u) return false;
    return size >= 2u;
  }
  if (gx_format != 14u) return false;
  for (std::size_t off = 0; off + 8u <= size; off += 8u) {
    const unsigned c0 = (unsigned(bytes[off])<<8)|bytes[off+1], c1 = (unsigned(bytes[off+2])<<8)|bytes[off+3];
    if (c0 > c1) continue;
    for (unsigned i = 0; i < 4u; ++i) {
      const unsigned row = bytes[off + 4u + i];
      for (unsigned t = 0; t < 4u; ++t) if (((row >> (2u*t)) & 3u) == 3u) return false;
    }
  }
  return size >= 8u;
}

// A texture whose bytes repeat with its texel (or RGBA8 block) period has one
// colour everywhere, so stretching it cannot distort anything.
inline bool IsUniformTexture(const std::uint8_t* bytes, std::size_t size,
                             unsigned gx_format) noexcept {
  if (gx_format == 14u) return bytes && IsUniformCmpr(bytes, size);
  const std::size_t period = gx_format==6u ? 64u : gx_format==14u ? 8u :
      (gx_format==3u||gx_format==4u||gx_format==5u||gx_format==10u) ? 2u : 1u;
  if (!bytes || size<period) return false;
  for (std::size_t i=period;i<size;++i)
    if (bytes[i]!=bytes[i-period]) return false;
  return true;
}

// v348: a viewport or scissor span that is narrow and touches neither
// horizontal canvas edge frames HUD content (a live 3D portrait, a minimap,
// a mirror). It must follow the UI transform like the HUD drawn around it.
// Split-screen views always reach a canvas edge.
inline bool IsInsetSpan(float left, float width, float canvas) noexcept {
  if (!std::isfinite(left) || !std::isfinite(width) || !std::isfinite(canvas) ||
      canvas <= 0.f || width <= 0.f) return false;
  return width <= canvas*.6f && left >= canvas*.02f && left+width <= canvas*.98f;
}

// Render-to-texture passes also draw into small viewports, then GXCopyTex
// their region. A span is a displayed inset only after a frame in which it
// was drawn and none of its region was copied out before the display copy.
// The confirmation covers the next frames, so a one-off texture producer is
// never moved.
class DisplayInsets {
  struct Span { float left=0, width=0; std::uint64_t seen=0, confirmed=0; bool copied=false, valid=false; };
  std::array<Span, 32> spans_{};
  std::size_t next_=0;
  static bool Same(const Span& s, float left, float width) noexcept {
    return s.valid && std::abs(s.left-left)<=.5f && std::abs(s.width-width)<=.5f;
  }
public:
  void Reset() noexcept { spans_={}; next_=0; }
  void NoteDraw(float left, float width, std::uint64_t epoch) noexcept {
    for (auto& s: spans_) {
      if (!Same(s,left,width)) continue;
      if (s.seen!=epoch) { s.seen=epoch; s.copied=false; }
      return;
    }
    spans_[next_]={left,width,epoch,0,false,true};
    next_=(next_+1)%spans_.size();
  }
  // A texture copy taken from inside a span drawn this frame makes it a
  // producer. A larger copy (a full-screen post-process grab) does not.
  void NoteCopy(float left, float width, std::uint64_t epoch) noexcept {
    for (auto& s: spans_)
      if (s.valid && s.seen==epoch && width>0.f &&
          left>=s.left-4.f && left+width<=s.left+s.width+4.f) s.copied=true;
  }
  // Display copy: every span drawn during the frame and never copied out was shown.
  void EndFrame(std::uint64_t epoch) noexcept {
    for (auto& s: spans_)
      if (s.valid && s.seen==epoch) s.confirmed = s.copied ? 0u : epoch+1u;
  }
  bool IsInset(float left, float width, std::uint64_t epoch) const noexcept {
    for (const auto& s: spans_)
      if (Same(s,left,width) && s.confirmed!=0u && epoch>=s.confirmed && epoch-s.confirmed<=2u)
        return true;
    return false;
  }
};

// v354: provenance of EFB copies. A copy is screen-derived when it covers the
// whole frame, or when its source rectangle was drawn this frame by passes
// that only sampled screen-derived copies (bloom/blur pyramids: Shadow the
// Hedgehog, Lemony Snicket). Such passes work in EFB pixel space and must keep
// the retail width; a copy of a 3D render used as a HUD sprite (Luigi's
// Game Boy Horror portrait) is not screen-derived and stays UI-scaled.
class CopyProvenance {
  struct Rect { float x0=0, y0=0, x1=0, y1=0; };
  struct Copy { const void* dest=nullptr; bool derived=false; };
  std::array<Rect, 32> rects_{};
  std::size_t rect_count_=0;
  std::array<Copy, 64> copies_{};
  std::size_t next_copy_=0;
public:
  void Reset() noexcept { rect_count_=0; copies_={}; next_copy_=0; }
  void BeginFrame() noexcept { rect_count_=0; }
  void NoteScreenDraw(float x0, float y0, float x1, float y1) noexcept {
    if (rect_count_ < rects_.size() && x1 > x0 && y1 > y0) rects_[rect_count_++] = {x0, y0, x1, y1};
  }
  bool NoteCopy(const void* dest, float x, float y, float w, float h,
                float canvas_w, float canvas_h) noexcept {
    if (!dest) return false;
    bool derived = canvas_w > 0.f && canvas_h > 0.f && w >= canvas_w*.95f && h >= canvas_h*.95f;
    for (std::size_t i = 0; !derived && i < rect_count_; ++i) {
      const auto& r = rects_[i];
      const float ix = std::min(x+w, r.x1) - std::max(x, r.x0);
      const float iy = std::min(y+h, r.y1) - std::max(y, r.y0);
      derived = ix > 0.f && iy > 0.f && ix*iy >= w*h*.9f;
    }
    for (auto& c : copies_) if (c.dest == dest) { c.derived = derived; return derived; }
    copies_[next_copy_] = {dest, derived};
    next_copy_ = (next_copy_ + 1) % copies_.size();
    return derived;
  }
  bool IsScreenDerived(const void* dest) const noexcept {
    for (const auto& c : copies_) if (c.dest == dest) return c.derived;
    return false;
  }
};

inline bool IsScreenFillTriangles(const std::array<std::array<float,2>,6>& vertices) noexcept {
  unsigned masks[2]={};
  for (unsigned i=0;i<6;++i) {
    const float x=vertices[i][0],y=vertices[i][1];
    if (!std::isfinite(x)||!std::isfinite(y)||
        std::abs(std::abs(x)-1.f)>.025f||std::abs(std::abs(y)-1.f)>.025f) return false;
    const unsigned id=y<0.f ? (x<0.f ? 0u:1u) : (x<0.f ? 3u:2u);
    const unsigned bit=1u<<id;
    if (masks[i/3u]&bit) return false;
    masks[i/3u]|=bit;
  }
  // Two triangles must share a diagonal, rather than overlapping along a side.
  const unsigned common=masks[0]&masks[1];
  return (masks[0]|masks[1])==15u && (common==5u||common==10u);
}
// GX renders texture passes into the EFB before copying them. They are not
// Aurora offscreen targets yet, so is_offscreen() alone cannot identify them.
// Remember only observed square texture producers, using their copy rectangle
// as evidence; a square viewport by itself is not sufficient.
class TextureViewportRoles {
  struct Role { float left=0, top=0, width=0, height=0; std::uint64_t epoch=0; bool valid=false; };
  std::array<Role, 8> roles_{};
  std::size_t next_=0;
public:
  void Reset() noexcept { roles_={}; next_=0; }
  void Observe(float left, float top, float width, float height,
               int source_x, int source_y, int source_width, int source_height,
               float display_width, float display_height, std::uint64_t epoch) noexcept {
    if (!std::isfinite(left) || !std::isfinite(top) || !std::isfinite(width) ||
        !std::isfinite(height) || width < 16.f || height < 16.f ||
        std::abs(width-height) > .5f || display_width <= 0 || display_height <= 0 ||
        (width >= display_width*.95f && height >= display_height*.95f)) return;
    // Allow the two-pixel inset used by shadow producers, but not an arbitrary
    // small copy from the main display viewport.
    if (source_x < left-.5f || source_y < top-.5f ||
        std::abs(source_width-width) > 4.f || std::abs(source_height-height) > 4.f ||
        source_x+source_width > left+width+.5f || source_y+source_height > top+height+.5f) return;
    for (auto& r: roles_) {
      if (r.valid && r.left==left && r.top==top && r.width==width && r.height==height) {
        r.epoch=epoch; return;
      }
    }
    roles_[next_]={left,top,width,height,epoch,true}; next_=(next_+1)%roles_.size();
  }
  bool IsProducer(float left, float top, float width, float height,
                  std::uint64_t epoch) const noexcept {
    for (const auto& r: roles_)
      if (r.valid && epoch>=r.epoch && epoch-r.epoch<=2 &&
          r.left==left && r.top==top && r.width==width && r.height==height) return true;
    return false;
  }
};

// v362: menus drawn as flat cards in front of a 3D camera (Pikmin) keep
// off-screen elements just outside the retail frustum; hor+ reveals them in
// the side bands. A run of frames without any depth-writing perspective
// geometry is a 2D screen: its perspective cards get the UI scissor (4:3)
// instead of the widened 3D view. Several frames are required so titles that
// alternate 3D and 2D display copies never qualify.
class FlatFrameHistory {
 public:
  static constexpr unsigned Frames = 4;
  void EndFrame(unsigned depth_writing_draws) noexcept {
    flat_run_ = depth_writing_draws == 0u ? std::min(flat_run_ + 1u, Frames) : 0u;
  }
  bool Flat() const noexcept { return flat_run_ >= Frames; }
  void Reset() noexcept { flat_run_ = 0u; }
 private:
  unsigned flat_run_ = 0u;
};
} // namespace GekkoAOT::GX
