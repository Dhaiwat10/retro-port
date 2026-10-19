/* A small x86 interpreter for code that only exists at run time.
 *
 * EA's AEMS sound system ships "modules" as x86 code inside .abk sound banks; the game relocates
 * them into heap memory and calls them through function pointers. The code is short glue
 * (mov/push/call/add/imul/ret) that calls back into the exe's component functions, so an
 * interpreter is plenty fast. It works on the same CPU struct as the recompiled code, and calls
 * out of the dynamic region go through the normal native dispatch.
 *
 * Regions are registered when the game marks memory executable (VirtualProtect); an indirect
 * call to an unknown address outside every region still traps. */
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>

#define MAX_REGIONS 1024
static struct { uint32_t lo, hi; } g_regions[MAX_REGIONS];
static volatile int g_nregions;

void dyncode_register(uint32_t addr, uint32_t len) {
    for (int i = 0; i < g_nregions; i++)
        if (g_regions[i].lo == addr) { g_regions[i].hi = addr + len; return; }
    if (g_nregions >= MAX_REGIONS) return;
    g_regions[g_nregions].lo = addr;
    g_regions[g_nregions].hi = addr + len;
    __atomic_add_fetch(&g_nregions, 1, __ATOMIC_RELEASE);
}

int dyncode_contains(uint32_t addr) {
    if (addr < 0x00a4d000u) return 0;  /* never the image itself */
    int n = __atomic_load_n(&g_nregions, __ATOMIC_ACQUIRE);
    for (int i = 0; i < n; i++)
        if (addr >= g_regions[i].lo && addr < g_regions[i].hi) return 1;
    return 0;
}

static void bad(CPU *c, uint32_t eip, const char *why) {
    char msg[160];
    snprintf(msg, sizeof msg, "interpreter: %s (bytes %02x %02x %02x %02x %02x %02x)", why, R8(eip), R8(eip + 1),
             R8(eip + 2), R8(eip + 3), R8(eip + 4), R8(eip + 5));
    guest_trap(c, eip, msg);
}

static int cond(CPU *c, int cc) {
    int r;
    switch (cc >> 1) {
    case 0: r = c->of; break;
    case 1: r = c->cf; break;
    case 2: r = c->zf; break;
    case 3: r = c->cf || c->zf; break;
    case 4: r = c->sf; break;
    case 5: r = PF(c); break;
    case 6: r = c->sf != c->of; break;
    default: r = c->zf || c->sf != c->of; break;
    }
    return (cc & 1) ? !r : r;
}

/* Decoded ModRM operand: either a register number or a memory address. */
typedef struct { int is_reg, reg, rm; uint32_t addr; } ModRM;

static uint32_t *REG(CPU *c, int r) { return &c->eax + r; }

static uint32_t decode_modrm(CPU *c, uint32_t p, ModRM *m) {
    uint8_t b = R8(p++);
    int mod = b >> 6;
    m->reg = (b >> 3) & 7;
    m->rm = b & 7;
    m->is_reg = mod == 3;
    if (m->is_reg) return p;
    uint32_t a = 0;
    if (m->rm == 4) {
        uint8_t sib = R8(p++);
        int scale = sib >> 6, idx = (sib >> 3) & 7, base = sib & 7;
        if (base == 5 && mod == 0) { a = R32(p); p += 4; }
        else a = *REG(c, base);
        if (idx != 4) a += *REG(c, idx) << scale;
    } else if (m->rm == 5 && mod == 0) {
        a = R32(p); p += 4;
    } else {
        a = *REG(c, m->rm);
    }
    if (mod == 1) { a += (uint32_t)(int32_t)(int8_t)R8(p); p += 1; }
    else if (mod == 2) { a += R32(p); p += 4; }
    m->addr = a;
    return p;
}

static uint32_t rd32(CPU *c, const ModRM *m) { return m->is_reg ? *REG(c, m->rm) : R32(m->addr); }
static void wr32(CPU *c, const ModRM *m, uint32_t v) { if (m->is_reg) *REG(c, m->rm) = v; else W32(m->addr, v); }
static uint8_t rd8(CPU *c, const ModRM *m) {
    if (!m->is_reg) return R8(m->addr);
    uint32_t r = *REG(c, m->rm & 3);
    return (uint8_t)(m->rm & 4 ? r >> 8 : r);
}
static uint8_t reg8(CPU *c, int r) { uint32_t v = *REG(c, r & 3); return (uint8_t)(r & 4 ? v >> 8 : v); }
static void setreg8(CPU *c, int r, uint8_t v) {
    uint32_t *p = REG(c, r & 3);
    *p = r & 4 ? (*p & 0xffff00ffu) | (uint32_t)v << 8 : (*p & 0xffffff00u) | v;
}
static void wr8(CPU *c, const ModRM *m, uint8_t v) { if (m->is_reg) setreg8(c, m->rm, v); else W8(m->addr, v); }

