/* Runtime for recompiled code: guest memory, PE image loading, function dispatch, traps. */
#include "runtime.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdlib.h>
#include <sys/mman.h>

uint8_t *g_mem;

/* ------------------------------------------------------------------ function table */

typedef struct { uint32_t addr; GuestFn fn; } FnEntry;
static FnEntry *g_fns;
static uint32_t g_fn_cap;  /* power of two */

void register_function(uint32_t addr, GuestFn fn) {
    if (!g_fns) {
        g_fn_cap = 1u << 16;
        g_fns = calloc(g_fn_cap, sizeof *g_fns);
    }
    uint32_t i = (addr * 2654435761u) & (g_fn_cap - 1);
    while (g_fns[i].fn && g_fns[i].addr != addr) i = (i + 1) & (g_fn_cap - 1);
    g_fns[i].addr = addr;
    g_fns[i].fn = fn;
}

GuestFn lookup_function(uint32_t addr) {
    if (!g_fns) return NULL;
    uint32_t i = (addr * 2654435761u) & (g_fn_cap - 1);
    while (g_fns[i].fn) {
        if (g_fns[i].addr == addr) return g_fns[i].fn;
        i = (i + 1) & (g_fn_cap - 1);
    }
    return NULL;
}

static void unimplemented_import(CPU *c);
static int g_log_imports = -1;
__thread uint32_t g_import_target;  /* which host slot the current import call came through */

static uint32_t g_trace_call = 1;   /* PORT_TRACE_CALL=addr: log indirect calls to that function */

void call_indirect(CPU *c, uint32_t target) {
    if (g_trace_call == 1) g_trace_call = getenv("PORT_TRACE_CALL") ? (uint32_t)strtoul(getenv("PORT_TRACE_CALL"), NULL, 16) : 0;
    uint32_t traced_this = 0;
    if (g_trace_call && target == g_trace_call) {
        static int n;
        if (n++ < 40) {
            fprintf(stderr, "[call] %08x from %08x ecx=%08x args %08x %08x %08x %08x\n", target, R32(c->esp), c->ecx,
                    R32(c->esp + 4), R32(c->esp + 8), R32(c->esp + 12), R32(c->esp + 16));
            traced_this = c->ecx;
        }
    }
    GuestFn fn = lookup_function(target);
    if (traced_this && fn) {
        fn(c);
        /* debugging the lighting instance: +0xc0 multipliers, +0x140 blend, config(+0x10) +0x70..+0x8c */
        uint32_t o = traced_this, cfg = R32(o + 0x10);
        float f[4], t, a[8];
        memcpy(f, HP(o + 0xc0), 16); memcpy(&t, HP(o + 0x140), 4);
        if (cfg) memcpy(a, HP(cfg + 0x70), 32); else memset(a, 0, sizeof a);
        fprintf(stderr, "[call]   -> mult %g %g %g %g  t %g  cfg %08x lo %g %g %g %g hi %g %g %g %g\n", f[0], f[1], f[2], f[3], t,
                cfg, a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7]);
        return;
    }
    if (!fn && dyncode_contains(target)) { interp_call(c, target); return; }
    if (!fn) {
        static char msg[96];
        snprintf(msg, sizeof msg, "indirect call to unknown function (return address %08x)", R32(c->esp));
        guest_trap(c, target, msg);
    }
    if (target >= IMPORT_BASE) {
        const char *name = import_name(target);
        if (g_log_imports < 0) g_log_imports = getenv("PORT_LOG_IMPORTS") != NULL;
        if (g_log_imports && name)
            fprintf(stderr, "[import] %-40s from %08x  args %08x %08x %08x %08x\n", name, R32(c->esp),
                    R32(c->esp + 4), R32(c->esp + 8), R32(c->esp + 12), R32(c->esp + 16));
        g_import_target = target;
        if (fn == unimplemented_import) {
            static char msg[160];
            snprintf(msg, sizeof msg, "unimplemented Windows import %s (called from %08x)",
                     name ? name : "?", R32(c->esp));
            guest_trap(c, target, msg);
        }
    }
    fn(c);
}

void jump_indirect(CPU *c, uint32_t target, uint32_t from) {
    /* An indirect jump that isn't a known switch case: treat as a tail call. */
    GuestFn fn = lookup_function(target);
    if (!fn) {
        fprintf(stderr, "indirect jump from %08x\n", from);
        guest_trap(c, target, "indirect jump to unknown target");
    }
    fn(c);
}

void guest_bounds_fail(uint32_t a, int n) {
    fprintf(stderr, "\n*** guest write of %d bytes at %08x is outside guest memory\n", n, a);
    __builtin_trap();   /* the engine's stack trace names the recompiled function */
}

