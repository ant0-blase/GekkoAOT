// Medal of Honor: Frontline: native particle emission.
//
// The game never stores particles. Every frame RenderSystem replays each live
// emission from the emitter's seed: 11 random numbers per particle, a camera
// facing quad, 96 bytes through the write-gather pipe. Nothing but the seed and
// the particle counter feeds back into the game, so the loop is a pure function
// of what the guest computed before it.
#pragma once

#include "native/hle/native_game_hooks.h"

#include <cstddef>
#include <cstdint>

namespace GekkoAOT::Native::MohFrontline {

inline constexpr std::uint32_t kTokenParticles = GEKKOAOT_MOH_PARTICLES_TOKEN;
inline constexpr std::uint32_t kRenderSystemStart = GEKKOAOT_MOH_RENDER_SYSTEM_START;
inline constexpr std::uint32_t kRenderSystemSize = GEKKOAOT_MOH_RENDER_SYSTEM_SIZE;
inline constexpr std::uint64_t kRenderSystemHash = GEKKOAOT_MOH_RENDER_SYSTEM_HASH;
inline constexpr std::uint32_t kHookPc = GEKKOAOT_MOH_PARTICLES_HOOK_PC;
inline constexpr std::uint32_t kExitPc = GEKKOAOT_MOH_PARTICLES_EXIT_PC;
inline constexpr std::uint32_t kTrigStart = GEKKOAOT_MOH_MSL_TRIG_START;
inline constexpr std::uint32_t kTrigSize = GEKKOAOT_MOH_MSL_TRIG_SIZE;
inline constexpr std::uint64_t kTrigHash = GEKKOAOT_MOH_MSL_TRIG_HASH;
inline constexpr std::uint32_t kTokenSin = GEKKOAOT_MOH_MSL_SIN_TOKEN;
inline constexpr std::uint32_t kTokenCos = GEKKOAOT_MOH_MSL_COS_TOKEN;

// Guest values at kHookPc (registers and r1-relative stack slots).
struct ParticleInput {
  std::uint32_t seed, counter;
  std::int32_t rate, delay, frame;
  std::uint16_t nverts;
  bool rotate;
  float t, age, sys_age, t_step, age_step, sys_age_step, last_emit;
  float half, growth, one, zero, scale;
  float sys_vel[3], sys_accel[3], accel[3];
  float pos0[3], pos_range[3], vel0[3], vel_range[3];
  float start[4], start_range[4], end[4], end_range[4];
  float right[3], up[3], fwd[3];
  float width_range, width0, height_range, height0, rot_range, rot0;
};

struct ParticleResult {
  std::uint32_t seed, counter;
  std::uint64_t particles, rotated, batches;
};

using ParticleFlush = void (*)(void* context, const std::uint8_t* bytes, std::size_t size);

// Writes the FIFO bytes the guest loop would write, big-endian, through
// `flush`. The first batch's GXBegin header has already been written by the
// guest; later headers are part of the stream. `capacity` is at least 256.
void EmitParticles(const ParticleInput& input, ParticleResult* result, std::uint8_t* buffer,
                   std::size_t capacity, ParticleFlush flush, void* context);

std::uint64_t CodeHash(const std::uint8_t* bytes, std::size_t size);

// DMMorphBlendDisplayLists' vertex loop. Each 7-byte list vertex becomes a
// 15-byte one with its position and normal blended over the active targets.
inline constexpr std::uint32_t kTokenMorph = GEKKOAOT_MOH_MORPH_TOKEN;
inline constexpr std::uint32_t kMorphStart = GEKKOAOT_MOH_MORPH_START;
inline constexpr std::uint32_t kMorphSize = GEKKOAOT_MOH_MORPH_SIZE;
inline constexpr std::uint64_t kMorphHash = GEKKOAOT_MOH_MORPH_HASH;
inline constexpr std::uint32_t kMorphHookPc = GEKKOAOT_MOH_MORPH_HOOK_PC;
inline constexpr std::uint32_t kMorphExitPc = GEKKOAOT_MOH_MORPH_EXIT_PC;
inline constexpr std::uint32_t kMorphMaxTargets = 16u;

struct MorphInput {
  // Offsets into MEM1.
  std::uint32_t list, list_end, output;
  std::uint32_t positions, normals;
  std::uint32_t target_data[kMorphMaxTargets];
  float weight[kMorphMaxTargets];
  std::uint32_t targets;
};

// False when any access would leave MEM1: nothing was written, the guest loop
// runs instead. `output` is written up to the returned byte count.
bool MorphBlend(std::uint8_t* ram, std::uint32_t ram_size, const MorphInput& input,
                std::uint32_t* written, std::uint64_t* vertices);

// CPartBin::Link's matrix loop. `matrices` holds the bone matrices (3x4), `normal`
// the guest's temporary normal matrix (3x4), kept when a bone is singular as the
// guest code keeps it. Returns the FIFO bytes written to `out` (46 per bone).
inline constexpr std::uint32_t kTokenLink = GEKKOAOT_MOH_LINK_TOKEN;
inline constexpr std::uint32_t kLinkHookPc = GEKKOAOT_MOH_LINK_HOOK_PC;
inline constexpr std::uint32_t kLinkExitPc = GEKKOAOT_MOH_LINK_EXIT_PC;
inline constexpr std::size_t kLinkBytesPerBone = 46u;
std::size_t EmitListMatrices(const std::uint8_t* matrices, const std::uint8_t* indices, unsigned count,
                             std::uint8_t* normal, std::uint8_t* out);

// _matrix_rot_yzx: rotates the basis of the guest matrix at `matrix`.
inline constexpr std::uint32_t kTokenRotYzx = GEKKOAOT_MOH_ROT_YZX_TOKEN;
inline constexpr std::uint32_t kRotYzx = GEKKOAOT_MOH_ROT_YZX;
inline constexpr std::uint32_t kRotYzxSize = GEKKOAOT_MOH_ROT_YZX_SIZE;
inline constexpr std::uint64_t kRotYzxHash = GEKKOAOT_MOH_ROT_YZX_HASH;
void MatrixRotYzx(std::uint8_t* matrix, std::int32_t rx, std::int32_t ry, std::int32_t rz);

// MSL's sin and cos, operation for operation (|x| <= 2^19 * pi/2 or not finite;
// larger angles use the host library and are not bit exact).
double MslSin(double x);
double MslCos(double x);

}  // namespace GekkoAOT::Native::MohFrontline
