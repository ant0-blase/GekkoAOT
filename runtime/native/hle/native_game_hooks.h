// Interior hooks for hand-written native game services. Shared by the runtime
// and tools/secondary_build.c, so this header stays plain C.
//
// A hook replaces a stretch of one known guest function: the builder asks
// DolRecomp to call the host at hook_pc when the function's code matches, and
// the runtime checks the same hash again before it takes over.
#ifndef GEKKOAOT_NATIVE_GAME_HOOKS_H
#define GEKKOAOT_NATIVE_GAME_HOOKS_H

// Medal of Honor: Frontline (GMFE69), Moh2RelGC.elf
// CParticleSystem::RenderSystem(CDmaPacket&): the emission loop, entered right
// after the first GXBegin returned.
#define GEKKOAOT_MOH_RENDER_SYSTEM_START 0x8007d704u
#define GEKKOAOT_MOH_RENDER_SYSTEM_SIZE 0x11b4u
#define GEKKOAOT_MOH_RENDER_SYSTEM_HASH 0x79630181763ffbf4ull
#define GEKKOAOT_MOH_PARTICLES_HOOK_PC 0x8007e118u
#define GEKKOAOT_MOH_PARTICLES_EXIT_PC 0x8007e804u
#define GEKKOAOT_MOH_PARTICLES_TOKEN 0xfffb0003u

// DMMorphBlendDisplayLists(DMRenderObject_T*): the vertex loop that rebuilds a
// morphed object's display list, up to the 32-byte padding.
#define GEKKOAOT_MOH_MORPH_START 0x800f712cu
#define GEKKOAOT_MOH_MORPH_SIZE 0x4ecu
#define GEKKOAOT_MOH_MORPH_HASH 0x2fb177d162d00c3aull
#define GEKKOAOT_MOH_MORPH_HOOK_PC 0x800f7330u
#define GEKKOAOT_MOH_MORPH_EXIT_PC 0x800f75fcu
#define GEKKOAOT_MOH_MORPH_TOKEN 0xfffb0006u

// CPartBin::Link: the matrix loop of one display list (GXLoadPosMtxIndx,
// PSMTXInvXpose, GXLoadNrmMtxImm for every bone), with the routines it calls.
#define GEKKOAOT_MOH_LINK_START 0x800f5930u
#define GEKKOAOT_MOH_LINK_SIZE 0x5d4u
#define GEKKOAOT_MOH_LINK_HASH 0x9057b1d9252917bdull
#define GEKKOAOT_MOH_INVXPOSE_START 0x80116bdcu
#define GEKKOAOT_MOH_INVXPOSE_SIZE 0xc8u
#define GEKKOAOT_MOH_INVXPOSE_HASH 0xbbb92bd9cfe9e57cull
#define GEKKOAOT_MOH_GXMTX_START 0x801301e0u
#define GEKKOAOT_MOH_GXMTX_SIZE 0x104u
#define GEKKOAOT_MOH_GXMTX_HASH 0xd510c2ad5c659a52ull
#define GEKKOAOT_MOH_LINK_HOOK_PC 0x800f5decu
#define GEKKOAOT_MOH_LINK_EXIT_PC 0x800f5e34u
#define GEKKOAOT_MOH_LINK_TOKEN 0xfffb0008u

// MSL sin and cos with everything they call (__ieee754_rem_pio2 ... sin).
#define GEKKOAOT_MOH_MSL_TRIG_START 0x80141a64u
#define GEKKOAOT_MOH_MSL_TRIG_SIZE 0x1e04u
#define GEKKOAOT_MOH_MSL_TRIG_HASH 0xa84408f47daba5b8ull
#define GEKKOAOT_MOH_MSL_SIN 0x80143790u
#define GEKKOAOT_MOH_MSL_COS 0x80143228u
#define GEKKOAOT_MOH_MSL_SIN_TOKEN 0xfffb0004u
#define GEKKOAOT_MOH_MSL_COS_TOKEN 0xfffb0005u

// _matrix_rot_yzx(CMatrix&, long, long, long): six sin/cos calls per bone.
#define GEKKOAOT_MOH_ROT_YZX 0x800f4e98u
#define GEKKOAOT_MOH_ROT_YZX_SIZE 0x3f8u
#define GEKKOAOT_MOH_ROT_YZX_HASH 0x8136707b8d973638ull
#define GEKKOAOT_MOH_ROT_YZX_TOKEN 0xfffb0007u

#endif
