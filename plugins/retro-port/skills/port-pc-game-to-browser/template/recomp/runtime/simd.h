/* Packed-integer helpers for MMX (8-byte) and SSE2 (16-byte) instructions.
 * All operate on little-endian byte arrays: d = d OP s, n = vector size in bytes. */
#ifndef PORT_SIMD_H
#define PORT_SIMD_H

#include <stdint.h>
#include <string.h>

static inline int64_t v_get(const uint8_t *p, int lane, int sgn) {
    switch (lane) {
    case 1: return sgn ? (int8_t)p[0] : p[0];
    case 2: { uint16_t v; memcpy(&v, p, 2); return sgn ? (int16_t)v : v; }
    case 4: { uint32_t v; memcpy(&v, p, 4); return sgn ? (int32_t)v : v; }
    default: { uint64_t v; memcpy(&v, p, 8); return (int64_t)v; }
    }
}
static inline void v_put(uint8_t *p, int lane, int64_t x) {
    switch (lane) {
    case 1: p[0] = (uint8_t)x; break;
    case 2: { uint16_t v = (uint16_t)x; memcpy(p, &v, 2); break; }
    case 4: { uint32_t v = (uint32_t)x; memcpy(p, &v, 4); break; }
    default: { uint64_t v = (uint64_t)x; memcpy(p, &v, 8); break; }
    }
}
static inline int64_t v_sat(int64_t x, int lane, int sgn) {
    int64_t lo = sgn ? -((int64_t)1 << (8 * lane - 1)) : 0;
    int64_t hi = sgn ? ((int64_t)1 << (8 * lane - 1)) - 1 : ((int64_t)1 << (8 * lane)) - 1;
    return x < lo ? lo : x > hi ? hi : x;
}

