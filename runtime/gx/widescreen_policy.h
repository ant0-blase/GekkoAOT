// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cmath>
#include <array>
#include <cstdint>

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
} // namespace GekkoAOT::GX
