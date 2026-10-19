/* Runs the game's own zlib inflate (recompiled from x86) on zlib streams and checks the output
 * byte-for-byte against the expected data.
 * Usage: test_zlib <game.exe> <case1.z> <case1.out> [<case2.z> <case2.out> ...]
 */
#include "../out_zlib/recomp.h"

#include <stdio.h>
#include <stdlib.h>

#define F_INFLATE_INIT2 f_0067a97a
#define F_INFLATE       f_0067aa76
#define F_INFLATE_END   f_0067a93c
#define HOST_ZALLOC 0xF8000000u
#define HOST_ZFREE  0xF8000010u

/* z_stream (zlib 1.1.x, 0x38 bytes) field offsets */
enum { ZS_NEXT_IN = 0x00, ZS_AVAIL_IN = 0x04, ZS_TOTAL_IN = 0x08, ZS_NEXT_OUT = 0x0c, ZS_AVAIL_OUT = 0x10,
       ZS_TOTAL_OUT = 0x14, ZS_MSG = 0x18, ZS_ZALLOC = 0x20, ZS_ZFREE = 0x24, ZS_OPAQUE = 0x28 };

/* Host implementations of the allocator callbacks the game's zlib calls through pointers (cdecl). */
static void host_zalloc(CPU *c) {
    uint32_t items = R32(c->esp + 8), size = R32(c->esp + 12);
    uint32_t a = guest_alloc(items * size);
    memset(HP(a), 0, (size_t)items * size);
    c->eax = a;
    c->esp += 4; /* ret */
}
static void host_zfree(CPU *c) { c->esp += 4; }

static uint8_t *slurp(const char *p, long *n) {
    FILE *f = fopen(p, "rb");
    if (!f) { perror(p); exit(2); }
    fseek(f, 0, SEEK_END); *n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc(*n ? *n : 1);
    if (fread(b, 1, *n, f) != (size_t)*n) exit(2);
    fclose(f);
    return b;
}

static uint32_t cdecl_call(CPU *c, GuestFn fn, int n, const uint32_t *args) {
    uint32_t sp = c->esp;
    guest_call(c, fn, n, args);
    c->esp = sp; /* caller cleans up */
    return c->eax;
}

int main(int argc, char **argv) {
    static CPU cpu;
    CPU *c = &cpu;
    runtime_init(c);
    LoadedImage img;
    if (load_image(argv[1], &img)) return 1;
    register_all_functions();
    register_function(HOST_ZALLOC, host_zalloc);
    register_function(HOST_ZFREE, host_zfree);

    uint32_t version = guest_alloc(16);
    strcpy((char *)HP(version), "1.1.3");
    int pass = 0, fail = 0;
    for (int i = 2; i + 1 < argc; i += 2) {
        long zn, en;
        uint8_t *z = slurp(argv[i], &zn), *expect = slurp(argv[i + 1], &en);
        uint32_t in = guest_alloc(zn), out = guest_alloc(en + 64), strm = guest_alloc(0x38);
        memcpy(HP(in), z, zn);
        memset(HP(strm), 0, 0x38);
        W32(strm + ZS_ZALLOC, HOST_ZALLOC);
        W32(strm + ZS_ZFREE, HOST_ZFREE);

        uint32_t a1[] = { strm, 15, version, 0x38 };
        int32_t r0 = (int32_t)cdecl_call(c, F_INFLATE_INIT2, 4, a1);
        W32(strm + ZS_NEXT_IN, in);   W32(strm + ZS_AVAIL_IN, (uint32_t)zn);
        W32(strm + ZS_NEXT_OUT, out); W32(strm + ZS_AVAIL_OUT, (uint32_t)en + 64);
        uint32_t a2[] = { strm, 4 /* Z_FINISH */ };
        int32_t r1 = (int32_t)cdecl_call(c, F_INFLATE, 2, a2);
        uint32_t total = R32(strm + ZS_TOTAL_OUT);
        uint32_t a3[] = { strm };
        cdecl_call(c, F_INFLATE_END, 1, a3);

        int ok = r0 == 0 && r1 == 1 /* Z_STREAM_END */ && total == (uint32_t)en && !memcmp(HP(out), expect, en);
        printf("%s %-40s in=%7ld out=%8u init=%d inflate=%d\n", ok ? "PASS" : "FAIL",
               strrchr(argv[i], '/') ? strrchr(argv[i], '/') + 1 : argv[i], zn, total, r0, r1);
        ok ? pass++ : fail++;
        free(z); free(expect);
    }
    printf("\n%d passed, %d failed\n", pass, fail);
    return fail != 0;
}