volatile uint8_t g_guest_lock;

jmp_buf *g_trap_jmp;        /* when set (tests), traps longjmp here instead of aborting */
char g_trap_msg[256];

void guest_trap(CPU *c, uint32_t addr, const char *why) {
    if (g_trap_jmp) {
        snprintf(g_trap_msg, sizeof g_trap_msg, "%08x: %s", addr, why);
        longjmp(*g_trap_jmp, 1);
    }
    fprintf(stderr, "\n*** guest trap at %08x: %s\n", addr, why);
    fprintf(stderr, "    eax=%08x ecx=%08x edx=%08x ebx=%08x esp=%08x ebp=%08x esi=%08x edi=%08x\n",
            c->eax, c->ecx, c->edx, c->ebx, c->esp, c->ebp, c->esi, c->edi);
    /* Stack words that look like return addresses into .text, plus the memory eax/ecx point at. */
    fprintf(stderr, "    stack:");
    for (int i = 0; i < 64; i++) {
        uint32_t v = R32(c->esp + 4 * i);
        if (looks_like_code(v)) fprintf(stderr, " [%d]%08x", i, v);
    }
    fprintf(stderr, "\n");
    uint32_t regs[2] = { c->eax, c->ecx };
    for (int r = 0; r < 2; r++) {
        if (regs[r] < GUEST_HEAP_BASE || regs[r] >= GUEST_HEAP_HI) continue;
        fprintf(stderr, "    [%s]:", r ? "ecx" : "eax");
        for (int i = -4; i < 12; i++) fprintf(stderr, " %08x", R32(regs[r] + 4 * i));
        fprintf(stderr, "\n");
    }
    if (addr >= GUEST_HEAP_BASE && addr < GUEST_VALLOC_HI) {
        fprintf(stderr, "    code at %08x:", addr);
        for (int i = 0; i < 64; i++) fprintf(stderr, "%s%02x", i % 32 ? "" : "\n      ", R8(addr + i));
        fprintf(stderr, "\n");
    }
    abort();
}

/* ------------------------------------------------------------------ tracing (--trace builds) */

TraceRec *g_trace;
uint32_t g_trace_len, g_trace_cap;

void trace_insn(CPU *c, uint32_t addr) {
    if (!g_trace || g_trace_len >= g_trace_cap) return;
    TraceRec *r = &g_trace[g_trace_len++];
    r->addr = addr;
    r->regs[0] = c->eax; r->regs[1] = c->ecx; r->regs[2] = c->edx; r->regs[3] = c->ebx;
    r->regs[4] = c->esp; r->regs[5] = c->ebp; r->regs[6] = c->esi; r->regs[7] = c->edi;
    r->st0 = c->st[c->top];
    r->top = (uint32_t)c->top;
}

/* ------------------------------------------------------------------ x87 helpers */

static double round_mode(CPU *c, double v) {
    switch ((c->fpu_cw >> 10) & 3) {
    case 0: return nearbyint(v); /* host default mode is round-to-nearest-even */
    case 1: return floor(v);
    case 2: return ceil(v);
    default: return trunc(v);
    }
}

int32_t fpu_to_int32(CPU *c, double v) {
    double r = round_mode(c, v);
    if (!(r >= -2147483648.0 && r <= 2147483647.0)) return INT32_MIN; /* "integer indefinite" */
    return (int32_t)r;
}

int64_t fpu_to_int64(CPU *c, double v) {
    double r = round_mode(c, v);
    if (!(r >= -9223372036854775808.0 && r < 9223372036854775808.0)) return INT64_MIN;
    return (int64_t)r;
}

double fpu_round(CPU *c, double v) { return round_mode(c, v); }

#include <fenv.h>
void fpu_sync_rounding(CPU *c) {
    static const int modes[4] = { FE_TONEAREST, FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO };
    static __thread int current = -1;  /* MSVC's _ftol switches modes twice per conversion: skip no-ops */
    int rc = (c->fpu_cw >> 10) & 3;
    if (rc != current) { fesetround(modes[rc]); current = rc; }
}

double RF80(uint32_t a) {
    uint64_t mant = R64(a);
    uint16_t se = R16(a + 8);
    int sign = se >> 15, exp = se & 0x7fff;
    double v;
    if (exp == 0 && mant == 0) v = 0.0;
    else if (exp == 0x7fff) v = (mant << 1) ? NAN : INFINITY;
    else v = ldexp((double)mant, exp - 16383 - 63);
    return sign ? -v : v;
}

