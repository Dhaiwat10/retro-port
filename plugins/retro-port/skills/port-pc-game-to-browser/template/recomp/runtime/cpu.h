/* Guest CPU state and memory model for recompiled game code.
 *
 * Every recompiled function has the signature `void f_XXXXXXXX(CPU *c)`. The guest stack lives
 * in guest memory and `call` pushes a real return address, so code that reads its arguments or
 * return address through ESP keeps working unchanged.
 */
#ifndef PORT_CPU_H
#define PORT_CPU_H

#include <stdint.h>
#include <string.h>

typedef union {
    float f[4];
    double d[2];
    uint8_t b[16];
    uint16_t w[8];
    uint32_t u[4];
    uint64_t q[2];
} XMM;

typedef struct CPU {
    uint32_t eax, ecx, edx, ebx, esp, ebp, esi, edi;
    /* Flags are kept as separate bytes; PF is derived from the low byte of the last result. */
    uint8_t cf, zf, sf, of, df;
    uint8_t idf;      /* EFLAGS.ID (bit 21): toggled by code probing for CPUID */
    uint8_t pf_byte;
    /* x87: values held as host doubles (exact 80-bit behaviour is a later milestone). */
    double st[8];
    int top;          /* index of ST(0) within st[] */
    uint16_t fpu_cw;  /* control word (rounding / precision) */
    uint16_t fpu_sw;  /* status word; C0/C2/C3 written by compares */
    XMM xmm[8];
    uint64_t mm[8];   /* MMX registers (kept separate from the x87 stack; code issues EMMS between uses) */
    uint32_t mxcsr;
    uint32_t fs_base; /* guest address of the thread information block (fs:[0] = SEH chain) */
    uint32_t preempt_ticks;   /* deterministic scheduling: function entries since this thread last yielded */
} CPU;

/* ------------------------------------------------------------------ guest memory */

extern uint8_t *g_mem; /* host pointer to guest address 0; 4 GiB reserved */

static inline uint8_t *HP(uint32_t a) { return g_mem + a; }

static inline uint8_t R8(uint32_t a) { return g_mem[a]; }
static inline uint16_t R16(uint32_t a) { uint16_t v; memcpy(&v, g_mem + a, 2); return v; }
static inline uint32_t R32(uint32_t a) { uint32_t v; memcpy(&v, g_mem + a, 4); return v; }
static inline uint64_t R64(uint32_t a) { uint64_t v; memcpy(&v, g_mem + a, 8); return v; }
static inline float RF32(uint32_t a) { float v; memcpy(&v, g_mem + a, 4); return v; }
static inline double RF64(uint32_t a) { double v; memcpy(&v, g_mem + a, 8); return v; }
/* PORT_BOUNDS (debug builds for wasm): writes outside the compact guest window would corrupt the host's
   own memory instead of faulting, so check them. */
#ifdef PORT_BOUNDS
void guest_bounds_fail(uint32_t a, int n);
#define BCHK(a, n) do { if ((uint64_t)(a) + (n) > 0x50010000ull) guest_bounds_fail((a), (n)); } while (0)
#else
#define BCHK(a, n) do { } while (0)
#endif
static inline void W8(uint32_t a, uint8_t v) { BCHK(a, 1); g_mem[a] = v; }
static inline void W16(uint32_t a, uint16_t v) { BCHK(a, 2); memcpy(g_mem + a, &v, 2); }
static inline void W32(uint32_t a, uint32_t v) { BCHK(a, 4); memcpy(g_mem + a, &v, 4); }
static inline void W64(uint32_t a, uint64_t v) { BCHK(a, 8); memcpy(g_mem + a, &v, 8); }
static inline void WF32(uint32_t a, float v) { BCHK(a, 4); memcpy(g_mem + a, &v, 4); }
static inline void WF64(uint32_t a, double v) { BCHK(a, 8); memcpy(g_mem + a, &v, 8); }

/* Global lock for x86 atomic instructions (lock prefix, xchg with memory) and the Interlocked* imports. */
extern volatile uint8_t g_guest_lock;
static inline void guest_lock(void) { while (__atomic_test_and_set(&g_guest_lock, __ATOMIC_ACQUIRE)) { } }
static inline void guest_unlock(void) { __atomic_clear(&g_guest_lock, __ATOMIC_RELEASE); }

