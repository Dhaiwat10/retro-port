/* Direct3D 8 vertex shaders (vs.1.1) and vertex declarations, run on the CPU.
 *
 * The renderer already converts every vertex on the CPU, so programmable vertex processing is done
 * the same way: the declaration says how stream data maps onto input registers v0-v15, the shader
 * runs per vertex, and its outputs (oPos in clip space, oD0/oD1, oT0-oT7) feed the fixed-function
 * pixel stage. Each shader is decoded once into a compact instruction list. */
#include "vshader.h"

#include <math.h>
#include <stdlib.h>

enum { RT_TEMP = 0, RT_INPUT = 1, RT_CONST = 2, RT_ADDR = 3, RT_RASTOUT = 4, RT_ATTROUT = 5, RT_TEXOUT = 6 };

typedef struct { uint8_t type, rel, neg, swz[4]; uint16_t num; } Src;
typedef struct { uint16_t op; uint8_t dtype, dmask; uint16_t dnum; uint8_t nsrc; Src s[3]; } Insn;

typedef struct {
    Insn *code; uint32_t n;
    /* declaration: input register sources */
    struct { uint8_t used, stream, type; uint16_t offset; } in[16];
    float defs[96][4]; uint8_t has_def[96];        /* constants from the declaration and `def` */
    int fixed_function;                            /* declaration without a program */
    int ok;
} VsProgram;

/* operand count (destination + sources) per vs.1.1 opcode */
static int nparams(uint32_t op) {
    switch (op) {
    case 0: return 0;
    case 1: case 6: case 7: case 14: case 15: case 16: case 19: case 78: case 79: return 2;
    case 2: case 3: case 5: case 8: case 9: case 10: case 11: case 12: case 13: case 17:
    case 20: case 21: case 22: case 23: case 24: return 3;
    case 4: case 18: return 4;
    default: return -1;
    }
}

static void parse_decl(VsProgram *p, const uint32_t *t, uint32_t n) {
    uint32_t stream = 0, offset = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t tok = t[i];
        if (tok == 0xFFFFFFFFu) break;
        switch (tok >> 29) {
        case 1: stream = tok & 0xF; offset = 0; break;               /* D3DVSD_STREAM */
        case 2:
            if (tok & 0x10000000u) offset += 4 * ((tok >> 16) & 0xF);   /* D3DVSD_SKIP */
            else {
                uint32_t reg = tok & 0x1F, type = (tok >> 16) & 0xF;
                if (reg < 16) { p->in[reg].used = 1; p->in[reg].stream = (uint8_t)stream; p->in[reg].type = (uint8_t)type;
                                p->in[reg].offset = (uint16_t)offset; }
                static const uint8_t sz[8] = { 4, 8, 12, 16, 4, 4, 4, 8 };
                offset += type < 8 ? sz[type] : 4;
            }
            break;
        case 4: {                                                    /* D3DVSD_CONST */
            uint32_t cnt = (tok >> 25) & 0xF, addr = tok & 0x7F;
            for (uint32_t k = 0; k < cnt && i + 4 <= n; k++, addr++) {
                if (addr < 96) { memcpy(p->defs[addr], &t[i + 1], 16); p->has_def[addr] = 1; }
                i += 4;
            }
            break;
        }
        case 5: i += (tok >> 24) & 0x1F; break;                     /* D3DVSD_EXT */
        default: break;
        }
    }
}

static Src decode_src(uint32_t t) {
    Src s;
    s.type = (uint8_t)((t >> 28) & 7);
    s.num = (uint16_t)(t & 0x1FFF);
    s.rel = (t >> 13) & 1;
    s.neg = ((t >> 24) & 0xF) == 1;
    for (int k = 0; k < 4; k++) s.swz[k] = (uint8_t)((t >> (16 + 2 * k)) & 3);
    return s;
}

