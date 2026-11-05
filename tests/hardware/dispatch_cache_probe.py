#!/usr/bin/env python3
"""Compile the emitted lookup, check range/collision/thread semantics and time it."""
from pathlib import Path
import subprocess
import textwrap

root = Path(__file__).resolve().parents[2]
out = root / '.gekkoaot/diagnostics/dispatch-cache-v227'
out.mkdir(parents=True, exist_ok=True)
source = root / '.gekkoaot/src/DolRecomp/src'
ranges = [(0, 16)]  # PC zero is valid code, not an empty-cache sentinel.
position = 0x3000
for i in range(600):
    width = [16, 32, 64, 128, 20, 36][i % 6]
    ranges.append((position, position + width))
    position += width + (0 if i % 3 else 12)
rows = ',\n'.join(f'{{0x{a:x}u,0x{b:x}u,0}}' for a,b in ranges)
generator = out / 'emit.c'
generator.write_text(textwrap.dedent('''\
#include "backend/dispatch.c"
int main(int argc, char** argv) {
  if (argc != 2) return 1;
  FunctionRange ranges[] = { RANGES };
  FunctionList list = {ranges, sizeof(ranges)/sizeof(ranges[0]), 0};
  FILE* f = fopen(argv[1], "w");
  if (!f) return 2;
  int ok = emit_lookup_indexed(f, &list);
  fclose(f); return ok ? 0 : 3;
}
''').replace('RANGES', rows))
subprocess.run(['cc', '-O2', '-I', str(source), str(generator), '-o', str(out/'emit')], check=True)
subprocess.run([str(out/'emit'), str(out/'lookup.inc')], check=True)
stubs = '\n'.join(f'static void func_{a:08X}(void* ctx) {{ (void)ctx; }}' for a,b in ranges)
fixture = out / 'verify.c'
fixture.write_text(textwrap.dedent('''\
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <pthread.h>
typedef uint32_t u32;
typedef uint64_t u64;
typedef void (*DolRecompFunction)(void*);
#define DOLRECOMP_UNUSED __attribute__((unused))
STUBS
#include "lookup.inc"
static const struct { u32 start, end; DolRecompFunction fn; } oracle[] = { ORACLE };
static DolRecompFunction expected(u32 pc) {
  if (pc & 3) return NULL;
  for (unsigned i=0; i<sizeof(oracle)/sizeof(oracle[0]); ++i)
    if (pc >= oracle[i].start && pc < oracle[i].end) return oracle[i].fn;
  return NULL;
}
static void check(u32 pc) {
  if (dolrecomp_find_original(pc) != expected(pc)) {
    fprintf(stderr, "wrong lookup at %08x\\n", pc); abort();
  }
}
static void* verify(void* unused) {
  (void)unused;
  for (u32 pc=0x2ff0; pc<END+32; ++pc) check(pc);
  u32 rng=123;
  for (unsigned i=0; i<200000; ++i) {
    rng=rng*1664525u+1013904223u;
    check(0x2000u+(rng % (END-0x1000u)));
  }
  for (u32 pc=0; pc<32; ++pc) check(pc);
  check(0xffffffffu); check(0x7e000000u);
  return NULL;
}
static double clock_ns(void) {
  struct timespec t; clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &t);
  return (double)t.tv_sec*1e9+t.tv_nsec;
}
int main(void) {
  pthread_t worker;
  if (pthread_create(&worker, NULL, verify, NULL)) return 1;
  verify(NULL); pthread_join(worker, NULL);
  DolRecompFunction volatile sink = NULL;
  const unsigned count=4000000;
  double start=clock_ns();
  for (unsigned i=0; i<count; ++i) sink=dolrecomp_find_original_uncached(oracle[i%40].start);
  double baseline=(clock_ns()-start)/count;
  start=clock_ns();
  for (unsigned i=0; i<count; ++i) sink=dolrecomp_find_original(oracle[i%40].start);
  double cached=(clock_ns()-start)/count;
  printf("lookup: exhaustive alignment/bounds/gaps + collisions + two threads passed; uncached %.3f ns, cached %.3f ns\\n", baseline, cached);
  return sink ? 0 : 1;
}
''').replace('STUBS', stubs).replace('ORACLE', ',\n'.join(f'{{0x{a:x}u,0x{b:x}u,func_{a:08X}}}' for a,b in ranges)).replace('END', f'0x{position:x}u'))
subprocess.run(['cc', '-std=c11', '-D_POSIX_C_SOURCE=200809L', '-O3', '-pthread', str(fixture), '-o', str(out/'verify')], check=True)
subprocess.run([str(out/'verify')], check=True)
