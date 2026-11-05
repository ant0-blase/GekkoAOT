// See moh_particles.h. Compiled with -ffp-contract=off: every multiply-add below
// is fused exactly where the PowerPC code uses fmadds/fmadd, and only there.
#include "native/hle/moh_particles.h"

#include <cmath>
#include <cstring>

#if defined(_MSC_VER)
#include <stdlib.h>
#endif

namespace GekkoAOT::Native::MohFrontline {
namespace {

inline std::uint32_t Swap32(std::uint32_t value) {
#if defined(_MSC_VER)
  return _byteswap_ulong(value);
#else
  return __builtin_bswap32(value);
#endif
}

inline std::uint8_t* PutWord(std::uint8_t* out, std::uint32_t value) {
  value = Swap32(value);
  std::memcpy(out, &value, 4u);
  return out + 4u;
}

inline std::uint8_t* PutFloat(std::uint8_t* out, float value) {
  std::uint32_t bits;
  std::memcpy(&bits, &value, 4u);
  return PutWord(out, bits);
}

inline std::int32_t HighWord(double value) {
  std::uint64_t bits;
  std::memcpy(&bits, &value, 8u);
  return std::int32_t(bits >> 32u);
}

inline double Fma(double a, double c, double b) { return std::fma(a, c, b); }
inline float Fmas(float a, float c, float b) { return std::fma(a, c, b); }

// MSL's fdlibm, operation for operation with the code in the game's ELF.
inline double KernelSin(double x, double y, bool has_tail) {
  if ((HighWord(x) & 0x7fffffff) < 0x3e400000) return x;
  const double z = x * x;
  const double v = z * x;
  double r = Fma(0x1.5d93a5acfd57cp-33, z, -0x1.ae5e68a2b9cebp-26);
  r = Fma(z, r, 0x1.71de357b1fe7dp-19);
  r = Fma(z, r, -0x1.a01a019c161d5p-13);
  r = Fma(z, r, 0x1.111111110f8a6p-7);
  if (!has_tail) return Fma(v, Fma(z, r, -0x1.5555555555549p-3), x);
  double w = Fma(0.5, y, -(v * r));
  w = Fma(z, w, -y);
  w = -Fma(-0x1.5555555555549p-3, v, -w);
  return x - w;
}

inline double KernelCos(double x, double y) {
  const std::int32_t ix = HighWord(x) & 0x7fffffff;
  if (ix < 0x3e400000) return 1.0;
  const double z = x * x;
  double r = Fma(-0x1.8fae9be8838d4p-37, z, 0x1.1ee9ebdb4b1c4p-29);
  r = Fma(z, r, -0x1.27e4f809c52adp-22);
  r = Fma(z, r, 0x1.a01a019cb1590p-16);
  r = Fma(z, r, -0x1.6c16c16c15177p-10);
  r = Fma(z, r, 0x1.555555555554cp-5);
  const double zr = z * r;
  if (ix < 0x3fd33333) {
    const double w = Fma(z, zr, -(x * y));
    return 1.0 - Fma(0.5, z, -w);
  }
  double qx = 0x1.2p-2;
  if (ix <= 0x3fe90000) {
    const std::uint64_t bits = std::uint64_t(std::uint32_t(ix - 0x00200000)) << 32u;
    std::memcpy(&qx, &bits, 8u);
  }
  const double hz = Fma(0.5, z, -qx);
  const double a = 1.0 - qx;
  return a - (hz - Fma(z, zr, -(x * y)));
}

constexpr std::int32_t kNpio2Hw[32] = {
    0x3ff921fb, 0x400921fb, 0x4012d97c, 0x401921fb, 0x401f6a7a, 0x4022d97c, 0x4025fdbb, 0x402921fb,
    0x402c463a, 0x402f6a7a, 0x4031475c, 0x4032d97c, 0x40346b9c, 0x4035fdbb, 0x40378fdb, 0x403921fb,
    0x403ab41b, 0x403c463a, 0x403dd85a, 0x403f6a7a, 0x40407e4c, 0x4041475c, 0x4042106c, 0x4042d97c,
    0x4043a28c, 0x40446b9c, 0x404534ac, 0x4045fdbb, 0x4046c6cb, 0x40478fdb, 0x404858eb, 0x404921fb};

constexpr double kPio2_1 = 0x1.921fb54400000p+0, kPio2_1t = 0x1.0b4611a626331p-34;
constexpr double kPio2_2 = 0x1.0b4611a600000p-34, kPio2_2t = 0x1.3198a2e037073p-69;
constexpr double kPio2_3 = 0x1.3198a2e000000p-69, kPio2_3t = 0x1.b839a252049c1p-104;

// |x| <= 2^19 * pi/2. Larger angles cannot come out of a particle's rotation
// range; Sin/Cos leave them to the host library, only a vertex depends on it.
inline std::int32_t RemPio2(double x, double y[2]) {
  const std::int32_t hx = HighWord(x);
  const std::int32_t ix = hx & 0x7fffffff;
  if (ix < 0x4002d97c) {
    if (hx > 0) {
      double z = x - kPio2_1;
      if (ix != 0x3ff921fb) {
        y[0] = z - kPio2_1t;
        y[1] = (z - y[0]) - kPio2_1t;
      } else {
        z = z - kPio2_2;
        y[0] = z - kPio2_2t;
        y[1] = (z - y[0]) - kPio2_2t;
      }
      return 1;
    }
    double z = kPio2_1 + x;
    if (ix != 0x3ff921fb) {
      y[0] = kPio2_1t + z;
      y[1] = kPio2_1t + (z - y[0]);
    } else {
      z = z + kPio2_2;
      y[0] = kPio2_2t + z;
      y[1] = kPio2_2t + (z - y[0]);
    }
    return -1;
  }
  double t = std::fabs(x);
  const std::int32_t n = std::int32_t(Fma(0x1.45f306dc9c883p-1, t, 0.5));
  const double fn = double(n);
  double r = Fma(-kPio2_1, fn, t);
  double w = kPio2_1t * fn;
  y[0] = r - w;
  if (!(n < 32 && ix != kNpio2Hw[n - 1])) {
    const std::int32_t j = ix >> 20;
    std::int32_t i = j - ((HighWord(y[0]) >> 20) & 0x7ff);
    if (i > 16) {
      t = r;
      w = kPio2_2 * fn;
      r = t - w;
      w = Fma(kPio2_2t, fn, -((t - r) - w));
      y[0] = r - w;
      i = j - ((HighWord(y[0]) >> 20) & 0x7ff);
      if (i > 49) {
        t = r;
        w = kPio2_3 * fn;
        r = t - w;
        w = Fma(kPio2_3t, fn, -((t - r) - w));
        y[0] = r - w;
      }
    }
  }
  y[1] = (r - y[0]) - w;
  if (hx < 0) {
    y[0] = -y[0];
    y[1] = -y[1];
    return -n;
  }
  return n;
}

inline double Sin(double x) {
  const std::int32_t ix = HighWord(x) & 0x7fffffff;
  if (ix <= 0x3fe921fb) return KernelSin(x, 0.0, false);
  if (ix >= 0x7ff00000) return x - x;
  if (ix > 0x413921fb) return std::sin(x);
  double y[2];
  switch (RemPio2(x, y) & 3) {
    case 0: return KernelSin(y[0], y[1], true);
    case 1: return KernelCos(y[0], y[1]);
    case 2: return -KernelSin(y[0], y[1], true);
    default: return -KernelCos(y[0], y[1]);
  }
}

inline double Cos(double x) {
  const std::int32_t ix = HighWord(x) & 0x7fffffff;
  if (ix <= 0x3fe921fb) return KernelCos(x, 0.0);
  if (ix >= 0x7ff00000) return x - x;
  if (ix > 0x413921fb) return std::cos(x);
  double y[2];
  switch (RemPio2(x, y) & 3) {
    case 0: return KernelCos(y[0], y[1]);
    case 1: return -KernelSin(y[0], y[1], true);
    case 2: return -KernelCos(y[0], y[1]);
    default: return KernelSin(y[0], y[1], true);
  }
}

// fctiwz: saturating, NaN gives INT_MIN.
inline std::int32_t Fctiwz(float value) {
  if (value >= 2147483648.0f) return 0x7fffffff;
  if (value >= -2147483648.0f) return std::int32_t(value);
  return std::int32_t(0x80000000u);
}

// The guest stores the low byte of the integer.
inline std::uint32_t ColorByte(float value) { return std::uint32_t(Fctiwz(value)) & 0xffu; }

inline std::uint32_t Be16(const std::uint8_t* p) { return (std::uint32_t(p[0]) << 8u) | p[1]; }
inline std::int32_t Be16s(const std::uint8_t* p) { return std::int16_t(Be16(p)); }
inline void PutBe16(std::uint8_t* p, std::int32_t value) {
  p[0] = std::uint8_t(std::uint32_t(value) >> 8u);
  p[1] = std::uint8_t(value);
}

}  // namespace

namespace {
// fres: Gekko's table estimate of 1/x, for a normal single-precision x.
struct EstimateEntry { std::int32_t base, dec; };
constexpr EstimateEntry kFresTable[32] = {
    {0x7ff800, 0x3e1}, {0x783800, 0x3a7}, {0x70ea00, 0x371}, {0x6a0800, 0x340},
    {0x638800, 0x313}, {0x5d6200, 0x2ea}, {0x579000, 0x2c4}, {0x520800, 0x2a0},
    {0x4cc800, 0x27f}, {0x47ca00, 0x261}, {0x430800, 0x245}, {0x3e8000, 0x22a},
    {0x3a2c00, 0x212}, {0x360800, 0x1fb}, {0x321400, 0x1e5}, {0x2e4a00, 0x1d1},
    {0x2aa800, 0x1be}, {0x272c00, 0x1ac}, {0x23d600, 0x19b}, {0x209e00, 0x18b},
    {0x1d8800, 0x17c}, {0x1a9000, 0x16e}, {0x17ae00, 0x15b}, {0x14f800, 0x15b},
    {0x124400, 0x143}, {0x0fbe00, 0x143}, {0x0d3800, 0x12d}, {0x0ade00, 0x12d},
    {0x088400, 0x11a}, {0x065000, 0x11a}, {0x041c00, 0x108}, {0x020c00, 0x106}};

inline double ApproxReciprocal(double value) {
  std::uint64_t bits;
  std::memcpy(&bits, &value, 8u);
  const std::uint64_t mantissa = bits & 0x000fffffffffffffull;
  const std::uint64_t sign = bits & 0x8000000000000000ull;
  std::uint64_t exponent = bits & 0x7ff0000000000000ull;
  double result;
  std::uint64_t out;
  if (mantissa == 0u && exponent == 0u) out = sign | 0x7ff0000000000000ull;
  else if (exponent == 0x7ff0000000000000ull) out = mantissa == 0u ? sign : bits | 0x0008000000000000ull;
  else if (exponent < (895ull << 52u)) out = sign | 0x47efffffe0000000ull;
  else if (exponent >= (1149ull << 52u)) out = sign;
  else {
    exponent = 0x7fd0000000000000ull - exponent;
    const auto i = std::uint32_t(mantissa >> 37u);
    const auto& entry = kFresTable[i / 1024u];
    out = sign | exponent |
          (std::uint64_t(entry.base - (entry.dec * std::int32_t(i % 1024u) + 1) / 2) << 29u);
  }
  std::memcpy(&result, &out, 8u);
  return result;
}

inline float GetFloat(const std::uint8_t* p) {
  std::uint32_t bits;
  std::memcpy(&bits, p, 4u);
  bits = Swap32(bits);
  float value;
  std::memcpy(&value, &bits, 4u);
  return value;
}
}  // namespace

std::size_t EmitListMatrices(const std::uint8_t* matrices, const std::uint8_t* indices, unsigned count,
                             std::uint8_t* normal, std::uint8_t* out) {
  std::uint8_t* const begin = out;
  for (unsigned j = 0; j < count; ++j) {
    const std::uint32_t index = indices[j];
    const std::uint32_t slot = j * 3u;
    // GXLoadPosMtxIndx(index, slot)
    *out++ = 0x20u;
    out = PutWord(out, (index << 16u) | ((((slot << 2u) & 0xffff0fffu) | 0xb000u) & 0xffffu));

    // PSMTXInvXpose, lane for lane with the paired-single code.
    const std::uint8_t* m = matrices + index * 0x30u;
    const float m00 = GetFloat(m), m01 = GetFloat(m + 4u), m02 = GetFloat(m + 8u);
    const float m10 = GetFloat(m + 0x10u), m11 = GetFloat(m + 0x14u), m12 = GetFloat(m + 0x18u);
    const float m20 = GetFloat(m + 0x20u), m21 = GetFloat(m + 0x24u), m22 = GetFloat(m + 0x28u);
    const float f11_0 = Fmas(m01, m12, -(m11 * m02)), f11_1 = Fmas(m02, m10, -(m12 * m00));
    const float f13_0 = Fmas(m11, m22, -(m21 * m12)), f13_1 = Fmas(m12, m20, -(m22 * m10));
    const float f12_0 = Fmas(m21, m02, -(m01 * m22)), f12_1 = Fmas(m22, m00, -(m02 * m20));
    const float f10 = Fmas(m10, m21, -(m11 * m20));
    const float f9 = Fmas(m01, m20, -(m00 * m21));
    const float f8 = Fmas(m00, m11, -(m01 * m10));
    const float det = Fmas(m20, f11_0, Fmas(m10, f12_0, m00 * f13_0));
    if (det != 0.0f) {
      const float estimate = float(ApproxReciprocal(double(det)));
      const float inverse = -Fmas(det, estimate * estimate, -(estimate + estimate));
      PutFloat(normal + 0x00u, f13_0 * inverse);
      PutFloat(normal + 0x04u, f13_1 * inverse);
      PutFloat(normal + 0x08u, f10 * inverse);
      PutWord(normal + 0x0cu, 0u);
      PutFloat(normal + 0x10u, f12_0 * inverse);
      PutFloat(normal + 0x14u, f12_1 * inverse);
      PutFloat(normal + 0x18u, f9 * inverse);
      PutWord(normal + 0x1cu, 0u);
      PutFloat(normal + 0x20u, f11_0 * inverse);
      PutFloat(normal + 0x24u, f11_1 * inverse);
      PutFloat(normal + 0x28u, f8 * inverse);
      PutWord(normal + 0x2cu, 0u);
    }

    // GXLoadNrmMtxImm(normal, slot)
    *out++ = 0x10u;
    out = PutWord(out, 0x00080000u + slot * 3u + 0x400u);
    std::memcpy(out, normal, 12u);
    std::memcpy(out + 12u, normal + 0x10u, 12u);
    std::memcpy(out + 24u, normal + 0x20u, 12u);
    out += 36u;
  }
  return std::size_t(out - begin);
}

void MatrixRotYzx(std::uint8_t* m, std::int32_t rx, std::int32_t ry, std::int32_t rz) {
  const auto get = [m](unsigned offset) {
    std::uint32_t bits;
    std::memcpy(&bits, m + offset, 4u);
    bits = Swap32(bits);
    float value;
    std::memcpy(&value, &bits, 4u);
    return value;
  };
  const auto set = [m](unsigned offset, float value) { PutFloat(m + offset, value); };
  const auto radians = [](std::int32_t angle) { return (6.2831855f * float(angle)) / 65536.0f; };
  const float ax = radians(rx), ay = radians(ry), az = radians(rz);
  const float sx = float(Sin(double(ax))), sy = float(Sin(double(ay))), sz = float(Sin(double(az)));
  const float cx = float(Cos(double(ax))), cy = float(Cos(double(ay))), cz = float(Cos(double(az)));
  const float right[3] = {get(0x00u), get(0x04u), get(0x08u)};
  const float front[3] = {get(0x10u), get(0x14u), get(0x18u)};
  const float up[3] = {get(0x20u), get(0x24u), get(0x28u)};
  for (unsigned i = 0; i < 3u; ++i) {
    const float r = cy * right[i] - sy * up[i];
    const float u = cy * up[i] + sy * right[i];
    const float f = cz * front[i] - sz * r;
    set(i * 4u, cz * r + sz * front[i]);
    set(0x10u + i * 4u, cx * f + sx * u);
    set(0x20u + i * 4u, cx * u - sx * f);
  }
}

double MslSin(double x) { return Sin(x); }
double MslCos(double x) { return Cos(x); }

std::uint64_t CodeHash(const std::uint8_t* bytes, std::size_t size) {
  std::uint64_t hash = 0xcbf29ce484222325ull;
  for (std::size_t i = 0; i < size; ++i) hash = (hash ^ bytes[i]) * 0x100000001b3ull;
  return hash;
}

bool MorphBlend(std::uint8_t* ram, std::uint32_t ram_size, const MorphInput& in,
                std::uint32_t* written, std::uint64_t* vertices) {
  // The guest loop stops on a zero opcode or exactly at the list's end.
  std::uint64_t cursor = in.list, total = 0u, primitives = 0u;
  while (true) {
    if (cursor + 3u > ram_size) return false;
    if (cursor == in.list_end || ram[cursor] == 0u) break;
    const std::uint32_t count = Be16(ram + cursor + 1u);
    cursor += 3u + std::uint64_t(count) * 7u;
    total += count;
    ++primitives;
  }
  const std::uint64_t size = primitives * 3u + total * 15u;
  if (std::uint64_t(in.output) + size > ram_size) return false;
  const std::uint64_t index_span = 0x10000u * 6u;
  if (std::uint64_t(in.positions) + index_span > ram_size ||
      std::uint64_t(in.normals) + index_span > ram_size)
    return false;
  for (std::uint32_t t = 0; t < in.targets; ++t)
    if (std::uint64_t(in.target_data[t]) + total * 4u > ram_size) return false;

  const std::uint8_t* const positions = ram + in.positions;
  const std::uint8_t* const normals = ram + in.normals;
  const std::uint8_t* cmd = ram + in.list;
  const std::uint8_t* const end = ram + in.list_end;
  std::uint8_t* out = ram + in.output;
  std::uint32_t pair = 0u;
  while (cmd != end && *cmd != 0u) {
    const std::uint32_t count = Be16(cmd + 1u);
    out[0] = cmd[0];
    out[1] = cmd[1];
    out[2] = cmd[2];
    out += 3u;
    const std::uint8_t* vertex = cmd + 3u;
    for (std::uint32_t i = 0; i < count; ++i, vertex += 7u, out += 15u, pair += 4u) {
      const std::uint8_t* position = positions + Be16(vertex + 1u) * 6u;
      const std::uint8_t* normal = normals + Be16(vertex + 3u) * 6u;
      std::int32_t value[6] = {Be16s(position), Be16s(position + 2u), Be16s(position + 4u),
                               Be16s(normal), Be16s(normal + 2u), Be16s(normal + 4u)};
      for (std::uint32_t t = 0; t < in.targets; ++t) {
        const std::uint8_t* indices = ram + in.target_data[t] + pair;
        const std::uint8_t* tp = positions + Be16(indices) * 6u;
        const std::uint8_t* tn = normals + Be16(indices + 2u) * 6u;
        const float w = in.weight[t];
        value[0] = std::int16_t(value[0] + Fctiwz(float(Be16s(tp)) * w));
        value[1] = std::int16_t(value[1] + Fctiwz(float(Be16s(tp + 2u)) * w));
        value[2] = std::int16_t(value[2] + Fctiwz(float(Be16s(tp + 4u)) * w));
        value[3] = std::int16_t(value[3] + Fctiwz(float(Be16s(tn)) * w));
        value[4] = std::int16_t(value[4] + Fctiwz(float(Be16s(tn + 2u)) * w));
        value[5] = std::int16_t(value[5] + Fctiwz(float(Be16s(tn + 4u)) * w));
      }
      out[0] = vertex[0];
      for (int c = 0; c < 6; ++c) PutBe16(out + 1 + c * 2, value[c]);
      out[13] = vertex[5];
      out[14] = vertex[6];
    }
    cmd = vertex;
  }
  *written = std::uint32_t(size);
  *vertices = total;
  return true;
}

void EmitParticles(const ParticleInput& in, ParticleResult* result, std::uint8_t* buffer,
                   std::size_t capacity, ParticleFlush flush, void* context) {
  std::uint8_t* out = buffer;
  std::uint8_t* const limit = buffer + capacity - 128u;
  std::uint32_t seed = in.seed;
  std::uint64_t particles = 0u, rotated = 0u, batches = 0u;
  float t = in.t, age = in.age, sys_age = in.sys_age;
  std::int32_t frame = in.frame;
  const float one = in.one, zero = in.zero, scale = in.scale;
  const auto rand_unit = [&seed, scale]() {
    seed = seed * 0x41c64e6du + 0x3039u;
    return float(std::int32_t(seed & 0x7fffu)) * scale;
  };

  for (bool first = true;; first = false) {
    const float sys_age2 = sys_age * sys_age;
    const float age2 = age * age;
    const float sys_x = sys_age * in.sys_vel[0] + sys_age2 * in.sys_accel[0];
    const float sys_y = sys_age * in.sys_vel[1] + sys_age2 * in.sys_accel[1];
    const float sys_z = sys_age * in.sys_vel[2] + sys_age2 * in.sys_accel[2];
    const float accel_x = age2 * in.accel[0];
    const float accel_y = age2 * in.accel[1];
    const float accel_z = age2 * in.accel[2];
    const float half_scale = in.half * Fmas(in.growth, t, one);
    const float t_inv = one - t;
    if (!first) {
      out[0] = 0x80u;
      out[1] = std::uint8_t(in.nverts >> 8u);
      out[2] = std::uint8_t(in.nverts);
      out += 3u;
    }
    ++batches;

    for (std::int32_t i = 0; i < in.rate; ++i) {
      const float u_px = rand_unit(), u_py = rand_unit(), u_pz = rand_unit();
      const float u_vx = rand_unit(), u_vy = rand_unit(), u_vz = rand_unit();
      const float mix_start = rand_unit(), mix_end = rand_unit();
      const float u_width = rand_unit(), u_height = rand_unit(), u_rot = rand_unit();

      const float px = Fmas(in.pos_range[0], u_px, in.pos0[0]);
      const float py = Fmas(in.pos_range[1], u_py, in.pos0[1]);
      const float pz = Fmas(in.pos_range[2], u_pz, in.pos0[2]);
      const float vx = Fmas(in.vel_range[0], u_vx, in.vel0[0]);
      const float vy = Fmas(in.vel_range[1], u_vy, in.vel0[1]);
      const float vz = Fmas(in.vel_range[2], u_vz, in.vel0[2]);

      std::uint32_t color = 0u;
      for (int c = 0; c < 4; ++c) {
        const float from = Fmas(in.start_range[c], mix_start, in.start[c]);
        const float to = Fmas(in.end_range[c], mix_end, in.end[c]);
        color = (color << 8u) | ColorByte(Fmas(t_inv, from, t * to));
      }

      const float width = Fmas(in.width_range, u_width, in.width0);
      const float height = Fmas(in.height_range, u_height, in.height0);
      const float cx = sys_x + (px + Fmas(vx, age, accel_x));
      const float cy = sys_y + (py + Fmas(vy, age, accel_y));
      const float cz = sys_z + (pz + Fmas(vz, age, accel_z));

      float rx, ry, rz, ux, uy, uz;
      if (!in.rotate) {
        const float w = half_scale * width;
        const float h = half_scale * height;
        rx = in.right[0] * w;
        ry = in.right[1] * w;
        rz = in.right[2] * w;
        ux = in.up[0] * h;
        uy = in.up[1] * h;
        uz = in.up[2] * h;
      } else {
        ++rotated;
        const double angle = double(t * Fmas(in.rot_range, u_rot, in.rot0));
        const float s = float(Sin(angle));
        const float c = float(Cos(angle));
        const float ax = in.fwd[0], ay = in.fwd[1], az = in.fwd[2];
        const float k = one - c;
        const float kx = k * ax, ky = k * ay, kz = k * az;
        const float sx = s * ax, sy = s * ay, sz = s * az;
        const float kxy = kx * ay, kyz = az * ky, kxz = kx * az;
        const float m00 = Fmas(kx, ax, c), m11 = Fmas(ay, ky, c), m22 = Fmas(az, kz, c);
        const float m01 = kxy - sz, m10 = sz + kxy;
        const float m02 = sy + kxz, m20 = kxz - sy;
        const float m21 = sx + kyz, m12 = kyz - sx;
        const float w = half_scale * width;
        const float h = half_scale * height;
        const float r0 = in.right[0], r1 = in.right[1], r2 = in.right[2];
        const float u0 = in.up[0], u1 = in.up[1], u2 = in.up[2];
        rx = w * Fmas(r2, m02, Fmas(r0, m00, r1 * m01));
        ry = w * Fmas(r2, m12, Fmas(r0, m10, r1 * m11));
        rz = w * Fmas(r2, m22, Fmas(r0, m20, r1 * m21));
        ux = h * Fmas(u2, m02, Fmas(u0, m00, u1 * m01));
        uy = h * Fmas(u2, m12, Fmas(u0, m10, u1 * m11));
        uz = h * Fmas(u2, m22, Fmas(u0, m20, u1 * m21));
      }

      const float xm = cx - rx, xp = cx + rx;
      const float ym = cy - ry, yp = cy + ry;
      const float zm = cz - rz, zp = cz + rz;
      out = PutFloat(out, xm + ux); out = PutFloat(out, ym + uy); out = PutFloat(out, zm + uz);
      out = PutWord(out, color);
      out = PutFloat(out, zero); out = PutFloat(out, zero);
      out = PutFloat(out, xp + ux); out = PutFloat(out, yp + uy); out = PutFloat(out, zp + uz);
      out = PutWord(out, color);
      out = PutFloat(out, one); out = PutFloat(out, zero);
      out = PutFloat(out, xp - ux); out = PutFloat(out, yp - uy); out = PutFloat(out, zp - uz);
      out = PutWord(out, color);
      out = PutFloat(out, one); out = PutFloat(out, one);
      out = PutFloat(out, xm - ux); out = PutFloat(out, ym - uy); out = PutFloat(out, zm - uz);
      out = PutWord(out, color);
      out = PutFloat(out, zero); out = PutFloat(out, one);
      if (out >= limit) {
        flush(context, buffer, std::size_t(out - buffer));
        out = buffer;
      }
    }
    particles += std::uint64_t(in.rate > 0 ? in.rate : 0);

    age += in.age_step;
    t += in.t_step;
    sys_age += in.sys_age_step;
    frame += in.delay;
    if (!(float(frame) <= in.last_emit)) break;
    if (out >= limit) {
      flush(context, buffer, std::size_t(out - buffer));
      out = buffer;
    }
  }
  if (out != buffer) flush(context, buffer, std::size_t(out - buffer));
  result->seed = seed;
  result->counter = in.counter + std::uint32_t(particles);
  result->particles = particles;
  result->rotated = rotated;
  result->batches = batches;
}

}  // namespace GekkoAOT::Native::MohFrontline
