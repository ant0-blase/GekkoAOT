// SPDX-License-Identifier: GPL-3.0-or-later
#include "ir/dolir.h"
#include <stdlib.h>
#define DOLIR_STATE_GPR3 ((DolIRStateSlot)(DOLIR_STATE_GPR0 + 3))
#define DOLIR_STATE_GPR4 ((DolIRStateSlot)(DOLIR_STATE_GPR0 + 4))
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%d: %s\n", __LINE__, #x); exit(1); } } while (0)

static void check_loop(unsigned mode) {
    DolIRModule module;
    dolir_module_init(&module);
    DolIRFunction* f = dolir_add_function(&module, "rel-loop", 0x80500000, 0x80500010);
    for (unsigned i = 0; i < 4; ++i) CHECK(dolir_add_block(f, 0x80500000 + i * 4));
    DolIRValue base = dolir_constant(f, &f->blocks[0], DOLIR_TYPE_I32, 0x805a3c18, 0x80500000);
    CHECK(dolir_state_write(f, &f->blocks[0], DOLIR_STATE_GPR3, base, 0x80500000));
    f->blocks[0].terminator.kind = DOLIR_TERM_BRANCH;
    f->blocks[0].terminator.targets[0] = 2;
    if (mode == 1) {
        DolIRValue changed = dolir_constant(f, &f->blocks[1], DOLIR_TYPE_I32, 0x805a4000, 0x80500004);
        CHECK(dolir_state_write(f, &f->blocks[1], DOLIR_STATE_GPR3, changed, 0x80500004));
    }
    f->blocks[1].terminator.kind = DOLIR_TERM_BRANCH;
    f->blocks[1].terminator.targets[0] = 2;
    f->blocks[1].terminator.linked = mode == 2;
    DolIRValue address = dolir_state_read(f, &f->blocks[2], DOLIR_STATE_GPR3, 0x80500008);
    DolIRValue loaded = dolir_append(f, &f->blocks[2], DOLIR_OP_GUEST_LOAD,
                                    DOLIR_TYPE_I32, &address, 1, 0, 4, 0x80500008, 0);
    unsigned load_index = f->blocks[2].instruction_count - 1;
    // A pointer read from RAM stays live; numerical overlap with the synthetic
    // REL window cannot grant it static relocation provenance.
    dolir_append(f, &f->blocks[2], DOLIR_OP_GUEST_LOAD,
                 DOLIR_TYPE_I32, &loaded, 1, 0, 4, 0x80500008, 0);
    unsigned heap_index = f->blocks[2].instruction_count - 1;
    DolIRValue index = dolir_state_read(f, &f->blocks[2], DOLIR_STATE_GPR4, 0x80500008);
    DolIRValue operands[] = {address, index};
    DolIRValue indexed = dolir_append(f, &f->blocks[2], DOLIR_OP_ADD,
                                     DOLIR_TYPE_I32, operands, 2, 0, 0, 0x80500008, 0);
    dolir_append(f, &f->blocks[2], DOLIR_OP_GUEST_LOAD,
                 DOLIR_TYPE_I32, &indexed, 1, 0, 4, 0x80500008, 0);
    unsigned indexed_index = f->blocks[2].instruction_count - 1;
    f->blocks[2].terminator.kind = DOLIR_TERM_COND_BRANCH;
    f->blocks[2].terminator.targets[0] = 1;
    f->blocks[2].terminator.targets[1] = 3;
    f->blocks[3].terminator.kind = DOLIR_TERM_RETURN;
    dolir_analyze_addresses(f);
    const DolIRInstruction* load = &f->blocks[2].instructions[load_index];
    const DolIRInstruction* indexed_load = &f->blocks[2].instructions[indexed_index];
    if (mode == 0) {
        CHECK(load->address_domain == DOLIR_ADDRESS_MEM1);
        CHECK(load->address_lower == 0x805a3c18 && load->address_upper == 0x805a3c18);
        CHECK(indexed_load->address_domain == DOLIR_ADDRESS_MEM1);
        CHECK(indexed_load->address_lower == 0x805a3c18 && indexed_load->address_upper == 0xffffffff);
    } else {
        CHECK(load->address_domain == DOLIR_ADDRESS_UNKNOWN);
        CHECK(indexed_load->address_domain == DOLIR_ADDRESS_UNKNOWN);
    }
    CHECK(f->blocks[2].instructions[heap_index].address_domain == DOLIR_ADDRESS_UNKNOWN);
    dolir_module_free(&module);
}

int main(void) {
    setenv("DOLRECOMP_REL_RUNTIME_TRANSLATE", "1", 1);
    for (unsigned mode = 0; mode < 3; ++mode) check_loop(mode);
    puts("REL CFG address regression passed");
    return 0;
}