void WF80(uint32_t a, double v) {
    uint16_t se = signbit(v) ? 0x8000 : 0;
    uint64_t mant = 0;
    v = fabs(v);
    if (isnan(v)) { se |= 0x7fff; mant = 0xc000000000000000ull; }
    else if (isinf(v)) { se |= 0x7fff; mant = 0x8000000000000000ull; }
    else if (v != 0) {
        int e;
        double m = frexp(v, &e);           /* v = m * 2^e, 0.5 <= m < 1 */
        mant = (uint64_t)ldexp(m, 64);      /* explicit integer bit at bit 63 */
        se |= (uint16_t)(e - 1 + 16383);
    }
    W64(a, mant);
    W16(a + 8, se);
}

/* ------------------------------------------------------------------ CPUID / RDTSC */

void cpuid(CPU *c) {
    switch (c->eax) {
    case 0:  /* max standard leaf 1, "GenuineIntel" */
        c->eax = 1; c->ebx = 0x756e6547; c->edx = 0x49656e69; c->ecx = 0x6c65746e; break;
    case 1:  /* family 6, model 8; FPU, TSC, CX8, CMOV only: deliberately no MMX (bit 23) or SSE (25/26) */
        c->eax = 0x00000683; c->ebx = 0; c->ecx = 0; c->edx = (1u << 0) | (1u << 4) | (1u << 8) | (1u << 15); break;
    case 0x80000000u:  /* no extended leaves, so no 3DNow! */
        c->eax = 0x80000000u; c->ebx = c->ecx = c->edx = 0; break;
    default:
        c->eax = c->ebx = c->ecx = c->edx = 0;
    }
}

#include <time.h>
/* A 2 GHz timestamp counter, consistent with the CPU speed reported through the registry. */
uint64_t (*g_rdtsc_hook)(void);   /* set by the host for deterministic runs */
int g_preempt_on;
void (*g_preempt_fn)(void);       /* the deterministic scheduler's yield */
void guest_preempt(CPU *c) { c->preempt_ticks = 0; if (g_preempt_fn) g_preempt_fn(); }
void rdtsc(CPU *c) {
    if (g_rdtsc_hook) { uint64_t t = g_rdtsc_hook(); c->eax = (uint32_t)t; c->edx = (uint32_t)(t >> 32); return; }
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t t = (uint64_t)ts.tv_sec * 2000000000ull + (uint64_t)ts.tv_nsec * 2ull;
    c->eax = (uint32_t)t; c->edx = (uint32_t)(t >> 32);
}

/* ------------------------------------------------------------------ imports */

typedef struct { char name[96]; GuestFn fn; } ImportSlot;
static ImportSlot g_imports[4096];  /* PE imports + GetProcAddress results + COM method slots */
static uint32_t g_nimports;

static void unimplemented_import(CPU *c) {
    /* The return address on the stack identifies the caller; the slot is found from the IAT
       value the caller used, which call_indirect passed as the target. */
    guest_trap(c, R32(c->esp), "call to an unimplemented Windows import");
}

static ImportHandler *g_import_handlers;
static uint32_t g_nhandlers;

void set_import_handlers(ImportHandler *h, uint32_t n) { g_import_handlers = h; g_nhandlers = n; }

static GuestFn find_handler(const char *name) {
    for (uint32_t i = 0; i < g_nhandlers; i++)
        if (!strcmp(g_import_handlers[i].name, name)) return g_import_handlers[i].fn;
    return NULL;
}

uint32_t add_dynamic_import(const char *name, GuestFn fn) {
    ImportSlot *sl = &g_imports[g_nimports];
    snprintf(sl->name, sizeof sl->name, "%s", name);
    sl->fn = fn;
    uint32_t magic = IMPORT_BASE + g_nimports * 16;
    register_function(magic, fn);
    g_nimports++;
    return magic;
}

GuestFn find_import_handler(const char *name) { return find_handler(name); }

const char *import_name(uint32_t addr) {
    if (addr < IMPORT_BASE || addr >= IMPORT_BASE + g_nimports * 16) return NULL;
    return g_imports[(addr - IMPORT_BASE) / 16].name;
}

/* ------------------------------------------------------------------ PE loading */

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

uint32_t g_code_lo = 0x401000, g_code_hi = 0x401000, g_data_lo, g_data_hi;