static uint32_t alu32(CPU *c, int op, uint32_t a, uint32_t b) {
    switch (op) {
    case 0: return add32(c, a, b);
    case 1: return logic32(c, a | b);
    case 2: return adc32(c, a, b);
    case 3: return sbb32(c, a, b);
    case 4: return logic32(c, a & b);
    case 5: return sub32(c, a, b);
    case 6: return logic32(c, a ^ b);
    default: sub32(c, a, b); return a;  /* cmp */
    }
}
static uint8_t alu8(CPU *c, int op, uint8_t a, uint8_t b) {
    switch (op) {
    case 0: return add8(c, a, b);
    case 1: return logic8(c, a | b);
    case 2: return adc8(c, a, b);
    case 3: return sbb8(c, a, b);
    case 4: return logic8(c, a & b);
    case 5: return sub8(c, a, b);
    case 6: return logic8(c, a ^ b);
    default: sub8(c, a, b); return a;
    }
}
static uint32_t imul_flags(CPU *c, int32_t a, int32_t b) {
    int64_t r = (int64_t)a * b;
    c->cf = c->of = r != (int32_t)r;
    return (uint32_t)r;
}
static uint32_t shift32(CPU *c, int op, uint32_t a, unsigned n, uint32_t eip) {
    n &= 31;
    if (!n) return a;
    switch (op) {
    case 0: return rol32(c, a, n);
    case 1: return ror32(c, a, n);
    case 4: case 6: return shl32(c, a, n);
    case 5: return shr32(c, a, n);
    case 7: return sar32(c, a, n);
    default: bad(c, eip, "unsupported shift"); return a;
    }
}

/* Runs guest code at `eip`; the caller has already pushed the return address (as for every
 * recompiled function). Returns once that frame's `ret` executes. */