static inline void PUSH32(CPU *c, uint32_t v) { c->esp -= 4; W32(c->esp, v); }
static inline uint32_t POP32(CPU *c) { uint32_t v = R32(c->esp); c->esp += 4; return v; }

/* ------------------------------------------------------------------ flags */

static inline int PF(const CPU *c) { return !__builtin_parity(c->pf_byte); }

#define DEF_FLAGS(BITS, T, ST)                                                                     \
    static inline T add##BITS(CPU *c, T a, T b) {                                                  \
        T r = (T)(a + b);                                                                          \
        c->cf = r < a; c->zf = r == 0; c->sf = (ST)r < 0; c->pf_byte = (uint8_t)r;                 \
        c->of = (((a ^ r) & (b ^ r)) >> (BITS - 1)) & 1;                                           \
        return r;                                                                                  \
    }                                                                                              \
    static inline T adc##BITS(CPU *c, T a, T b) {                                                  \
        unsigned cin = c->cf;                                                                      \
        T r = (T)(a + b + cin);                                                                    \
        c->cf = cin ? r <= a : r < a; c->zf = r == 0; c->sf = (ST)r < 0; c->pf_byte = (uint8_t)r;  \
        c->of = (((a ^ r) & (b ^ r)) >> (BITS - 1)) & 1;                                           \
        return r;                                                                                  \
    }                                                                                              \
    static inline T sub##BITS(CPU *c, T a, T b) {                                                  \
        T r = (T)(a - b);                                                                          \
        c->cf = a < b; c->zf = r == 0; c->sf = (ST)r < 0; c->pf_byte = (uint8_t)r;                 \
        c->of = (((a ^ b) & (a ^ r)) >> (BITS - 1)) & 1;                                           \
        return r;                                                                                  \
    }                                                                                              \
    static inline T sbb##BITS(CPU *c, T a, T b) {                                                  \
        unsigned cin = c->cf;                                                                      \
        T r = (T)(a - b - cin);                                                                    \
        c->cf = cin ? a <= b : a < b; c->zf = r == 0; c->sf = (ST)r < 0; c->pf_byte = (uint8_t)r;  \
        c->of = (((a ^ b) & (a ^ r)) >> (BITS - 1)) & 1;                                           \
        return r;                                                                                  \
    }                                                                                              \
    static inline T logic##BITS(CPU *c, T r) {                                                     \
        c->cf = 0; c->of = 0; c->zf = r == 0; c->sf = (ST)r < 0; c->pf_byte = (uint8_t)r;          \
        return r;                                                                                  \
    }                                                                                              \
    static inline T inc##BITS(CPU *c, T a) { /* CF preserved */                                    \
        T r = (T)(a + 1);                                                                          \
        c->zf = r == 0; c->sf = (ST)r < 0; c->pf_byte = (uint8_t)r;                                \
        c->of = r == (T)((T)1 << (BITS - 1));                                                      \
        return r;                                                                                  \
    }                                                                                              \
    static inline T dec##BITS(CPU *c, T a) {                                                       \
        T r = (T)(a - 1);                                                                          \
        c->zf = r == 0; c->sf = (ST)r < 0; c->pf_byte = (uint8_t)r;                                \
        c->of = a == (T)((T)1 << (BITS - 1));                                                      \
        return r;                                                                                  \
    }                                                                                              \
    static inline T neg##BITS(CPU *c, T a) {                                                       \
        T r = (T)(0 - a);                                                                          \
        c->cf = a != 0; c->zf = r == 0; c->sf = (ST)r < 0; c->pf_byte = (uint8_t)r;                \
        c->of = a == (T)((T)1 << (BITS - 1));                                                      \
        return r;                                                                                  \
    }                                                                                              \
    static inline T shl##BITS(CPU *c, T a, unsigned n) {                                           \
        n &= 31; if (!n) return a;                                                                 \
        T r = n >= BITS ? 0 : (T)(a << n);                                                         \
        c->cf = n <= BITS ? (a >> (BITS - n)) & 1 : 0;                                             \
        c->zf = r == 0; c->sf = (ST)r < 0; c->pf_byte = (uint8_t)r;                                \
        c->of = ((r >> (BITS - 1)) & 1) ^ c->cf;                                                   \
        return r;                                                                                  \
    }                                                                                              \
    static inline T shr##BITS(CPU *c, T a, unsigned n) {                                           \
        n &= 31; if (!n) return a;                                                                 \
        T r = n >= BITS ? 0 : (T)(a >> n);                                                         \
        c->cf = n <= BITS ? (a >> (n - 1)) & 1 : 0;                                                \
        c->zf = r == 0; c->sf = (ST)r < 0; c->pf_byte = (uint8_t)r;                                \
        c->of = (a >> (BITS - 1)) & 1;                                                             \
        return r;                                                                                  \
    }                                                                                              \
    static inline T sar##BITS(CPU *c, T a, unsigned n) {                                           \
        n &= 31; if (!n) return a;                                                                 \
        ST s = (ST)a;                                                                              \
        T r = (T)(n >= BITS ? (s < 0 ? -1 : 0) : (s >> n));                                        \
        c->cf = n >= BITS ? (s < 0) : (s >> (n - 1)) & 1;                                          \
        c->zf = r == 0; c->sf = (ST)r < 0; c->pf_byte = (uint8_t)r; c->of = 0;                     \
        return r;                                                                                  \
    }                                                                                              \
    static inline T rol##BITS(CPU *c, T a, unsigned n) {                                           \
        n &= 31; if (!n) return a;                                                                 \
        n %= BITS;                                                                                 \
        T r = n ? (T)((a << n) | (a >> (BITS - n))) : a;                                           \
        c->cf = r & 1; c->of = ((r >> (BITS - 1)) & 1) ^ c->cf;                                    \
        return r;                                                                                  \
    }                                                                                              \
    static inline T ror##BITS(CPU *c, T a, unsigned n) {                                           \
        n &= 31; if (!n) return a;                                                                 \
        n %= BITS;                                                                                 \
        T r = n ? (T)((a >> n) | (a << (BITS - n))) : a;                                           \
        c->cf = (r >> (BITS - 1)) & 1; c->of = c->cf ^ ((r >> (BITS - 2)) & 1);                    \
        return r;                                                                                  \
    }

