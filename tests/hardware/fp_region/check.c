#define _POSIX_C_SOURCE 200809L
#include "cpu/cpu.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef void (*Body)(CPUState *);
#define DECL(pc) void func_##pc##_on(CPUState *); void func_##pc##_off(CPUState *)
DECL(80010000);
DECL(80010200);
DECL(80010300);
DECL(80010400);
DECL(80010500);
DECL(80010600);
DECL(80010700);

static u64 rng = 0x831d6b52e058af79ull;
static u64 random64(void) {
  rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
  return rng;
}
static f64 double_bits(u64 bits) { f64 v; memcpy(&v, &bits, 8); return v; }
static int compare(const CPUState *on, const CPUState *off, u32 pc, u32 n) {
  if (!memcmp(on, off, sizeof(*on))) return 1;
  const unsigned char *a = (const unsigned char *)on;
  const unsigned char *b = (const unsigned char *)off;
  for (u32 i = 0; i < sizeof(*on); ++i)
    if (a[i] != b[i]) {
      fprintf(stderr, "coalescing mismatch pc=%08X iteration=%u byte=%u on=%02X off=%02X\n",
              pc, n, i, a[i], b[i]);
      break;
    }
  return 0;
}
static void prepare(CPUState *cpu, u32 pc, u32 fpscr, u32 kind) {
  cpu->pc = pc; cpu->lr = 0x81234564u; cpu->exception = 0;
  cpu->msr = 1u << 13; cpu->fpscr = fpscr; cpu->downcount = 100000;
  cpu->ctr = 4; cpu->gpr[3] = 0; cpu->gpr[4] = 0xcc000000u;
  static const u64 edge[] = {
      0, 0x8000000000000000ull, 1, 0x8000000000000001ull,
      0x0010000000000000ull, 0x8010000000000000ull,
      0x7ff0000000000000ull, 0xfff0000000000000ull,
      0x7ff0000000000001ull, 0xfff0000000000001ull,
      0x7ff8123456789abcull, 0xfff8123456789abcull,
      0x7fefffffffffffffull, 0xffefffffffffffffull,
      0x3810000000000000ull, 0xb810000000000000ull};
  for (u32 r = 0; r < 32; ++r) {
    if (kind == 0) {
      cpu->fpr[r] = double_bits(random64());
      cpu->ps1[r] = double_bits(random64());
    } else if (kind == 1) {
      // Install bit patterns without evaluating signalling NaNs.
      u64 lane0 = edge[(random64() >> 24) % (sizeof(edge)/sizeof(*edge))];
      u64 lane1 = edge[(random64() >> 24) % (sizeof(edge)/sizeof(*edge))];
      cpu->fpr[r] = double_bits(lane0); cpu->ps1[r] = double_bits(lane1);
    } else {
      cpu->fpr[r] = (f64)(s32)(random64() & 65535u) / 65536.0;
      cpu->ps1[r] = -(f64)(s32)(random64() & 65535u) / 65536.0;
    }
  }
}
static u64 clear_fp_read(CPUState *cpu, u32 ea, u8 width) {
  (void)ea; (void)width;
  cpu->msr &= ~(1u << 13);
  return 0x12345678;
}
static void reference_chain(CPUState *c, u32 first) {
#define STEP(i,call) do { if (first <= (i)) { call; } } while (0)
  STEP(0, ppc_ps_add_op(c,1,2,3)); STEP(1, ppc_ps_sub_op(c,4,5,6));
  STEP(2, ppc_ps_add_op(c,7,8,9)); STEP(3, ppc_ps_mul_op(c,10,1,4));
  STEP(4, ppc_ps_madd_op(c,11,10,4,7,false,false));
  STEP(5, ppc_ps_madd_op(c,12,11,4,7,true,false));
  STEP(6, ppc_ps_madd_op(c,13,12,4,7,true,true));
  STEP(7, ppc_ps_madd_op(c,14,13,4,7,false,true));
  STEP(8, ppc_ps_add_op(c,15,14,11)); STEP(9, ppc_ps_sub_op(c,16,15,12));
  STEP(10, ppc_ps_mul_op(c,17,16,4));
  STEP(11, ppc_ps_madd_op(c,18,17,4,7,false,false));
  STEP(12, ppc_ps_madd_op(c,19,18,4,7,true,false));
  STEP(13, ppc_ps_madd_op(c,20,19,4,7,true,true));
  STEP(14, ppc_ps_madd_op(c,21,20,4,7,false,true));
  STEP(15, ppc_ps_add_op(c,22,21,11)); STEP(16, ppc_ps_sub_op(c,23,22,12));
  STEP(17, ppc_ps_mul_op(c,24,23,4));
  STEP(18, ppc_ps_madd_op(c,25,24,4,7,false,false));
  STEP(19, ppc_ps_add_op(c,26,25,11)); STEP(20, ppc_ps_sub_op(c,27,26,12));
#undef STEP
}
static double seconds(void) {
  struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
  return t.tv_sec + t.tv_nsec * 1e-9;
}
static double benchmark(Body body, CPUState initial, unsigned count) {
  // Reset only initial sources and control state; the chain overwrites all
  // destinations. This measures generated execution, not memset/memcpy time.
  double start = seconds();
  for (unsigned i = 0; i < count; ++i) {
    initial.pc = 0x80010000u; initial.downcount = 100000;
    body(&initial);
  }
  return (seconds() - start) * 1e9 / count;
}
int main(void) {
  CPUState initial; if (!cpu_init(&initial)) return 1;
  const u32 controls[] = {0,1,2,3,4,5,6,7,0x80,0x02000080u,
                          0xffffffffu,0x60040000u,0xa01f0000u};
  struct Case { u32 pc; Body on, off; } cases[] = {
      {0x80010000,func_80010000_on,func_80010000_off},
      {0x80010020,func_80010000_on,func_80010000_off},
      {0x80010200,func_80010200_on,func_80010200_off},
      {0x80010300,func_80010300_on,func_80010300_off},
      {0x80010400,func_80010400_on,func_80010400_off},
      {0x80010500,func_80010500_on,func_80010500_off},
      {0x80010600,func_80010600_on,func_80010600_off},
      {0x80010608,func_80010600_on,func_80010600_off},
      {0x80010700,func_80010700_on,func_80010700_off}};
  u32 compared = 0;
  for (u32 n = 0; n < 160000; ++n) {
    const struct Case *test = &cases[n % (sizeof(cases)/sizeof(*cases))];
    prepare(&initial,test->pc,controls[n % (sizeof(controls)/sizeof(*controls))],n%3);
    if (n % 17 == 0) initial.msr &= ~(1u << 13);
    initial.ctr = test->pc >= 0x80010600 ? 0x80010608u : 4;
    initial.external_read = clear_fp_read;
    CPUState on = initial, off = initial;
    ppc_fpscr_control_updated(&on); test->on(&on);
    ppc_fpscr_control_updated(&off); test->off(&off);
    if (!compare(&on,&off,test->pc,n)) return 2;
    ++compared;
    if (test->pc < 0x80010200u && (initial.msr & (1u << 13))) {
      CPUState reference = initial;
      ppc_fpscr_control_updated(&reference);
      reference_chain(&reference,(test->pc-0x80010000u)/4);
      if (memcmp(on.fpr,reference.fpr,sizeof(on.fpr)) ||
          memcmp(on.ps1,reference.ps1,sizeof(on.ps1)) || on.fpscr != reference.fpscr) {
        fprintf(stderr,"independent paired helper mismatch n=%u fpscr=%08X\n",n,initial.fpscr);
        return 3;
      }
    }
  }
  // Assert restart addresses independently of the on/off comparison.
  prepare(&initial,0x80010000u,0,2); initial.msr = 0;
  func_80010000_on(&initial);
  if (!(initial.exception & PPC_EXC_FP_UNAVAILABLE) || initial.srr0 != 0x80010000u)
    return 4;
  prepare(&initial,0x80010020u,0,2); initial.msr = 0;
  func_80010000_on(&initial);
  if (!(initial.exception & PPC_EXC_FP_UNAVAILABLE) || initial.srr0 != 0x80010020u)
    return 4;
  prepare(&initial,0x80010300u,0,2); initial.external_read = clear_fp_read;
  func_80010300_on(&initial);
  if (!(initial.exception & PPC_EXC_FP_UNAVAILABLE) || initial.srr0 != 0x80010308u)
    return 4;
  prepare(&initial,0x80010000,0,2); ppc_fpscr_control_updated(&initial);
  double off = benchmark(func_80010000_off, initial, 1500000);
  double on = benchmark(func_80010000_on, initial, 1500000);
  printf("FP_REGION_PASSED full-state=%u independent-helper=paired-chain off=%.2fns on=%.2fns ratio=%.4f\n",
         compared,off,on,on/off);
  cpu_free(&initial);
  return 0;
}
