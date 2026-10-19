/* Direct3D 8 pixel shaders (ps.1.1) translated to WGSL fragment shaders.
 *
 * ps.1.1 has four texture registers (t0-t3, filled by "texture address" instructions such as tex,
 * texcoord or texbem), two colour inputs (v0 diffuse, v1 specular), two temporaries (r0 = output)
 * and eight constants. Arithmetic results are clamped to [-1, 1] as on period hardware; r0 is
 * saturated at the end. Co-issued instructions (rgb + alpha pairs) read their sources before either
 * writes. The generated function plugs into the shared WGSL header in render_wgpu.c. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { char *s; size_t n, cap; } Buf;
static void put(Buf *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
#include <stdarg.h>
static void put(Buf *b, const char *fmt, ...) {
    va_list ap;
    for (;;) {
        va_start(ap, fmt);
        int k = vsnprintf(b->s + b->n, b->cap - b->n, fmt, ap);
        va_end(ap);
        if (k >= 0 && b->n + (size_t)k < b->cap) { b->n += (size_t)k; return; }
        b->cap = b->cap * 2 + (size_t)k + 256;
        b->s = realloc(b->s, b->cap);
    }
}

static void fmt_float(char *o, size_t n, float f) {
    snprintf(o, n, "%.9g", (double)f);
    if (!strpbrk(o, ".eEn")) strncat(o, ".0", n - strlen(o) - 1);
}

typedef struct { float v[4]; int set; } Def;

/* source operand -> WGSL expression */
static void src_expr(char *out, size_t n, uint32_t t, const Def *defs) {
    uint32_t type = (t >> 28) & 7, num = t & 0x1FFF, swz = (t >> 16) & 0xFF, mod = (t >> 24) & 0xF;
    char base[96];
    switch (type) {
    case 0: snprintf(base, sizeof base, "r%u", num); break;
    case 1: snprintf(base, sizeof base, "v%u", num); break;
    case 2:
        if (num < 8 && defs[num].set) {
            char a[4][32];
            for (int k = 0; k < 4; k++) fmt_float(a[k], sizeof a[k], defs[num].v[k]);
            snprintf(base, sizeof base, "vec4<f32>(%s, %s, %s, %s)", a[0], a[1], a[2], a[3]);
        } else snprintf(base, sizeof base, "u.psc[%u]", num & 7);
        break;
    case 3: snprintf(base, sizeof base, "t%u", num & 3); break;
    default: snprintf(base, sizeof base, "vec4<f32>(0.0)"); break;
    }
    char sw[96];
    if (swz == 0xE4) snprintf(sw, sizeof sw, "%s", base);
    else {
        static const char c[4] = { 'x', 'y', 'z', 'w' };
        snprintf(sw, sizeof sw, "(%s).%c%c%c%c", base, c[swz & 3], c[(swz >> 2) & 3], c[(swz >> 4) & 3], c[(swz >> 6) & 3]);
    }
    switch (mod) {
    case 1: snprintf(out, n, "(-%s)", sw); break;
    case 2: snprintf(out, n, "(%s - vec4<f32>(0.5))", sw); break;
    case 3: snprintf(out, n, "(vec4<f32>(0.5) - %s)", sw); break;
    case 4: snprintf(out, n, "(%s * 2.0 - vec4<f32>(1.0))", sw); break;
    case 5: snprintf(out, n, "(vec4<f32>(1.0) - %s * 2.0)", sw); break;
    case 6: snprintf(out, n, "(vec4<f32>(1.0) - %s)", sw); break;
    case 7: snprintf(out, n, "(%s * 2.0)", sw); break;
    case 8: snprintf(out, n, "(%s * -2.0)", sw); break;
    default: snprintf(out, n, "%s", sw); break;
    }
}

static const char *dst_name(uint32_t t, char *buf, size_t n) {
    uint32_t type = (t >> 28) & 7, num = t & 0x1FFF;
    snprintf(buf, n, "%s%u", type == 3 ? "t" : "r", type == 3 ? (num & 3) : (num & 1));
    return buf;
}