/* kind: 0 wrap, 1 signed saturate, 2 unsigned saturate */
static inline void v_add(uint8_t *d, const uint8_t *s, int n, int lane, int kind) {
    for (int i = 0; i < n; i += lane) {
        int sg = kind == 1;
        int64_t r = v_get(d + i, lane, sg) + v_get(s + i, lane, sg);
        v_put(d + i, lane, kind ? v_sat(r, lane, sg) : r);
    }
}
static inline void v_sub(uint8_t *d, const uint8_t *s, int n, int lane, int kind) {
    for (int i = 0; i < n; i += lane) {
        int sg = kind == 1;
        int64_t r = v_get(d + i, lane, sg) - v_get(s + i, lane, sg);
        v_put(d + i, lane, kind ? v_sat(r, lane, sg) : r);
    }
}
/* which: 0 low16 signed (pmullw), 1 high16 signed (pmulhw), 2 high16 unsigned (pmulhuw) */
static inline void v_mulw(uint8_t *d, const uint8_t *s, int n, int which) {
    for (int i = 0; i < n; i += 2) {
        int64_t r = v_get(d + i, 2, which != 2) * v_get(s + i, 2, which != 2);
        v_put(d + i, 2, which ? r >> 16 : r);
    }
}
static inline void v_pmaddwd(uint8_t *d, const uint8_t *s, int n) {
    for (int i = 0; i < n; i += 4) {
        int64_t r = v_get(d + i, 2, 1) * v_get(s + i, 2, 1) + v_get(d + i + 2, 2, 1) * v_get(s + i + 2, 2, 1);
        v_put(d + i, 4, r);
    }
}
static inline void v_pmuludq(uint8_t *d, const uint8_t *s, int n) {
    for (int i = 0; i < n; i += 8) v_put(d + i, 8, (int64_t)((uint64_t)v_get(d + i, 4, 0) * (uint64_t)v_get(s + i, 4, 0)));
}
/* kind: 0 sll, 1 srl, 2 sra */
static inline void v_shift(uint8_t *d, uint64_t cnt, int n, int lane, int kind) {
    int bits = 8 * lane;
    for (int i = 0; i < n; i += lane) {
        uint64_t v = (uint64_t)v_get(d + i, lane, 0);
        int64_t sv = v_get(d + i, lane, 1);
        int64_t r;
        if (kind == 2) r = cnt >= (uint64_t)bits ? (sv < 0 ? -1 : 0) : sv >> cnt;
        else if (cnt >= (uint64_t)bits) r = 0;
        else r = (int64_t)(kind == 0 ? v << cnt : v >> cnt);
        v_put(d + i, lane, r);
    }
}
static inline void v_bshift(uint8_t *d, unsigned cnt, int n, int left) {  /* pslldq / psrldq */
    uint8_t t[16] = { 0 };
    if (cnt > 16) cnt = 16;
    for (int i = 0; i < n; i++) {
        int src = left ? i - (int)cnt : i + (int)cnt;
        t[i] = (src >= 0 && src < n) ? d[src] : 0;
    }
    memcpy(d, t, (size_t)n);
}
/* kind: 0 and, 1 andn, 2 or, 3 xor */
static inline void v_logic(uint8_t *d, const uint8_t *s, int n, int kind) {
    for (int i = 0; i < n; i++)
        d[i] = kind == 0 ? d[i] & s[i] : kind == 1 ? (uint8_t)(~d[i] & s[i]) : kind == 2 ? d[i] | s[i] : d[i] ^ s[i];
}
static inline void v_unpack(uint8_t *d, const uint8_t *s, int n, int lane, int high) {
    uint8_t t[16];
    int half = n / 2, base = high ? half : 0;
    for (int i = 0; i < half / lane; i++) {
        memcpy(t + (2 * i) * lane, d + base + i * lane, (size_t)lane);
        memcpy(t + (2 * i + 1) * lane, s + base + i * lane, (size_t)lane);
    }
    memcpy(d, t, (size_t)n);
}
/* from_lane 2 -> 1 (packsswb / packuswb) or 4 -> 2 (packssdw) */
static inline void v_pack(uint8_t *d, const uint8_t *s, int n, int from_lane, int sgn_out) {
    uint8_t t[16];
    int to = from_lane / 2, cnt = n / from_lane;
    for (int i = 0; i < cnt; i++) v_put(t + i * to, to, v_sat(v_get(d + i * from_lane, from_lane, 1), to, sgn_out));
    for (int i = 0; i < cnt; i++) v_put(t + (cnt + i) * to, to, v_sat(v_get(s + i * from_lane, from_lane, 1), to, sgn_out));
    memcpy(d, t, (size_t)n);
}
static inline void v_cmp(uint8_t *d, const uint8_t *s, int n, int lane, int gt) {
    for (int i = 0; i < n; i += lane) {
        int64_t a = v_get(d + i, lane, 1), b = v_get(s + i, lane, 1);
        v_put(d + i, lane, (gt ? a > b : a == b) ? -1 : 0);
    }
}
/* kind: 0 pmaxsw, 1 pminsw, 2 pmaxub, 3 pminub, 4 pavgb, 5 pavgw */
static inline void v_minmax(uint8_t *d, const uint8_t *s, int n, int kind) {
    int lane = (kind == 0 || kind == 1 || kind == 5) ? 2 : 1, sg = kind <= 1;
    for (int i = 0; i < n; i += lane) {
        int64_t a = v_get(d + i, lane, sg), b = v_get(s + i, lane, sg), r;
        if (kind == 0 || kind == 2) r = a > b ? a : b;
        else if (kind == 1 || kind == 3) r = a < b ? a : b;
        else r = (a + b + 1) >> 1;
        v_put(d + i, lane, r);
    }
}
static inline void v_psadbw(uint8_t *d, const uint8_t *s, int n) {
    for (int k = 0; k < n; k += 8) {
        int sum = 0;
        for (int i = 0; i < 8; i++) sum += d[k + i] > s[k + i] ? d[k + i] - s[k + i] : s[k + i] - d[k + i];
        memset(d + k, 0, 8);
        v_put(d + k, 2, sum);
    }
}
static inline void v_pshufw(uint8_t *d, const uint8_t *s, unsigned imm, int lane, int n_words, int base) {
    uint8_t t[16];
    memcpy(t, d, 16);
    for (int i = 0; i < n_words; i++) memcpy(t + base + i * lane, s + base + ((imm >> (2 * i)) & 3) * lane, (size_t)lane);
    memcpy(d + base, t + base, (size_t)(n_words * lane));
}
static inline uint32_t v_movmskb(const uint8_t *s, int n) {
    uint32_t m = 0;
    for (int i = 0; i < n; i++) m |= (uint32_t)(s[i] >> 7) << i;
    return m;
}

#endif