static int parse_code(VsProgram *p, const uint32_t *t, uint32_t n) {
    if (!n || (t[0] & 0xFFFF0000u) != 0xFFFE0000u) return 0;
    p->code = calloc(n, sizeof(Insn));
    for (uint32_t i = 1; i < n;) {
        uint32_t tok = t[i++], op = tok & 0xFFFF;
        if (tok == 0x0000FFFFu) break;
        if (op == 0xFFFE) { i += (tok >> 16) & 0x7FFF; continue; }   /* comment */
        if (op == 81) {                                             /* def c#, f, f, f, f */
            uint32_t r = t[i] & 0x1FFF;
            if (r < 96) { memcpy(p->defs[r], &t[i + 1], 16); p->has_def[r] = 1; }
            i += 5;
            continue;
        }
        int np = nparams(op);
        if (np < 0) { fprintf(stderr, "[vs] unsupported opcode %u\n", op); return 0; }
        if (np == 0) continue;
        Insn *in = &p->code[p->n++];
        in->op = (uint16_t)op;
        uint32_t d = t[i++];
        in->dtype = (uint8_t)((d >> 28) & 7);
        in->dnum = (uint16_t)(d & 0x1FFF);
        in->dmask = (uint8_t)((d >> 16) & 0xF);
        in->nsrc = (uint8_t)(np - 1);
        for (int k = 0; k < np - 1; k++) {
            uint32_t s = t[i++];
            in->s[k] = decode_src(s);
            if (in->s[k].rel && (t[0] & 0xFFFF) >= 0x0200) i++;     /* vs.2.0 relative token (not expected) */
        }
    }
    return 1;
}

static VsProgram *program_for(Shader *sh) {
    if (sh->backend) return sh->backend;
    VsProgram *p = calloc(1, sizeof *p);
    parse_decl(p, sh->decl, sh->decl_len);
    if (!sh->code) { p->fixed_function = 1; p->ok = 1; }
    else p->ok = parse_code(p, sh->code, sh->code_len);
    if (!p->ok) fprintf(stderr, "[vs] could not decode shader (version %08x)\n", sh->code ? sh->code[0] : 0);
    if (getenv("PORT_DUMP_VS")) {   /* raw tokens, for offline disassembly */
        fprintf(stderr, "[vsdump %p] decl", (void *)sh);
        for (uint32_t i = 0; i < sh->decl_len; i++) fprintf(stderr, " %08x", sh->decl[i]);
        fprintf(stderr, "\n[vsdump %p] code", (void *)sh);
        for (uint32_t i = 0; i < sh->code_len; i++) fprintf(stderr, " %08x", sh->code[i]);
        fprintf(stderr, "\n");
    }
    sh->backend = p;
    return p;
}

int vs_is_fixed_function(Shader *sh) { return program_for(sh)->fixed_function; }

/* ------------------------------------------------------------------ input fetch */

static void fetch(const uint8_t *src, uint32_t type, float *o) {
    o[0] = o[1] = o[2] = 0; o[3] = 1;
    switch (type) {
    case 0: memcpy(o, src, 4); break;
    case 1: memcpy(o, src, 8); break;
    case 2: memcpy(o, src, 12); break;
    case 3: memcpy(o, src, 16); break;
    case 4: o[0] = src[2] / 255.f; o[1] = src[1] / 255.f; o[2] = src[0] / 255.f; o[3] = src[3] / 255.f; break;  /* D3DCOLOR */
    case 5: for (int k = 0; k < 4; k++) o[k] = src[k]; break;                                                 /* UBYTE4 */
    case 6: { int16_t s[2]; memcpy(s, src, 4); o[0] = s[0]; o[1] = s[1]; break; }                           /* SHORT2 */
    case 7: { int16_t s[4]; memcpy(s, src, 8); for (int k = 0; k < 4; k++) o[k] = s[k]; break; }            /* SHORT4 */
    }
}

/* ------------------------------------------------------------------ execution */

static inline void read_src(const Src *s, float r[][4], const float v[16][4], const float c[96][4], int a0,
                            float *o) {
    const float *base;
    static const float zero[4];
    switch (s->type) {
    case RT_TEMP: base = s->num < 12 ? r[s->num] : zero; break;
    case RT_INPUT: base = s->num < 16 ? v[s->num] : zero; break;
    case RT_CONST: { int i = s->num + (s->rel ? a0 : 0); base = i >= 0 && i < 96 ? c[i] : zero; break; }
    default: base = zero; break;
    }
    for (int k = 0; k < 4; k++) o[k] = s->neg ? -base[s->swz[k]] : base[s->swz[k]];
}