/* Returns a malloc'd WGSL fragment entry point `fs`, or NULL if the shader uses something
   unsupported (the caller then falls back to the fixed-function stage). `cube_mask` reports which
   texture stages are sampled as cube maps (not supported yet: such shaders are rejected). */
char *ps_to_wgsl(const uint32_t *code, uint32_t n, char *why, size_t why_n) {
    if (!n || (code[0] != 0xFFFF0101u && code[0] != 0xFFFF0100u)) {
        snprintf(why, why_n, "version %08x", n ? code[0] : 0);
        return NULL;
    }
    Def defs[8];
    memset(defs, 0, sizeof defs);
    /* first pass: constants from def */
    for (uint32_t i = 1; i < n;) {
        uint32_t tok = code[i++], op = tok & 0xFFFF;
        if (tok == 0x0000FFFFu) break;
        if (op == 0xFFFE) { i += (tok >> 16) & 0x7FFF; continue; }
        if (op == 81) {
            uint32_t r = code[i] & 0x1FFF;
            if (r < 8) { memcpy(defs[r].v, &code[i + 1], 16); defs[r].set = 1; }
            i += 5;
            continue;
        }
        static const int8_t np[96] = { [0] = 0, [1] = 2, [2] = 3, [3] = 3, [4] = 4, [5] = 3, [8] = 3, [9] = 3,
                                       [18] = 4, [64] = 1, [65] = 1, [66] = 1, [67] = 2, [68] = 2, [69] = 2,
                                       [70] = 2, [71] = 2, [72] = 2, [73] = 2, [74] = 2, [76] = 3, [77] = 2,
                                       [80] = 4, [88] = 4 };
        if (op >= 96) { snprintf(why, why_n, "opcode %u", op); return NULL; }
        i += (uint32_t)np[op];
    }

    Buf b = { 0 };
    put(&b, "@fragment fn fs(i: VO) -> @location(0) vec4<f32> {\n"
            "  var r0 = vec4<f32>(0.0); var r1 = vec4<f32>(0.0);\n"
            "  var t0 = vec4<f32>(0.0); var t1 = vec4<f32>(0.0); var t2 = vec4<f32>(0.0); var t3 = vec4<f32>(0.0);\n"
            "  let v0 = i.diffuse; let v1 = i.specular;\n"
            "  var kill = false;\n"
            "  var pad0 = 0.0; var pad1 = 0.0;\n");
    char pending[4][160];
    int npending = 0, tmp = 0, m3x3 = 0;
    for (uint32_t i = 1; i < n;) {
        uint32_t tok = code[i++], op = tok & 0xFFFF;
        if (tok == 0x0000FFFFu) break;
        if (op == 0xFFFE) { i += (tok >> 16) & 0x7FFF; continue; }
        if (op == 81) { i += 5; continue; }
        if (op == 0) continue;
        int coissue = (tok >> 30) & 1;
        if (!coissue) { for (int k = 0; k < npending; k++) put(&b, "%s", pending[k]); npending = 0; }
        uint32_t d = code[i++];
        char dn[16];
        dst_name(d, dn, sizeof dn);
        uint32_t dnum = d & 3;
        if (op >= 64 && op <= 77) {
            /* texture address instructions write t# directly (never co-issued) */
            uint32_t s = op == 64 || op == 65 || op == 66 ? 0 : code[i++];
            uint32_t m = s & 3;
            if (op == 76) i++;   /* texm3x3spec: eye-ray constant */
            switch (op) {
            case 64: put(&b, "  t%u = vec4<f32>(clamp(i.tc%u.xyz, vec3<f32>(0.0), vec3<f32>(1.0)), 1.0);\n", dnum, dnum); break;
            case 65: put(&b, "  kill = kill || any(i.tc%u.xyz < vec3<f32>(0.0));\n", dnum); break;
            case 66: put(&b, "  t%u = textureSample(tex%u, smp%u, i.tc%u.xy);\n", dnum, dnum, dnum, dnum); break;
            case 67: case 68:
                put(&b, "  t%u = textureSample(tex%u, smp%u, i.tc%u.xy + vec2<f32>(u.bem[%u].x * t%u.x + u.bem[%u].z * t%u.y, "
                        "u.bem[%u].y * t%u.x + u.bem[%u].w * t%u.y));\n", dnum, dnum, dnum, dnum, dnum, m, dnum, m, dnum, m, dnum, m);
                if (op == 68) put(&b, "  t%u = vec4<f32>(t%u.xyz * saturate(t%u.z * u.bel[%u].x + u.bel[%u].y), t%u.w);\n",
                                  dnum, dnum, m, dnum, dnum, dnum);
                break;
            case 69: put(&b, "  t%u = textureSample(tex%u, smp%u, t%u.wx);\n", dnum, dnum, dnum, m); break;
            case 70: put(&b, "  t%u = textureSample(tex%u, smp%u, t%u.yz);\n", dnum, dnum, dnum, m); break;
            case 71: put(&b, "  pad0 = dot(i.tc%u.xyz, t%u.xyz);\n", dnum, m); break;
            case 72: put(&b, "  t%u = textureSample(tex%u, smp%u, vec2<f32>(pad0, dot(i.tc%u.xyz, t%u.xyz)));\n",
                         dnum, dnum, dnum, dnum, m); break;
            case 73: put(&b, "  pad%d = dot(i.tc%u.xyz, t%u.xyz);\n", m3x3++ & 1, dnum, m); break;
            default:
                free(b.s);
                snprintf(why, why_n, "texture op %u (cube maps)", op);
                return NULL;
            }
            continue;
        }
        int ns = op == 1 ? 1 : (op == 4 || op == 18 || op == 80 || op == 88) ? 3 : 2;
        char s[3][320];
        for (int k = 0; k < ns; k++) src_expr(s[k], sizeof s[k], code[i++], defs);
        char e[1200];
        switch (op) {
        case 1: snprintf(e, sizeof e, "%s", s[0]); break;
        case 2: snprintf(e, sizeof e, "%s + %s", s[0], s[1]); break;
        case 3: snprintf(e, sizeof e, "%s - %s", s[0], s[1]); break;
        case 4: snprintf(e, sizeof e, "%s * %s + %s", s[0], s[1], s[2]); break;
        case 5: snprintf(e, sizeof e, "%s * %s", s[0], s[1]); break;
        case 8: snprintf(e, sizeof e, "vec4<f32>(dot((%s).xyz, (%s).xyz))", s[0], s[1]); break;
        case 9: snprintf(e, sizeof e, "vec4<f32>(dot(%s, %s))", s[0], s[1]); break;
        case 18: snprintf(e, sizeof e, "mix(%s, %s, %s)", s[2], s[1], s[0]); break;
        case 80: snprintf(e, sizeof e, "select(%s, %s, (%s) > vec4<f32>(0.5))", s[2], s[1], s[0]); break;
        case 88: snprintf(e, sizeof e, "select(%s, %s, (%s) >= vec4<f32>(0.0))", s[2], s[1], s[0]); break;
        default:
            free(b.s);
            snprintf(why, why_n, "opcode %u", op);
            return NULL;
        }
        int shift = (int)((d >> 24) & 0xF);
        if (shift & 8) shift -= 16;
        float scale = shift >= 0 ? (float)(1 << shift) : 1.f / (float)(1 << -shift);
        int sat = (d >> 20) & 1;
        char sc[32];
        fmt_float(sc, sizeof sc, scale);
        put(&b, "  let a%d = %s((%s) * %s%s);\n", tmp, sat ? "saturate" : "clamp", e, sc,
            sat ? "" : ", vec4<f32>(-1.0), vec4<f32>(1.0)");
        uint32_t mask = (d >> 16) & 0xF;
        if (npending < 4)
            snprintf(pending[npending++], sizeof pending[0], "  %s = select(%s, a%d, vec4<bool>(%s, %s, %s, %s));\n", dn, dn, tmp,
                     mask & 1 ? "true" : "false", mask & 2 ? "true" : "false", mask & 4 ? "true" : "false",
                     mask & 8 ? "true" : "false");
        tmp++;
    }
    for (int k = 0; k < npending; k++) put(&b, "%s", pending[k]);
    put(&b, "  let o = saturate(r0);\n"
            "  if (kill || !alpha_ok(o.a)) { discard; }\n"
            "  return o;\n"
            "}\n");
    return b.s;
}