#define DEF_RC(BITS, T)                                                                            \
    static inline T rcl##BITS(CPU *c, T a, unsigned n) {                                           \
        n = (n & 31) % (BITS + 1);                                                                 \
        for (unsigned i = 0; i < n; i++) { unsigned out = (a >> (BITS - 1)) & 1;                   \
            a = (T)((a << 1) | c->cf); c->cf = out; }                                              \
        if (n == 1) c->of = ((a >> (BITS - 1)) & 1) ^ c->cf;                                       \
        return a;                                                                                  \
    }                                                                                              \
    static inline T rcr##BITS(CPU *c, T a, unsigned n) {                                           \
        n = (n & 31) % (BITS + 1);                                                                 \
        if (n == 1) c->of = ((a >> (BITS - 1)) & 1) ^ c->cf;                                       \
        for (unsigned i = 0; i < n; i++) { unsigned out = a & 1;                                   \
            a = (T)((a >> 1) | ((T)c->cf << (BITS - 1))); c->cf = out; }                           \
        return a;                                                                                  \
    }

DEF_FLAGS(8, uint8_t, int8_t)
DEF_FLAGS(16, uint16_t, int16_t)
DEF_FLAGS(32, uint32_t, int32_t)
DEF_RC(8, uint8_t)
DEF_RC(16, uint16_t)
DEF_RC(32, uint32_t)

/* ------------------------------------------------------------------ x87 stack */