void interp_call(CPU *c, uint32_t eip) {
    const uint32_t esp0 = c->esp;
    for (;;) {
        uint32_t start = eip;
        uint8_t op = R8(eip++);
        ModRM m;
        if (op < 0x40 && (op & 7) < 6) {  /* add/or/adc/sbb/and/sub/xor/cmp */
            int alu = op >> 3;
            switch (op & 7) {
            case 0: eip = decode_modrm(c, eip, &m); wr8(c, &m, alu8(c, alu, rd8(c, &m), reg8(c, m.reg))); break;
            case 1: eip = decode_modrm(c, eip, &m); { uint32_t r = alu32(c, alu, rd32(c, &m), *REG(c, m.reg)); if (alu != 7) wr32(c, &m, r); } break;
            case 2: eip = decode_modrm(c, eip, &m); setreg8(c, m.reg, alu8(c, alu, reg8(c, m.reg), rd8(c, &m))); break;
            case 3: eip = decode_modrm(c, eip, &m); *REG(c, m.reg) = alu32(c, alu, *REG(c, m.reg), rd32(c, &m)); break;
            case 4: setreg8(c, 0, alu8(c, alu, (uint8_t)c->eax, R8(eip))); eip += 1; break;
            case 5: c->eax = alu32(c, alu, c->eax, R32(eip)); eip += 4; break;
            }
            continue;
        }
        if (op >= 0x40 && op < 0x48) { *REG(c, op & 7) = inc32(c, *REG(c, op & 7)); continue; }
        if (op >= 0x48 && op < 0x50) { *REG(c, op & 7) = dec32(c, *REG(c, op & 7)); continue; }
        if (op >= 0x50 && op < 0x58) { PUSH32(c, *REG(c, op & 7)); continue; }
        if (op >= 0x58 && op < 0x60) { uint32_t v = POP32(c); *REG(c, op & 7) = v; continue; }
        if (op >= 0x70 && op < 0x80) {
            int8_t d = (int8_t)R8(eip++);
            if (cond(c, op & 15)) eip += (uint32_t)(int32_t)d;
            continue;
        }
        if (op >= 0xb0 && op < 0xb8) { setreg8(c, op & 7, R8(eip)); eip += 1; continue; }
        if (op >= 0xb8 && op < 0xc0) { *REG(c, op & 7) = R32(eip); eip += 4; continue; }
        switch (op) {
        case 0x0f: {
            uint8_t op2 = R8(eip++);
            if (op2 >= 0x80 && op2 < 0x90) {
                uint32_t d = R32(eip); eip += 4;
                if (cond(c, op2 & 15)) eip += d;
            } else if (op2 == 0xaf) {
                eip = decode_modrm(c, eip, &m);
                *REG(c, m.reg) = imul_flags(c, (int32_t)*REG(c, m.reg), (int32_t)rd32(c, &m));
            } else if (op2 == 0xb6 || op2 == 0xbe) {
                eip = decode_modrm(c, eip, &m);
                uint8_t v = rd8(c, &m);
                *REG(c, m.reg) = op2 == 0xb6 ? v : (uint32_t)(int32_t)(int8_t)v;
            } else if (op2 == 0xb7 || op2 == 0xbf) {
                eip = decode_modrm(c, eip, &m);
                uint16_t v = m.is_reg ? (uint16_t)*REG(c, m.rm) : R16(m.addr);
                *REG(c, m.reg) = op2 == 0xb7 ? v : (uint32_t)(int32_t)(int16_t)v;
            } else bad(c, start, "unsupported 0f opcode");
            break;
        }
        case 0x68: PUSH32(c, R32(eip)); eip += 4; break;
        case 0x6a: PUSH32(c, (uint32_t)(int32_t)(int8_t)R8(eip)); eip += 1; break;
        case 0x69: case 0x6b: {
            eip = decode_modrm(c, eip, &m);
            int32_t imm;
            if (op == 0x69) { imm = (int32_t)R32(eip); eip += 4; } else { imm = (int8_t)R8(eip); eip += 1; }
            *REG(c, m.reg) = imul_flags(c, (int32_t)rd32(c, &m), imm);
            break;
        }
        case 0x80: case 0x81: case 0x83: {
            eip = decode_modrm(c, eip, &m);
            if (op == 0x80) { uint8_t imm = R8(eip); eip += 1; uint8_t r = alu8(c, m.reg, rd8(c, &m), imm); if (m.reg != 7) wr8(c, &m, r); break; }
            uint32_t imm;
            if (op == 0x81) { imm = R32(eip); eip += 4; } else { imm = (uint32_t)(int32_t)(int8_t)R8(eip); eip += 1; }
            uint32_t r = alu32(c, m.reg, rd32(c, &m), imm);
            if (m.reg != 7) wr32(c, &m, r);
            break;
        }
        case 0x84: eip = decode_modrm(c, eip, &m); logic8(c, rd8(c, &m) & reg8(c, m.reg)); break;
        case 0x85: eip = decode_modrm(c, eip, &m); logic32(c, rd32(c, &m) & *REG(c, m.reg)); break;
        case 0x88: eip = decode_modrm(c, eip, &m); wr8(c, &m, reg8(c, m.reg)); break;
        case 0x89: eip = decode_modrm(c, eip, &m); wr32(c, &m, *REG(c, m.reg)); break;
        case 0x8a: eip = decode_modrm(c, eip, &m); setreg8(c, m.reg, rd8(c, &m)); break;
        case 0x8b: eip = decode_modrm(c, eip, &m); *REG(c, m.reg) = rd32(c, &m); break;
        case 0x8d: eip = decode_modrm(c, eip, &m); if (m.is_reg) bad(c, start, "lea with register"); *REG(c, m.reg) = m.addr; break;
        case 0x90: break;
        case 0x99: c->edx = (int32_t)c->eax < 0 ? 0xffffffffu : 0; break;
        case 0xc1: case 0xd1: case 0xd3: {
            eip = decode_modrm(c, eip, &m);
            unsigned n = op == 0xd1 ? 1 : op == 0xd3 ? (c->ecx & 0xff) : R8(eip++);
            wr32(c, &m, shift32(c, m.reg, rd32(c, &m), n, start));
            break;
        }
        case 0xc6: eip = decode_modrm(c, eip, &m); wr8(c, &m, R8(eip)); eip += 1; break;
        case 0xc7: eip = decode_modrm(c, eip, &m); wr32(c, &m, R32(eip)); eip += 4; break;
        case 0xc2: case 0xc3: {
            uint32_t ret = POP32(c);
            if (op == 0xc2) { c->esp += R16(eip); }
            if (c->esp > esp0) return;   /* the frame we were called with */
            eip = ret;
            if (!dyncode_contains(eip)) bad(c, start, "internal return to non-dynamic code");
            break;
        }
        case 0xe8: {
            uint32_t target = eip + 4 + R32(eip);
            eip += 4;
            PUSH32(c, eip);
            if (dyncode_contains(target)) eip = target;    /* stay in the interpreter */
            else call_indirect(c, target);                 /* native code pops the return address */
            break;
        }
        case 0xe9: eip = eip + 4 + R32(eip); if (!dyncode_contains(eip)) bad(c, start, "jmp out of dynamic code"); break;
        case 0xeb: eip = eip + 1 + (uint32_t)(int32_t)(int8_t)R8(eip); break;
        case 0xff: {
            eip = decode_modrm(c, eip, &m);
            switch (m.reg) {
            case 0: wr32(c, &m, inc32(c, rd32(c, &m))); break;
            case 1: wr32(c, &m, dec32(c, rd32(c, &m))); break;
            case 2: {
                uint32_t target = rd32(c, &m);
                PUSH32(c, eip);
                if (dyncode_contains(target)) eip = target;
                else call_indirect(c, target);
                break;
            }
            case 6: PUSH32(c, rd32(c, &m)); break;
            default: bad(c, start, "unsupported ff group op");
            }
            break;
        }
        default: bad(c, start, "unsupported opcode");
        }
    }
}
