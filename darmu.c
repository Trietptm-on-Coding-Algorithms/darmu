#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "darm.h"
#include "darmu.h"

void darmu_init(darmu_t *d, uint8_t *stack, uint32_t stack_size)
{
    memset(d, 0, sizeof(darmu_t));

    d->regs[SP] = 0xb00b0000;
    darmu_mapping_add(d, stack, stack_size, d->regs[SP] - stack_size);
}

int darmu_mapping_add(darmu_t *d, uint8_t *image, uint32_t raw_size,
    uint32_t address)
{
    if(d->mapping_count == DARMU_MAPPINGS_COUNT) return -1;

    d->mappings[d->mapping_count].image = image;
    d->mappings[d->mapping_count].raw_size = raw_size;
    d->mappings[d->mapping_count].address = address;
    d->mapping_count++;
    return 0;
}

uint32_t darmu_mapping_lookup_virtual(const darmu_t *d, uint8_t *raw)
{
    for (uint32_t i = 0; i < d->mapping_count; i++) {
        const darmu_mapping_t *m = &d->mappings[i];
        if(raw >= m->image && raw < m->image + m->raw_size) {
            return raw - m->image + m->address;
        }
    }
    return 0;
}

uint32_t *darmu_mapping_lookup_raw(const darmu_t *d, uint32_t address)
{
    for (uint32_t i = 0; i < d->mapping_count; i++) {
        const darmu_mapping_t *m = &d->mappings[i];
        if(address >= m->address && address < m->address + m->raw_size) {
            return (uint32_t *) &m->image[address - m->address];
        }
    }
    fprintf(stderr, "Invalid virtual address: 0x%08x\n", address);
    fflush(stderr);

    static uint32_t null;
    return &null;
}

uint32_t darmu_register_get(darmu_t *d, uint32_t idx)
{
    return idx < 16 ? d->regs[idx] : 0;
}

void darmu_register_set(darmu_t *d, uint32_t idx, uint32_t value)
{
    if(idx < 16) {
        d->regs[idx] = value;
    }
}

uint32_t darmu_flags_get(darmu_t *d)
{
    uint32_t value;
    memcpy(&value, &d->flags, sizeof(uint32_t));
    return value;
}

void darmu_flags_set(darmu_t *d, uint32_t value)
{
    memcpy(&d->flags, &value, sizeof(uint32_t));
}

extern void (*g_handlers[I_INSTRCNT])(darmu_t *du, const darm_t *d);

int darmu_single_step(darmu_t *du)
{
    darm_t d;

    // calculate the raw offset of the program counter
    uint32_t opcode = darmu_read32(du, du->regs[PC]);

    // disassemble the instruction
    int ret = darm_armv7_disasm(&d, opcode);
    if(ret < 0) {
        fprintf(stderr, "Invalid instruction.. 0x%08x\n", opcode);
        return ret;
    }

    uint32_t pc = du->regs[PC];

    int exec = 0;
    switch (d.cond) {
    case C_EQ: exec = du->flags.Z == 1; break;
    case C_NE: exec = du->flags.Z == 0; break;
    case C_CS: exec = du->flags.C == 1; break;
    case C_CC: exec = du->flags.C == 0; break;
    case C_MI: exec = du->flags.N == 1; break;
    case C_PL: exec = du->flags.N == 0; break;
    case C_VS: exec = du->flags.V == 1; break;
    case C_VC: exec = du->flags.V == 0; break;
    case C_HI: exec = du->flags.C == 1 && du->flags.Z == 0; break;
    case C_LS: exec = du->flags.C == 0 || du->flags.Z == 1; break;
    case C_GE: exec = du->flags.N == du->flags.V; break;
    case C_LT: exec = du->flags.N != du->flags.V; break;
    case C_GT: exec = du->flags.Z == 0 && du->flags.N == du->flags.V; break;
    case C_LE: exec = du->flags.Z == 1 || du->flags.N != du->flags.V; break;
    case C_AL: case C_UNCOND: exec = 1; break;
    case C_INVLD:
        fprintf(stderr, "Can't handle C_INVLD!\n");
        return -1;
    }

    if(g_handlers[d.instr] == NULL) {
        darm_str_t str;
        darm_str(&d, &str);
        fprintf(stderr, "[-] instruction '%s' unhandled!\n", str.instr);
        return -1;
    }
    else if(exec == 1) {
        g_handlers[d.instr](du, &d);
    }

    // increase the program counter if it hasn't been altered
    if(pc == du->regs[PC]) {
        du->regs[PC] += 4;
    }

    return 0;
}

uint8_t darmu_read8(const darmu_t *d, uint32_t addr)
{
    return *(uint8_t *) darmu_mapping_lookup_raw(d, addr);
}

uint16_t darmu_read16(const darmu_t *d, uint32_t addr)
{
    return *(uint16_t *) darmu_mapping_lookup_raw(d, addr);
}

uint32_t darmu_read32(const darmu_t *d, uint32_t addr)
{
    return *darmu_mapping_lookup_raw(d, addr);
}

void darmu_write8(const darmu_t *d, uint32_t addr, uint8_t value)
{
    *(uint8_t *) darmu_mapping_lookup_raw(d, addr) = value;
}

void darmu_write16(const darmu_t *d, uint32_t addr, uint16_t value)
{
    *(uint16_t *) darmu_mapping_lookup_raw(d, addr) = value;
}

void darmu_write32(const darmu_t *d, uint32_t addr, uint32_t value)
{
    *darmu_mapping_lookup_raw(d, addr) = value;
}

uint32_t darmu_apply_shift(const darm_t *d, uint32_t value, uint32_t shift,
    uint32_t *carry_out)
{
    uint64_t result = 0;
    switch (d->shift_type) {
    case S_INVLD:
        result = value;
        *carry_out = 0;
        break;

    case S_LSL:
        result = LSL(value, shift);
        *carry_out = (result >> 32) & 1;
        break;

    case S_LSR:
        result = LSR(value, shift);
        *carry_out = LSR(value, shift-1) & 1;
        break;

    case S_ASR:
        result = ASR(value, shift);
        *carry_out = ASR(value, shift-1) & 1;
        break;

    case S_ROR:
        result = ROR(value, shift);
        *carry_out = (result >> 31) & 1;
        break;
    }
    return (uint32_t) result;
}

uint32_t darmu_get_offset(const darm_t *d, darm_reg_t shift_register,
    uint32_t value, uint32_t shift, uint32_t *carry_out)
{
    if(d->I == B_SET) return d->imm;

    return darmu_apply_shift(d, value,
        shift_register != R_INVLD ? shift : d->shift, carry_out);
}