#define ST(i) (c->st[(c->top + (i)) & 7])
static inline void FPUSH(CPU *c, double v) { c->top = (c->top - 1) & 7; c->st[c->top] = v; }
static inline double FPOP(CPU *c) { double v = c->st[c->top]; c->top = (c->top + 1) & 7; return v; }
static inline void FCOM(CPU *c, double a, double b) {
    /* C3 (bit 14), C2 (bit 10), C0 (bit 8); TOP lives in bits 11-13 and is filled in by FNSTSW */
    uint16_t sw = c->fpu_sw & ~0x4500;
    if (a != a || b != b) sw |= 0x4500;
    else if (a < b) sw |= 0x0100;
    else if (a == b) sw |= 0x4000;
    c->fpu_sw = sw;
}
static inline void FCOMI(CPU *c, double a, double b) {
    c->of = 0; c->sf = 0;
    if (a != a || b != b) { c->zf = 1; c->pf_byte = 0; c->cf = 1; }
    else { c->zf = a == b; c->pf_byte = 1; c->cf = a < b; }
}
static inline uint16_t FNSTSW(CPU *c) { return (uint16_t)((c->fpu_sw & ~0x3800) | ((c->top & 7) << 11)); }
/* Precision control (FPU CW bits 8-9). Direct3D 8 switches the FPU to 24-bit precision unless the
   device is created with D3DCREATE_FPU_PRESERVE, so honouring it matters for matching the game. */
static inline double FPRES(CPU *c, double v) { return ((c->fpu_cw >> 8) & 3) == 0 ? (double)(float)v : v; }
/* Division, setting the sticky status flags code can read back with FNSTSW: ZE for x/0, IE for 0/0. */
static inline double FDIV(CPU *c, double a, double b) {
    if (b == 0 && a == a) c->fpu_sw |= (a == 0) ? 0x0001 : 0x0004;
    return a / b;
}
/* fsin/fcos: out-of-range arguments (|x| >= 2^63) are left unchanged with C2 set, as on hardware */
#define FTRIG(c, v, fn) (fabs(v) >= 9223372036854775808.0 ? ((c)->fpu_sw |= 0x0400, (v)) \
                                                         : ((c)->fpu_sw &= ~0x0400, fn(v)))
double RF80(uint32_t a);            /* load an 80-bit extended value */
void WF80(uint32_t a, double v);    /* store as 80-bit extended */
int32_t fpu_to_int32(CPU *c, double v);  /* honours the rounding mode in fpu_cw */
int64_t fpu_to_int64(CPU *c, double v);
double fpu_round(CPU *c, double v);      /* FRNDINT */
/* The x87 rounding-control bits (CW 10-11) are mirrored into the host FPU's rounding mode, so float
   stores, precision rounding and arithmetic round like the original (the game uses "chop" mode for
   fast float->int conversion). Generated code is compiled with -frounding-math. */
void fpu_sync_rounding(CPU *c);

/* ------------------------------------------------------------------ CPU identification */
void cpuid(CPU *c);   /* reports a plain Pentium-class CPU: no MMX/SSE/3DNow!, so the game takes x87 paths */
void rdtsc(CPU *c);

/* Deterministic preemption: with the deterministic scheduler on, every recompiled function entry counts
   toward a quantum, after which the thread yields. The count depends only on the thread's own execution,
   so preemption points are the same in every run, and threads that spin on memory another thread writes
   (no Windows call in the loop) still let it run. */
extern int g_preempt_on;
void guest_preempt(CPU *c);
#define PREEMPT_QUANTUM 100000u
#define PREEMPT_POLL(c) do { if (__builtin_expect(g_preempt_on, 0) && ++(c)->preempt_ticks >= PREEMPT_QUANTUM) guest_preempt(c); } while (0)

/* ------------------------------------------------------------------ dispatch */

typedef void (*GuestFn)(CPU *);
void call_indirect(CPU *c, uint32_t target);           /* call through a guest function pointer */
void jump_indirect(CPU *c, uint32_t target, uint32_t from); /* unresolved indirect jump */
void guest_trap(CPU *c, uint32_t addr, const char *why) __attribute__((noreturn));
void register_function(uint32_t addr, GuestFn fn);
void trace_insn(CPU *c, uint32_t addr);  /* only called by --trace builds */

/* Imports: the PE's IAT slots are filled with magic addresses that resolve to host functions. */
#define IMPORT_BASE 0xF0000000u

#endif
