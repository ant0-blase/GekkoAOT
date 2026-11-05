// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <optional>

namespace GekkoAOT::GX::Shadow {
// Explicit common-space inputs. GX model-view matrices alone do not establish
// a world-space frame. Callers must retain that provenance with their casters.
using Vec3 = std::array<double, 3>;
using Mat4 = std::array<double, 16>; // row-major, column-vector multiplication
inline double Dot(Vec3 a, Vec3 b) noexcept {
    return a[0]*b[0]+a[1]*b[1]+a[2]*b[2];
}
inline Vec3 Cross(Vec3 a, Vec3 b) noexcept {
    return {a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]};
}
inline bool Finite(Vec3 v) noexcept {
    return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]);
}
inline std::optional<Vec3> Unit(Vec3 v) noexcept {
    if (!Finite(v)) return {};
    const auto length=std::hypot(v[0],v[1],v[2]);
    if (!(length>1e-12) || !std::isfinite(length)) return {};
    for(auto& x:v)x/=length;
    return v;
}
struct Basis { Vec3 right, up, forward; };
inline std::optional<Basis> MakeBasis(Vec3 forward, Vec3 up) noexcept {
    const auto f=Unit(forward);
    if(!f)return {};
    const auto r=Unit(Cross(up,*f));
    if(!r)return {};
    return Basis{*r,Cross(*f,*r),*f};
}

// Six separate 90-degree depth projections; near/far are radial-axis distances,
// not a directional cascade split. No atlas tile transform is baked into these.
inline std::optional<std::array<Mat4,6>> PointFaces(
    Vec3 position,double nearPlane,double farPlane,bool reversed=true) noexcept {
    if(!Finite(position) || !std::isfinite(nearPlane) || !std::isfinite(farPlane) ||
       !(nearPlane>0 && farPlane>nearPlane))return {};
    constexpr std::array<Vec3,6> forward={Vec3{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}};
    constexpr std::array<Vec3,6> up={Vec3{0,1,0},{0,1,0},{0,0,-1},{0,0,1},{0,1,0},{0,1,0}};
    std::array<Mat4,6> result{};
    const double a=reversed ? -nearPlane/(farPlane-nearPlane) : farPlane/(farPlane-nearPlane);
    const double b=(reversed ? nearPlane : -nearPlane)*(farPlane/(farPlane-nearPlane));
    for(unsigned i=0;i<6;++i){
        const auto basis=*MakeBasis(forward[i],up[i]);
        auto& m=result[i];
        for(unsigned k=0;k<3;++k){m[k]=basis.right[k];m[4+k]=basis.up[k];
            m[8+k]=a*basis.forward[k];m[12+k]=basis.forward[k];}
        m[3]=-Dot(basis.right,position);m[7]=-Dot(basis.up,position);
        m[11]=b-a*Dot(basis.forward,position);m[15]=-Dot(basis.forward,position);
        for(auto value:m)if(!std::isfinite(value))return {};
    }
    return result;
}

struct Cascade { Mat4 clip; double texelSize=0, nearDepth=0, farDepth=0; };
// The eight corners must already describe one split in a proved common space.
// Padding expands caster depth coverage; off-camera casters still need capture.
inline std::optional<Cascade> DirectionalCascade(
    const std::array<Vec3,8>& corners,Vec3 direction,std::uint32_t resolution,
    double casterPadding,bool reversed=true) noexcept {
    if(resolution<4 || resolution>16384 || !std::isfinite(casterPadding) || casterPadding<0)return {};
    const auto f=Unit(direction);
    if(!f)return {};
    const auto basis=MakeBasis(*f,std::abs((*f)[1])<.95 ? Vec3{0,1,0} : Vec3{0,0,1});
    Vec3 center{};
    for(const auto& p:corners){if(!Finite(p))return {};for(unsigned k=0;k<3;++k)center[k]+=p[k]/8.;}
    double radius=0,lo=INFINITY,hi=-INFINITY;
    for(const auto& p:corners){
        radius=std::max(radius,std::hypot(p[0]-center[0],p[1]-center[1],p[2]-center[2]));
        const auto z=Dot(basis->forward,p);lo=std::min(lo,z);hi=std::max(hi,z);
    }
    if(!(radius>1e-9) || !std::isfinite(radius))return {};
    // Quantize extent and allow half a texel of center snapping on each edge.
    radius=std::ceil(radius*16.)/16.;radius/=1.-1./resolution;
    const double texel=2.*radius/resolution;
    const double x=std::floor(Dot(basis->right,center)/texel+.5)*texel;
    const double y=std::floor(Dot(basis->up,center)/texel+.5)*texel;
    lo-=casterPadding;hi+=casterPadding;
    if(!(hi-lo>1e-9) || !std::isfinite(lo) || !std::isfinite(hi))return {};
    Cascade out{};out.texelSize=texel;out.nearDepth=lo;out.farDepth=hi;
    const double zScale=(reversed ? -1. : 1.)/(hi-lo);
    for(unsigned k=0;k<3;++k){out.clip[k]=basis->right[k]/radius;
        out.clip[4+k]=basis->up[k]/radius;out.clip[8+k]=basis->forward[k]*zScale;}
    out.clip[3]=-x/radius;out.clip[7]=-y/radius;
    out.clip[11]=(reversed ? hi : -lo)/(hi-lo);out.clip[15]=1.;
    for(auto value:out.clip)if(!std::isfinite(value))return {};
    return out;
}

inline std::optional<std::array<double,5>> CascadeSplits(
    double nearPlane,double farPlane,unsigned count,double logarithmicWeight=.6) noexcept {
    if(count<1 || count>4 || !std::isfinite(nearPlane) || !std::isfinite(farPlane) ||
       !(nearPlane>0 && farPlane>nearPlane) || !std::isfinite(logarithmicWeight) ||
       logarithmicWeight<0 || logarithmicWeight>1)return {};
    std::array<double,5> splits{};splits[0]=nearPlane;
    for(unsigned i=1;i<=count;++i){
        const double t=double(i)/count;
        const double linear=nearPlane+(farPlane-nearPlane)*t;
        const double logarithmic=std::exp(std::log(nearPlane)*(1.-t)+std::log(farPlane)*t);
        splits[i]=i==count ? farPlane : linear*(1.-logarithmicWeight)+logarithmic*logarithmicWeight;
    }
    return splits;
}
} // namespace GekkoAOT::GX::Shadow