int load_image(const char *path, LoadedImage *out) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); return -1; }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *file = malloc(size);
    if (fread(file, 1, size, f) != (size_t)size) { fclose(f); return -1; }
    fclose(f);

    const uint8_t *pe = file + rd32(file + 0x3c);
    uint16_t nsec = rd16(pe + 6);
    uint16_t optsz = rd16(pe + 20);
    const uint8_t *opt = pe + 24;
    uint32_t image_base = rd32(opt + 28);
    uint32_t entry = rd32(opt + 16);
    uint32_t hdr_size = rd32(opt + 60);
    uint32_t imp_rva = rd32(opt + 96 + 8);

    memcpy(HP(image_base), file, hdr_size);
    const uint8_t *sec = opt + optsz;
    for (int i = 0; i < nsec; i++, sec += 40) {
        uint32_t vsize = rd32(sec + 8), va = rd32(sec + 12), rawsz = rd32(sec + 16), rawptr = rd32(sec + 20);
        uint32_t n = rawsz < vsize ? rawsz : vsize;
        memset(HP(image_base + va), 0, vsize);
        memcpy(HP(image_base + va), file + rawptr, n);
        uint32_t flags = rd32(sec + 36), lo = image_base + va, hi = lo + vsize;
        if (flags & 0x20000000u) {   /* IMAGE_SCN_MEM_EXECUTE */
            if (g_code_hi == 0x401000 || lo < g_code_lo) g_code_lo = lo;
            if (hi > g_code_hi) g_code_hi = hi;
        } else if (flags & 0x80000000u) {   /* IMAGE_SCN_MEM_WRITE: .data, .data1, ... */
            if (!g_data_lo || lo < g_data_lo) g_data_lo = lo;
            if (hi > g_data_hi) g_data_hi = hi;
        }
    }
    g_data_lo &= ~7u;
    g_data_hi = (g_data_hi + 7) & ~7u;

    /* Resolve imports: each IAT slot gets a magic address that dispatches to a host handler. */
    for (uint32_t d = image_base + imp_rva; R32(d + 12); d += 20) {
        const char *dll = (const char *)HP(image_base + R32(d + 12));
        uint32_t ilt = R32(d) ? R32(d) : R32(d + 16), iat = R32(d + 16);
        for (uint32_t k = 0;; k++) {
            uint32_t thunk = R32(image_base + ilt + 4 * k);
            if (!thunk) break;
            ImportSlot *s = &g_imports[g_nimports];
            if (thunk & 0x80000000u)
                snprintf(s->name, sizeof s->name, "%s!#%u", dll, thunk & 0xffff);
            else
                snprintf(s->name, sizeof s->name, "%s!%s", dll, (const char *)HP(image_base + thunk + 2));
            for (char *p = s->name; *p && *p != '!'; p++) *p = (char)toupper((unsigned char)*p);
            GuestFn fn = find_handler(s->name);
            s->fn = fn ? fn : unimplemented_import;
            uint32_t magic = IMPORT_BASE + g_nimports * 16;
            W32(image_base + iat + 4 * k, magic);
            register_function(magic, s->fn);
            g_nimports++;
        }
    }
    free(file);
    out->image_base = image_base;
    out->entry = image_base + entry;
    out->num_imports = g_nimports;
    return 0;
}

/* ------------------------------------------------------------------ memory + CPU setup */

void runtime_init(CPU *c) {
    /* +64 KiB so multi-byte accesses straddling 0xFFFFFFFF stay inside the mapping */
#ifdef __EMSCRIPTEN__
    g_mem = calloc(1, (size_t)GUEST_SPACE + 0x10000);
    if (!g_mem) { fprintf(stderr, "cannot allocate guest memory\n"); exit(1); }
#else
    g_mem = mmap(NULL, GUEST_SPACE + 0x10000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
    if (g_mem == MAP_FAILED) { perror("mmap 4GiB guest space"); exit(1); }
#endif
    memset(c, 0, sizeof *c);
    c->esp = GUEST_STACK_TOP;
    c->fpu_cw = 0x027f; /* MSVC default: 53-bit precision, round to nearest, exceptions masked */
    fpu_sync_rounding(c);
    c->mxcsr = 0x1f80;
    c->fs_base = GUEST_TIB;
    W32(GUEST_TIB + 0x00, 0xffffffffu);    /* empty SEH chain */
    W32(GUEST_TIB + 0x18, GUEST_TIB);       /* self pointer */
    W32(GUEST_TIB + 0x04, GUEST_STACK_TOP); /* stack base */
    W32(GUEST_TIB + 0x08, GUEST_STACK_TOP - GUEST_STACK_SIZE);
}

uint32_t g_heap_next = GUEST_HEAP_BASE;

uint32_t guest_alloc(uint32_t size) {
    uint32_t a = (g_heap_next + 15) & ~15u;
    g_heap_next = a + size;
    return a;
}

/* Calls a recompiled function the way the original would be called: arguments are pushed
   right-to-left and a sentinel return address is pushed. The callee pops what its convention says. */
void guest_call(CPU *c, GuestFn fn, int nargs, const uint32_t *args) {
    for (int i = nargs - 1; i >= 0; i--) PUSH32(c, args[i]);
    PUSH32(c, GUEST_RETURN_SENTINEL);
    fn(c);
}