static inline float dp3(const float *a, const float *b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
static inline float dp4(const float *a, const float *b) { return dp3(a, b) + a[3] * b[3]; }

int vs_trace;   /* set by the renderer to trace one vertex */

void vs_run(Shader *sh, const float consts[96][4], const VsStreams *st, uint32_t index, VsOut *out) {
    VsProgram *p = program_for(sh);
    float v[16][4];
    for (int i = 0; i < 16; i++) {
        if (!p->in[i].used || !st->data[p->in[i].stream]) { v[i][0] = v[i][1] = v[i][2] = 0; v[i][3] = 1; continue; }
        const uint8_t *src = st->data[p->in[i].stream] + (size_t)index * st->stride[p->in[i].stream] + p->in[i].offset;
        fetch(src, p->in[i].type, v[i]);
    }
    if (p->fixed_function) {
        /* declaration-only shader: D3D's fixed register mapping (v0 position, v5/v6 colors, v7+ texcoords) */
        const float *m = st->ff_mvp;
        const float *x = v[0];
        for (int k = 0; k < 4; k++) out->pos[k] = x[0] * m[k] + x[1] * m[4 + k] + x[2] * m[8 + k] + m[12 + k];
        if (p->in[5].used) memcpy(out->d0, v[5], 16); else for (int k = 0; k < 4; k++) out->d0[k] = 1;
        if (p->in[6].used) memcpy(out->d1, v[6], 16); else memset(out->d1, 0, 16);
        for (int t = 0; t < 4; t++) memcpy(out->tex[t], v[7 + t], 16);
        return;
    }
    if (!p->ok) { memset(out, 0, sizeof *out); return; }

    /* constants: declaration/def values override nothing set later by SetVertexShaderConstant, but D3D
       applies declaration constants at SetVertexShader time; using them as a base is close enough */
    float r[12][4] = { { 0 } };
    float o_pos[4] = { 0, 0, 0, 1 }, o_d[2][4] = { { 1, 1, 1, 1 }, { 0, 0, 0, 0 } }, o_t[8][4];
    for (int t = 0; t < 8; t++) { o_t[t][0] = o_t[t][1] = o_t[t][2] = 0; o_t[t][3] = 1; }
    float dummy[4];
    int a0 = 0;
    const float (*c)[4] = consts;
    float cbuf[96][4];
    int any_def = 0;
    for (int i = 0; i < 96 && !any_def; i++) any_def = p->has_def[i];
    if (any_def) {
        memcpy(cbuf, consts, sizeof cbuf);
        for (int i = 0; i < 96; i++) if (p->has_def[i]) memcpy(cbuf[i], p->defs[i], 16);
        c = (const float (*)[4])cbuf;
    }

    for (uint32_t i = 0; i < p->n; i++) {
        const Insn *in = &p->code[i];
        float s0[4], s1[4], s2[4], res[4] = { 0, 0, 0, 0 };
        if (in->nsrc > 0) read_src(&in->s[0], r, (const float (*)[4])v, c, a0, s0);
        if (in->nsrc > 1) read_src(&in->s[1], r, (const float (*)[4])v, c, a0, s1);
        if (in->nsrc > 2) read_src(&in->s[2], r, (const float (*)[4])v, c, a0, s2);
        switch (in->op) {
        case 1: memcpy(res, s0, 16); break;                                          /* mov */
        case 2: for (int k = 0; k < 4; k++) res[k] = s0[k] + s1[k]; break;          /* add */
        case 3: for (int k = 0; k < 4; k++) res[k] = s0[k] - s1[k]; break;          /* sub */
        case 4: for (int k = 0; k < 4; k++) res[k] = s0[k] * s1[k] + s2[k]; break;  /* mad */
        case 5: for (int k = 0; k < 4; k++) res[k] = s0[k] * s1[k]; break;          /* mul */
        case 6: { float x = s0[3]; float y = x == 1.f ? 1.f : x == 0.f ? INFINITY : 1.f / x;   /* rcp */
                  for (int k = 0; k < 4; k++) res[k] = y; break; }
        case 7: { float x = fabsf(s0[3]); float y = x == 1.f ? 1.f : x == 0.f ? INFINITY : 1.f / sqrtf(x);  /* rsq */
                  for (int k = 0; k < 4; k++) res[k] = y; break; }
        case 8: { float d = dp3(s0, s1); for (int k = 0; k < 4; k++) res[k] = d; break; }
        case 9: { float d = dp4(s0, s1); for (int k = 0; k < 4; k++) res[k] = d; break; }
        case 10: for (int k = 0; k < 4; k++) res[k] = s0[k] < s1[k] ? s0[k] : s1[k]; break;
        case 11: for (int k = 0; k < 4; k++) res[k] = s0[k] >= s1[k] ? s0[k] : s1[k]; break;
        case 12: for (int k = 0; k < 4; k++) res[k] = s0[k] < s1[k] ? 1.f : 0.f; break;   /* slt */
        case 13: for (int k = 0; k < 4; k++) res[k] = s0[k] >= s1[k] ? 1.f : 0.f; break;  /* sge */
        case 14: { float y = exp2f(s0[3]); for (int k = 0; k < 4; k++) res[k] = y; break; }   /* exp */
        case 15: { float x = fabsf(s0[3]); float y = x == 0.f ? -INFINITY : log2f(x);         /* log */
                   for (int k = 0; k < 4; k++) res[k] = y; break; }
        case 16: {                                                                   /* lit */
            float pw = s0[3] < -128.f ? -128.f : s0[3] > 128.f ? 128.f : s0[3];
            res[0] = 1; res[3] = 1;
            res[1] = s0[0] > 0 ? s0[0] : 0;
            res[2] = s0[0] > 0 && s0[1] > 0 ? powf(s0[1], pw) : 0;
            break;
        }
        case 17: res[0] = 1; res[1] = s0[1] * s1[1]; res[2] = s0[2]; res[3] = s1[3]; break;   /* dst */
        case 18: for (int k = 0; k < 4; k++) res[k] = s0[k] * (s1[k] - s2[k]) + s2[k]; break; /* lrp */
        case 19: for (int k = 0; k < 4; k++) res[k] = s0[k] - floorf(s0[k]); break;           /* frc */
        case 20: case 21: case 22: case 23: case 24: {                               /* m4x4 m4x3 m3x4 m3x3 m3x2 */
            int rows = in->op == 20 || in->op == 22 ? 4 : in->op == 24 ? 2 : 3, four = in->op <= 21;
            Src row = in->s[1];
            for (int k = 0; k < rows; k++) {
                float m[4];
                read_src(&row, r, (const float (*)[4])v, c, a0, m);
                res[k] = four ? dp4(s0, m) : dp3(s0, m);
                row.num++;
            }
            break;
        }
        case 78: { float w = s0[3]; float f = floorf(w);                             /* expp */
                   res[0] = exp2f(f); res[1] = w - f; res[2] = exp2f(w); res[3] = 1; break; }
        case 79: { float x = fabsf(s0[3]);                                           /* logp */
                   if (x == 0.f) { res[0] = res[2] = -INFINITY; res[1] = 1; }
                   else { int e; float m = frexpf(x, &e); res[0] = (float)(e - 1); res[1] = m * 2; res[2] = log2f(x); }
                   res[3] = 1; break; }
        default: break;
        }
        float *dst;
        switch (in->dtype) {
        case RT_TEMP: dst = in->dnum < 12 ? r[in->dnum] : dummy; break;
        case RT_ADDR: if (in->dmask & 1) a0 = (int)floorf(res[0]); continue;
        case RT_RASTOUT: dst = in->dnum == 0 ? o_pos : dummy; break;
        case RT_ATTROUT: dst = in->dnum < 2 ? o_d[in->dnum] : dummy; break;
        case RT_TEXOUT: dst = in->dnum < 8 ? o_t[in->dnum] : dummy; break;
        default: dst = dummy; break;
        }
        for (int k = 0; k < 4; k++) if (in->dmask & (1 << k)) dst[k] = res[k];
    }
    if (vs_trace) {
        fprintf(stderr, "[vstrace %p] idx %u\n", (void *)sh, index);
        for (int k = 0; k < 16; k++) if (p->in[k].used)
            fprintf(stderr, "  v%d = %g %g %g %g\n", k, v[k][0], v[k][1], v[k][2], v[k][3]);
        for (int k = 0; k < 12; k++) fprintf(stderr, "  r%d = %g %g %g %g\n", k, r[k][0], r[k][1], r[k][2], r[k][3]);
        fprintf(stderr, "  oPos = %g %g %g %g  oD0 = %g %g %g %g  oD1 = %g %g %g %g  oT0 = %g %g\n", o_pos[0], o_pos[1],
                o_pos[2], o_pos[3], o_d[0][0], o_d[0][1], o_d[0][2], o_d[0][3], o_d[1][0], o_d[1][1], o_d[1][2],
                o_d[1][3], o_t[0][0], o_t[0][1]);
        for (int k = 0; k < 96; k++)
            fprintf(stderr, "  c%d = %g %g %g %g\n", k, c[k][0], c[k][1], c[k][2], c[k][3]);
    }
    memcpy(out->pos, o_pos, 16);
    memcpy(out->d0, o_d[0], 16);
    memcpy(out->d1, o_d[1], 16);
    for (int t = 0; t < 4; t++) memcpy(out->tex[t], o_t[t], 16);
}
