// SPDX-License-Identifier: GPL-3.0-or-later
#include "cpu/cpu.h"
#define PPC_MSR_EE 0x8000u
#define PPC_MSR_PR 0x4000u
#define PPC_MSR_POW 0x40000u
#define PPC_MSR_RFI_MASK 0x87c0ffffu
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%d: %s\n", __LINE__, #x); exit(1); } } while (0)
static unsigned queries;
static bool pending;
static bool query(CPUState* cpu, u32 token) {
    CHECK(token == 0xfffffff4u);
    CHECK(cpu->pc == 0x80012340u);
    CHECK(cpu->msr & PPC_MSR_EE);
    ++queries;
    if (pending && cpu->external_write_count < 255) ++cpu->external_write_count;
    return pending;
}
int main(void) {
    CPUState *cpu = calloc(1, sizeof(*cpu)), *expected = malloc(sizeof(*cpu));
    CHECK(cpu && expected);
    for (unsigned ee=0; ee<2; ++ee) for (unsigned host=0; host<2; ++host)
    for (unsigned irq=0; irq<2; ++irq) for (unsigned epoch=0; epoch<256; ++epoch) {
        memset(cpu, 0, sizeof(*cpu));
        cpu->pc=0x80004000; cpu->msr=0x00040032; cpu->srr0=0x80012343;
        cpu->srr1=0x2032 | (ee ? PPC_MSR_EE : 0);
        cpu->lr=0x80023000; cpu->gpr[1]=0x80034000; cpu->fpr[3]=1.25;
        cpu->external_write_count=epoch; cpu->host_call=host ? query : NULL;
        memcpy(expected, cpu, sizeof(*cpu));
        expected->msr=(cpu->msr & ~PPC_MSR_RFI_MASK) | (cpu->srr1 & PPC_MSR_RFI_MASK);
        expected->msr &= ~PPC_MSR_POW;
        expected->pc=cpu->srr0 & ~3u;
        if (ee && host && irq && epoch<255) ++expected->external_write_count;
        queries=0; pending=irq;
        ppc_rfi(cpu, 0x80004000);
        CHECK(queries == (ee && host));
        CHECK(memcmp(cpu, expected, sizeof(*cpu)) == 0);
    }
    // Privileged instruction fault must not restore context or call the host.
    memset(cpu, 0, sizeof(*cpu)); cpu->msr=PPC_MSR_PR;
    cpu->srr0=0x80012340; cpu->srr1=PPC_MSR_EE; cpu->host_call=query;
    queries=0; ppc_rfi(cpu, 0x80004000);
    CHECK(queries==0); CHECK(cpu->pc != 0x80012340);
    free(expected); free(cpu);
    puts("RFI: exact restored state, pending epoch, EE-off/null host, saturation and privilege passed");
    return 0;
}
